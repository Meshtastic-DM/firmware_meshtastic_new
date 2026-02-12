#include "SDNModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "RTC.h"
#include "configuration.h"
#include "mesh/TypeConversions.h"
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

struct __attribute__((packed)) SDNAnnMacData {
    uint32_t controller;   // full node id
    uint32_t seq;
    uint32_t timestamp;
    uint8_t  pubkey[32];
};

SDNModule::SDNModule()
    : ProtobufModule("sdn", meshtastic_PortNum_SDN_APP, &meshtastic_SDN_msg),
      concurrency::OSThread("SDNModule"),
      isSDNController(false),
      announcementInterval(300), // Default 5 minutes
      sdnControllerNode(0),
      sdnAuthenticated(false),
      announcementSeqNum(0),
      lastAnnouncementTime(0),
      hmacSecretLen(0)
{
    isPromiscuous = true; // Receive all SDN messages

    // TODO: Load config (secret, controller mode, interval) from config.sdn.
    // Example:
    // const char *secret = "meshtastic-sdn-secret";
    // hmacSecretLen = strlen(secret);
    // memcpy(hmacSecret, secret, hmacSecretLen);

    if (isSDNController) {
        setIntervalFromNow(announcementInterval * 1000);
        LOG_INFO("SDN: Initialized as controller, announcement interval=%us", announcementInterval);
    } else {
        LOG_INFO("SDN: Initialized as regular node");
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
    default:
        LOG_WARN("SDN: Unknown message variant");
        break;
    }

    return false;
}

void SDNModule::handleSDNAnnouncement(const meshtastic_MeshPacket &mp, const meshtastic_SDNAnnouncement &ann)
{
    const uint32_t controllerNode = mp.from;

    LOG_INFO("SDN: Received announcement from 0x%x, seq=%u, timestamp=%u",
             controllerNode, ann.sequence_num, ann.timestamp);

    // Public key must be present (we bind it into the HMAC)
    if (ann.public_key.size != 32) {
        LOG_WARN("SDN: Invalid public key size: %d (expected 32)", ann.public_key.size);
        return;
    }

    // Verify HMAC if secret configured
    if (hmacSecretLen > 0) {
        if (ann.hmac_hash.size != 16) {
            LOG_WARN("SDN: Invalid HMAC size: %d (expected 16)", ann.hmac_hash.size);
            return;
        }

        SDNAnnMacData mac;
        mac.controller = controllerNode;
        mac.seq = ann.sequence_num;
        mac.timestamp = ann.timestamp;
        memcpy(mac.pubkey, ann.public_key.bytes, 32);

        uint8_t expected[16];
        hmac_sha256_16(hmacSecret, hmacSecretLen,
                       (const uint8_t *)&mac, sizeof(mac),
                       expected);

        if (memcmp(expected, ann.hmac_hash.bytes, 16) != 0) {
            LOG_WARN("SDN: HMAC verification failed for controller 0x%x", controllerNode);
            sdnAuthenticated = false;
            return;
        }
    }

    // Anti-replay (single-controller assumption)
    if (ann.sequence_num <= g_lastAcceptedControllerSeq) {
        LOG_WARN("SDN: Replay/old announcement from 0x%x seq=%u last=%u",
                 controllerNode, ann.sequence_num, g_lastAcceptedControllerSeq);
        return;
    }
    g_lastAcceptedControllerSeq = ann.sequence_num;

    sdnAuthenticated = true;
    sdnControllerNode = controllerNode;

    // Store public key in local member
    memcpy(sdnPublicKey, ann.public_key.bytes, 32);
    
    // Store controller public key in NodeDB
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(controllerNode);
    if (node && node->has_user) {
        // Node exists with user info, update only if public key changed
        if (node->user.public_key.size != 32 || 
            memcmp(node->user.public_key.bytes, ann.public_key.bytes, 32) != 0) {
            
            meshtastic_User tempUser = TypeConversions::ConvertToUser(controllerNode, node->user);
            memcpy(tempUser.public_key.bytes, ann.public_key.bytes, 32);
            tempUser.public_key.size = 32;
            
            nodeDB->updateUser(controllerNode, tempUser, 0);
            LOG_INFO("SDN: Updated controller 0x%x public key in NodeDB", controllerNode);
        } else {
            LOG_DEBUG("SDN: Controller 0x%x public key already stored", controllerNode);
        }
    } else {
        // Node doesn't exist or has no user info, create minimal user entry with public key
        meshtastic_User tempUser = meshtastic_User_init_default;
        snprintf(tempUser.id, sizeof(tempUser.id), "!%08x", controllerNode);
        snprintf(tempUser.long_name, sizeof(tempUser.long_name), "SDN-%08x", controllerNode);
        snprintf(tempUser.short_name, sizeof(tempUser.short_name), "S%02x", controllerNode & 0xFF);
        memcpy(tempUser.public_key.bytes, ann.public_key.bytes, 32);
        tempUser.public_key.size = 32;
        
        nodeDB->updateUser(controllerNode, tempUser, 0);
        LOG_INFO("SDN: Created node entry and stored controller 0x%x public key in NodeDB", controllerNode);
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
    if (crypto->public_key[0] != 0) {
        memcpy(ann.public_key.bytes, crypto->public_key, 32);
        ann.public_key.size = 32;
    } else {
        LOG_WARN("SDN: No public key available, announcement will not include key");
        ann.public_key.size = 0;
    }
#else
    LOG_WARN("SDN: PKI excluded, announcement will not include public key");
    ann.public_key.size = 0;
#endif

    ann.sequence_num = ++announcementSeqNum;
    ann.timestamp = timestamp;

    // HMAC over controller_id + seq + timestamp + pubkey
    if (hmacSecretLen > 0) {
        if (ann.public_key.size != 32) {
            LOG_WARN("SDN: Cannot HMAC announcement without 32-byte public key");
            return;
        }

        SDNAnnMacData mac;
        mac.controller = nodeDB->getNodeNum();
        mac.seq = ann.sequence_num;
        mac.timestamp = ann.timestamp;
        memcpy(mac.pubkey, ann.public_key.bytes, 32);

        uint8_t tag16[16];
        hmac_sha256_16(hmacSecret, hmacSecretLen,
                       (const uint8_t *)&mac, sizeof(mac),
                       tag16);

        memcpy(ann.hmac_hash.bytes, tag16, 16);
        ann.hmac_hash.size = 16;
    }

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

    LOG_INFO("SDN: Sending announcement seq=%u, timestamp=%u", announcementSeqNum, timestamp);
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
    p->want_ack = true;
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
