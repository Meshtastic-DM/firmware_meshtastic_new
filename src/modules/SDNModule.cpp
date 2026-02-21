#include "SDNModule.h"
#include "AODVModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "RTC.h"
#include "configuration.h"
#include <pb_encode.h>
#include <SHA256.h>

SDNModule *sdnModule;

/*
 * NOTE:
 * - Replaced the insecure "XOR + hash" with real HMAC-SHA256 (truncated to 16 bytes).
 * - HMAC covers: controller_id + seq + timestamp + public_key (prevents spoof + binds key).
 * - Added anti-replay using sequence number (single controller assumption).
 */

static uint32_t g_lastAcceptedControllerSeq = 0;
static uint32_t g_sdnAnnouncementSeqFallback = 0;

static void sha256_once(const uint8_t *data, size_t len, uint8_t out[32])
{
    SHA256 h;
    h.reset();
    h.update(data, len);
    h.finalize(out, 32);
}

static void hmac_sha256(const uint8_t *key, size_t keyLen,
                        const uint8_t *msg, size_t msgLen,
                        uint8_t out[32])
{
    // HMAC-SHA256: SHA256((K0 xor opad) || SHA256((K0 xor ipad) || msg))
    uint8_t k0[64];
    memset(k0, 0, sizeof(k0));

    if (keyLen > 64) {
        uint8_t kh[32];
        sha256_once(key, keyLen, kh);
        memcpy(k0, kh, 32);
    } else {
        memcpy(k0, key, keyLen);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }

    uint8_t innerHash[32];
    {
        SHA256 inner;
        inner.reset();
        inner.update(ipad, 64);
        inner.update(msg, msgLen);
        inner.finalize(innerHash, 32);
    }

    {
        SHA256 outer;
        outer.reset();
        outer.update(opad, 64);
        outer.update(innerHash, 32);
        outer.finalize(out, 32);
    }
}

static void hmac_sha256_16(const uint8_t *key, size_t keyLen,
                           const uint8_t *msg, size_t msgLen,
                           uint8_t out16[16])
{
    uint8_t full[32];
    hmac_sha256(key, keyLen, msg, msgLen, full);
    memcpy(out16, full, 16); // truncate
}

static inline void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v);
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
}

static void buildAnnMacMsg(uint32_t controller, uint32_t seq, uint32_t ts,
                           const uint8_t pubkey[32], uint8_t out[4 + 4 + 4 + 32])
{
    put_u32_le(out + 0, controller);
    put_u32_le(out + 4, seq);
    put_u32_le(out + 8, ts);
    memcpy(out + 12, pubkey, 32);
}

SDNModule::SDNModule()
    : ProtobufModule("sdn", meshtastic_PortNum_SDN_APP, &meshtastic_SDN_msg),
      concurrency::OSThread("SDNModule"),
      isSDNController(false),
      announcementInterval(300), // Default 5 minutes
      sdnControllerNode(0),
      sdnAuthenticated(false),
      lastAnnouncementTime(0),
      hmacSecretLen(0)
{
    isPromiscuous = true; // Receive all SDN messages

    // Test-only configuration until SDN config is added to module/local config protobufs.
    static constexpr uint32_t kTestControllerNode = 0x00000010;
    static constexpr uint32_t kTestAnnouncementIntervalSec = 60;
    static constexpr const char *kTestSecret = "meshtastic-sdn-secret";

    announcementInterval = kTestAnnouncementIntervalSec;

    size_t secretLen = strlen(kTestSecret);
    if (secretLen > sizeof(hmacSecret)) {
        secretLen = sizeof(hmacSecret);
    }
    memcpy(hmacSecret, kTestSecret, secretLen);
    hmacSecretLen = secretLen;

    const uint32_t myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    isSDNController = (myNode == kTestControllerNode);

    if (isSDNController) {
        setIntervalFromNow(announcementInterval * 1000);
        LOG_INFO("SDN: Test mode controller enabled (node=0x%08x, interval=%us)", myNode, announcementInterval);
    } else {
        LOG_INFO("SDN: Test mode regular node (node=0x%08x)", myNode);
    }
}

bool SDNModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_SDN *sdn)
{
    switch (sdn->which_payload_variant) {
    case meshtastic_SDN_announcement_tag:
        handleSDNAnnouncement(mp, sdn->payload_variant.announcement);
        break;
    case meshtastic_SDN_route_update_tag:
        handleSDNRouteUpdate(mp, sdn->payload_variant.route_update);
        break;
    case meshtastic_SDN_route_command_tag:
        handleSDNRouteCommand(mp, sdn->payload_variant.route_command);
        break;
    default:
        LOG_WARN("SDN: Unknown message variant");
        break;
    }

    return false;
}

void SDNModule::handleSDNAnnouncement(const meshtastic_MeshPacket &mp, const meshtastic_SDNAnnouncement &ann)
{   
    // Ignore our own announcements
    if (mp.from == nodeDB->getNodeNum()) {
        LOG_DEBUG("SDN: Ignoring our own announcement (0x%08x)", mp.from);
        return;
    }
    
    const uint32_t controllerNode = mp.from;

    LOG_INFO("SDN: Received announcement from 0x%x, seq=%u, timestamp=%u",
             controllerNode, ann.sequence_num, ann.timestamp);

    // Public key must be present (we bind it into the HMAC)
    if (ann.public_key.size != 32) {
        LOG_WARN("SDN: Invalid public key size: %d (expected 32)", ann.public_key.size);
        return;
    }

    if (hmacSecretLen == 0) {
        LOG_WARN("SDN: No secret configured; refusing unauthenticated controller");
        return;
    }

    if (ann.hmac_hash.size != 16) {
        LOG_WARN("SDN: Invalid HMAC size: %d (expected 16)", ann.hmac_hash.size);
        return;
    }

    uint8_t macMsg[44];
    buildAnnMacMsg(controllerNode, ann.sequence_num, ann.timestamp, ann.public_key.bytes, macMsg);

    uint8_t expected[16];
    hmac_sha256_16(hmacSecret, hmacSecretLen, macMsg, sizeof(macMsg), expected);

    if (memcmp(expected, ann.hmac_hash.bytes, 16) != 0) {
        LOG_WARN("SDN: HMAC verification failed for controller 0x%x", controllerNode);
        sdnAuthenticated = false;
        return;
    }

    // Anti-replay (single-controller assumption)
    if (ann.sequence_num <= g_lastAcceptedControllerSeq) {
        LOG_WARN("SDN: Replay/old announcement from 0x%x seq=%u last=%u",
                 controllerNode, ann.sequence_num, g_lastAcceptedControllerSeq);
        return;
    }

    uint32_t now = getTime();
    if (now != 0) {
        if (ann.timestamp > now + 60 || now - ann.timestamp > 600) {
            LOG_WARN("SDN: Announcement timestamp out of window (ann=%u now=%u)", ann.timestamp, now);
            return;
        }
    }
    g_lastAcceptedControllerSeq = ann.sequence_num;

    sdnAuthenticated = true;
    sdnControllerNode = controllerNode;

    // Install/update reverse route to authenticated controller (same pattern as AODV RREQ handling)
    if (aodvModule && aodvModule->getRouteTable()) {
        uint8_t hopCount = mp.hop_start - mp.hop_limit;
        uint8_t prevHop = mp.relay_node; // already last-byte
        if (prevHop == 0) {
            prevHop = nodeDB->getLastByteOfNodeNum(mp.from); // fallback only
        }

        aodvModule->getRouteTable()->updateRoute(controllerNode, prevHop, hopCount + 1, ann.sequence_num);
        LOG_INFO("SDN: Route to controller installed (dest=0x%x via=0x%x hops=%u seq=%u)",
                 controllerNode, prevHop, hopCount + 1, ann.sequence_num);
    } else {
        LOG_WARN("SDN: AODV unavailable, cannot install route to authenticated controller 0x%x", controllerNode);
    }

    // Store public key in local member
    memcpy(sdnPublicKey, ann.public_key.bytes, 32);
    
    // Store controller public key in NodeDB
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(controllerNode);
    if (node && node->has_user && node->user.public_key.size == 32 &&
        memcmp(node->user.public_key.bytes, ann.public_key.bytes, 32) == 0) {
        LOG_DEBUG("SDN: Controller key already stored");
    } else {
        meshtastic_User u = meshtastic_User_init_default;
        snprintf(u.long_name, sizeof(u.long_name), "SDN-%08x", controllerNode);
        snprintf(u.short_name, sizeof(u.short_name), "SDN");
        memcpy(u.public_key.bytes, ann.public_key.bytes, 32);
        u.public_key.size = 32;
        nodeDB->updateUser(controllerNode, u, 0);
        LOG_INFO("SDN: Stored controller 0x%x public key in NodeDB", controllerNode);
    }
    
    // Install controller's public key as admin key for remote administration
    installAdminKey();
}

void SDNModule::installAdminKey()
{
    bool needsSave = false;
    
    // Enable admin channel if disabled
    if (!config.security.admin_channel_enabled) {
        config.security.admin_channel_enabled = true;
        needsSave = true;
        LOG_INFO("SDN: Enabled admin channel for remote administration");
    }
    
    // Check if SDN key is already installed in slot 0
    if (config.security.admin_key[0].size == 32 &&
        memcmp(config.security.admin_key[0].bytes, sdnPublicKey, 32) == 0) {
        LOG_DEBUG("SDN: Controller admin key already installed in slot 0");
        return;
    }
    
    // Install SDN controller public key in admin_key[0] (reserved for SDN)
    memcpy(config.security.admin_key[0].bytes, sdnPublicKey, 32);
    config.security.admin_key[0].size = 32;
    needsSave = true;
    LOG_INFO("SDN: Installed controller public key in admin_key[0] for remote administration");
    
    // Save configuration changes
    if (needsSave && service) {
        service->reloadConfig(SEGMENT_CONFIG);
    }
}

void SDNModule::handleSDNRouteUpdate(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteUpdate &update)
{
    if (!isSDNController) {
        LOG_DEBUG("SDN: Received route update but not a controller, ignoring");
        return;
    }

    LOG_INFO("SDN: Route update from 0x%x: dest=0x%x, next_hop=0x%x, hops=%u, seq=%u",
             update.reporter_node, update.destination, update.next_hop,
             update.hop_count, update.dest_seq_num);

    // TODO: Add auth for route updates too (HMAC/signature) to prevent fake route injection.
    // For now, only announcements are authenticated.
}

void SDNModule::handleSDNRouteCommand(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteCommand &cmd)
{
    LOG_INFO("SDN: Received route command from 0x%x: dest=0x%x, next_hop=0x%x",
             mp.from, cmd.destination, cmd.next_hop);

    // Check if AODV module is available
    if (!aodvModule || !aodvModule->getRouteTable()) {
        LOG_WARN("SDN: Cannot process route command - AODV module unavailable");
        return;
    }

    // Attempt to activate the backup route
    bool success = aodvModule->getRouteTable()->activateBackupRoute(cmd.destination, (uint8_t)cmd.next_hop);
    
    if (success) {
        LOG_INFO("SDN: Successfully activated backup route for dest=0x%x via next_hop=0x%x",
                 cmd.destination, cmd.next_hop);
    } else {
        LOG_WARN("SDN: Failed to activate backup route for dest=0x%x via next_hop=0x%x",
                 cmd.destination, cmd.next_hop);
    }
}

void SDNModule::sendAnnouncement()
{
    if (!isSDNController) {
        return;
    }

    meshtastic_SDNAnnouncement ann = meshtastic_SDNAnnouncement_init_default;

    uint32_t timestamp = getTime();
    if (timestamp == 0) {
        timestamp = millis() / 1000;
    }

    // Add our public key first (we bind it into HMAC)
#if !(MESHTASTIC_EXCLUDE_PKI)
    // Get our own public key from NodeDB (canonical source after initialization)
    meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (ourNode && ourNode->has_user && ourNode->user.public_key.size == 32) {
        memcpy(ann.public_key.bytes, ourNode->user.public_key.bytes, 32);
        ann.public_key.size = 32;
    } 
    // Fallback: config.security.public_key (source of truth before NodeDB init completes)
    else if (config.has_security && config.security.public_key.size == 32) {
        memcpy(ann.public_key.bytes, config.security.public_key.bytes, 32);
        ann.public_key.size = 32;
        LOG_DEBUG("SDN: Using public key from config (NodeDB not yet populated)");
    } 
    else {
        LOG_WARN("SDN: No public key available (NodeDB or config). Not sending announcement.");
        return;
    }
#else
    LOG_WARN("SDN: PKI excluded. Not sending announcement.");
    return;
#endif

    if (aodvModule && aodvModule->getRouteTable()) {
        ann.sequence_num = aodvModule->getRouteTable()->incrementMySeqNum();
    } else {
        ann.sequence_num = ++g_sdnAnnouncementSeqFallback;
        LOG_WARN("SDN: AODV route table unavailable, using local fallback announcement sequence");
    }
    ann.timestamp = timestamp;

    if (hmacSecretLen == 0) {
        LOG_WARN("SDN: No secret configured; refusing to send unauthenticated announcement");
        return;
    }

    // HMAC over LE(controller_id + seq + timestamp + pubkey)
    if (ann.public_key.size != 32) {
        LOG_WARN("SDN: Cannot HMAC announcement without 32-byte public key");
        return;
    }

    uint8_t macMsg[44];
    buildAnnMacMsg(nodeDB->getNodeNum(), ann.sequence_num, ann.timestamp, ann.public_key.bytes, macMsg);

    uint8_t tag16[16];
    hmac_sha256_16(hmacSecret, hmacSecretLen, macMsg, sizeof(macMsg), tag16);

    memcpy(ann.hmac_hash.bytes, tag16, 16);
    ann.hmac_hash.size = 16;

    meshtastic_SDN sdn = meshtastic_SDN_init_default;
    sdn.which_payload_variant = meshtastic_SDN_announcement_tag;
    sdn.payload_variant.announcement = ann;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_SDN_APP;
    p->channel = channels.getPrimaryIndex();
    p->want_ack = false;
    p->hop_limit = 3;

    p->decoded.payload.size = pb_encode_to_bytes(
        p->decoded.payload.bytes,
        sizeof(p->decoded.payload.bytes),
        &meshtastic_SDN_msg,
        &sdn
    );

    LOG_INFO("SDN: Sending announcement seq=%u, timestamp=%u", ann.sequence_num, timestamp);
    router->sendLocal(p);

    lastAnnouncementTime = millis();
}

void SDNModule::sendRouteUpdate(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum)
{
    if (sdnControllerNode == 0) {
        LOG_DEBUG("SDN: No controller known, skipping route update");
        return;
    }

    if (!sdnAuthenticated) {
        LOG_DEBUG("SDN: Controller not authenticated, skipping route update");
        return;
    }

    meshtastic_SDNRouteUpdate update = meshtastic_SDNRouteUpdate_init_default;

    update.destination = destination;
    update.next_hop = nextHop;
    update.hop_count = hopCount;
    update.dest_seq_num = destSeqNum;
    update.reporter_node = nodeDB->getNodeNum();

    uint32_t timestamp = getTime();
    if (timestamp == 0) {
        timestamp = millis() / 1000;
    }
    update.timestamp = timestamp;

    meshtastic_SDN sdn = meshtastic_SDN_init_default;
    sdn.which_payload_variant = meshtastic_SDN_route_update_tag;
    sdn.payload_variant.route_update = update;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = sdnControllerNode;
    p->decoded.portnum = meshtastic_PortNum_SDN_APP;
    p->channel = channels.getPrimaryIndex();
    p->want_ack = false;
    p->hop_limit = config.lora.hop_limit;

    p->decoded.payload.size = pb_encode_to_bytes(
        p->decoded.payload.bytes,
        sizeof(p->decoded.payload.bytes),
        &meshtastic_SDN_msg,
        &sdn
    );

    LOG_INFO("SDN: Sending route update for dest=0x%x (next_hop=0x%x, hops=%u) to controller 0x%x",
             destination, nextHop, hopCount, sdnControllerNode);
    router->sendLocal(p);
}

void SDNModule::sendRouteCommand(uint32_t targetNode, uint32_t destination, uint8_t nextHop)
{
    if (targetNode == 0) {
        LOG_WARN("SDN: Invalid target node for route command");
        return;
    }

    meshtastic_SDNRouteCommand cmd = meshtastic_SDNRouteCommand_init_default;

    cmd.destination = destination;
    cmd.next_hop = nextHop;

    meshtastic_SDN sdn = meshtastic_SDN_init_default;
    sdn.which_payload_variant = meshtastic_SDN_route_command_tag;
    sdn.payload_variant.route_command = cmd;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = targetNode;
    p->decoded.portnum = meshtastic_PortNum_SDN_APP;
    p->channel = channels.getPrimaryIndex();
    p->want_ack = false;
    p->hop_limit = config.lora.hop_limit;

    p->decoded.payload.size = pb_encode_to_bytes(
        p->decoded.payload.bytes,
        sizeof(p->decoded.payload.bytes),
        &meshtastic_SDN_msg,
        &sdn
    );

    LOG_INFO("SDN: Sending route command to node 0x%x: activate dest=0x%x via next_hop=0x%x",
             targetNode, destination, nextHop);
    router->sendLocal(p);
}

int32_t SDNModule::runOnce()
{
    if (isSDNController) {
        uint32_t now = millis();
        uint32_t timeSinceLastAnnouncement = now - lastAnnouncementTime;

        if (timeSinceLastAnnouncement >= (announcementInterval * 1000)) {
            sendAnnouncement();
        }

        return (announcementInterval * 1000) - (millis() - lastAnnouncementTime);
    }

    return INT32_MAX;
}
