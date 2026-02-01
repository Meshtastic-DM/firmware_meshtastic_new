#include "AODVModule.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "configuration.h"

AODVModule *aodvModule;

AODVModule::AODVModule()
    : ProtobufModule("aodv", meshtastic_PortNum_AODV_ROUTING_APP, &meshtastic_AODV_msg),
      concurrency::OSThread("AODVModule")
{
    isPromiscuous = true; // Process all AODV packets, not just those addressed to us
    encryptedOk = false;  // AODV packets are encrypted with channel PSK, but we process after decrypt
    lastCleanupTime = 0;

    // Start the cleanup thread
    setIntervalFromNow(AODV_ROUTE_CLEANUP_INTERVAL);
}

bool AODVModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_AODV *aodv)
{
    // Process based on message type
    switch (aodv->which_variant) {
    case meshtastic_AODV_rreq_tag:
        handleRouteRequest(mp, aodv->variant.rreq);
        break;
    case meshtastic_AODV_rrep_tag:
        handleRouteReply(mp, aodv->variant.rrep);
        break;
    case meshtastic_AODV_rerr_tag:
        handleRouteError(mp, aodv->variant.rerr);
        break;
    default:
        LOG_WARN("AODV: Unknown message variant");
        break;
    }

    return false; // Always allow other modules to see AODV packets
}

void AODVModule::handleRouteRequest(const meshtastic_MeshPacket &mp, const meshtastic_RouteRequest &rreq)
{
    LOG_INFO("AODV: Received RREQ from 0x%x for dest 0x%x, ID=%u, hops=%d", rreq.originator, rreq.destination, rreq.rreq_id,
             rreq.hop_count);

    // Ignore RREQs from ourselves
    if (rreq.originator == nodeDB->getNodeNum()) {
        LOG_DEBUG("AODV: Ignoring our own RREQ");
        return;
    }

    // Check if we've already seen this RREQ
    if (hasSeenRREQ(rreq.originator, rreq.rreq_id)) {
        LOG_DEBUG("AODV: Duplicate RREQ, ignoring");
        return;
    }

    // Mark as seen
    markRREQAsSeen(rreq.originator, rreq.rreq_id);

    // Update reverse route to originator (for RREP to travel back)
    // Previous hop = who relayed this packet to us
    uint8_t prevHop = mp.relay_node;                 // already last-byte
    if (prevHop == 0) prevHop = nodeDB->getLastByteOfNodeNum(mp.from); // fallback only
    
    routeTable.updateRoute(rreq.originator, prevHop, rreq.hop_count + 1, rreq.originator_seq_num);

    // Are we the destination?
    if (rreq.destination == nodeDB->getNodeNum()) {
        LOG_INFO("AODV: We are destination, sending RREP");
        // Increment our sequence number (destination always has fresh seq num)
        uint32_t mySeqNum = routeTable.incrementMySeqNum();
        sendRREP(rreq.originator, rreq.destination, mySeqNum, 0); // 0 hops to ourselves
        return;
    }

    // Do we have a route to the destination?
    AODVRouteEntry *route = routeTable.findRoute(rreq.destination);
    if (route && route->destSeqNum >= rreq.dest_seq_num) {
        LOG_INFO("AODV: Have route to dest, sending intermediate RREP");
        sendRREP(rreq.originator, rreq.destination, route->destSeqNum, route->hopCount);
        return;
    }

    // Forward RREQ if we're not the destination and don't have a route
    LOG_DEBUG("AODV: Forwarding RREQ");
    forwardRREQ(rreq, rreq.hop_count + 1);
}

void AODVModule::handleRouteReply(const meshtastic_MeshPacket &mp, const meshtastic_RouteReply &rrep)
{
    LOG_INFO("AODV: Received RREP for dest 0x%x, hops=%d, seq=%u", rrep.destination, rrep.hop_count, rrep.dest_seq_num);

    // Update forward route to destination
    // Previous hop = who relayed this packet to us
    uint8_t prevHop = mp.relay_node;
    if (prevHop == 0) prevHop = nodeDB->getLastByteOfNodeNum(mp.from);
    
    routeTable.updateRoute(rrep.destination, prevHop, rrep.hop_count + 1, rrep.dest_seq_num);

    // Are we the originator who requested this route?
    if (rrep.originator == nodeDB->getNodeNum()) {
        LOG_INFO("AODV: RREP reached originator, route established to 0x%x", rrep.destination);
        
        // Remove pending RREQ
        routeTable.removePendingRREQ(rrep.destination);
        
        // Deliver any buffered packets
        deliverBufferedPackets(rrep.destination);
        return;
    }

    // Forward RREP toward originator
    LOG_DEBUG("AODV: Forwarding RREP to originator 0x%x", rrep.originator);
    forwardRREP(rrep, rrep.originator);
}

void AODVModule::handleRouteError(const meshtastic_MeshPacket &mp, const meshtastic_RouteError &rerr)
{
    LOG_INFO("AODV: Received RERR with %d unreachable destinations", rerr.unreachable_destinations_count);

    bool affectsOurRoutes = false;

    // Invalidate routes to unreachable destinations
    for (size_t i = 0; i < rerr.unreachable_destinations_count; i++) {
        uint32_t unreachableDest = rerr.unreachable_destinations[i].node_num;
        
        // Only invalidate if we use the sender as next hop for this destination
        AODVRouteEntry *route = routeTable.findRoute(unreachableDest);
        if (route && route->nextHop == nodeDB->getLastByteOfNodeNum(mp.from)) {
            LOG_INFO("AODV: Invalidating route to 0x%x due to RERR", unreachableDest);
            routeTable.invalidateRoute(unreachableDest);
            affectsOurRoutes = true;
        }
    }

    // Forward RERR to precursors if it affects our routes
    if (affectsOurRoutes) {
        forwardRERR(rerr);
    }
}

void AODVModule::initiateRouteDiscovery(uint32_t destination, meshtastic_MeshPacket *packet)
{
    // Check if RREQ already pending
    if (routeTable.hasPendingRREQ(destination)) {
        LOG_DEBUG("AODV: RREQ already pending for 0x%x, buffering packet", destination);
        routeTable.bufferPacket(destination, packet);
        return;
    }

    // Check rate limiting
    if (!routeTable.canSendRREQ(destination)) {
        LOG_WARN("AODV: Rate limited RREQ for 0x%x, buffering packet", destination);
        routeTable.bufferPacket(destination, packet);
        return;
    }

    // Buffer the packet
    routeTable.bufferPacket(destination, packet);

    // Get last known sequence number for destination
    uint32_t destSeqNum = 0;
    AODVRouteEntry *oldRoute = routeTable.findRoute(destination);
    if (oldRoute) {
        destSeqNum = oldRoute->destSeqNum;
    }

    // Create and broadcast RREQ
    uint32_t rreqId = routeTable.getNextRREQId();
    uint32_t mySeqNum = routeTable.incrementMySeqNum();

    meshtastic_RouteRequest rreq = meshtastic_RouteRequest_init_default;
    rreq.rreq_id = rreqId;
    rreq.originator = nodeDB->getNodeNum();
    rreq.originator_seq_num = mySeqNum;
    rreq.destination = destination;
    rreq.dest_seq_num = destSeqNum;
    rreq.hop_count = 0;

    // Create AODV packet
    meshtastic_AODV aodv = meshtastic_AODV_init_default;
    aodv.which_variant = meshtastic_AODV_rreq_tag;
    aodv.variant.rreq = rreq;

    // Allocate mesh packet
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
    p->channel = channels.getPrimaryIndex(); // Use primary channel for AODV routing control packets
    p->want_ack = false;
    p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    // Send
    LOG_INFO("AODV: Initiating RREQ for dest 0x%x, ID=%u", destination, rreqId);
    router->sendLocal(p);

    // Mark as pending and update rate limit
    routeTable.addPendingRREQ(destination, rreqId);
    routeTable.updateRREQRateLimit(destination);
}

void AODVModule::handleLinkFailure(uint32_t destination)
{
    LOG_INFO("AODV: Link failure detected for 0x%x", destination);

    // Invalidate the route
    routeTable.invalidateRoute(destination);

    // Get precursors who need to be notified
    AODVRouteEntry *route = routeTable.findRoute(destination);
    if (!route || route->precursors.empty()) {
        LOG_DEBUG("AODV: No precursors to notify");
        return;
    }

    // Create RERR message
    meshtastic_RouteError rerr = meshtastic_RouteError_init_default;
    rerr.unreachable_destinations_count = 1;
    rerr.unreachable_destinations[0].node_num = destination;
    rerr.unreachable_destinations[0].seq_num = route->destSeqNum;

    meshtastic_AODV aodv = meshtastic_AODV_init_default;
    aodv.which_variant = meshtastic_AODV_rerr_tag;
    aodv.variant.rerr = rerr;

    // Send RERR to precursors (broadcast for simplicity, could unicast to each)
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
    p->channel = channels.getPrimaryIndex(); // Use primary channel for AODV routing control packets
    p->want_ack = false;
    p->hop_limit = config.lora.hop_limit;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    LOG_INFO("AODV: Sending RERR for 0x%x to precursors", destination);
    router->sendLocal(p);
}

bool AODVModule::hasSeenRREQ(uint32_t originator, uint32_t rreqId)
{
    auto key = std::make_pair(originator, rreqId);
    return seenRREQs.find(key) != seenRREQs.end();
}

void AODVModule::markRREQAsSeen(uint32_t originator, uint32_t rreqId)
{
    auto key = std::make_pair(originator, rreqId);
    seenRREQs[key] = millis();
}

void AODVModule::forwardRREQ(const meshtastic_RouteRequest &rreq, uint8_t hopCount)
{
    meshtastic_RouteRequest forwardedRreq = rreq;
    forwardedRreq.hop_count = hopCount;

    meshtastic_AODV aodv = meshtastic_AODV_init_default;
    aodv.which_variant = meshtastic_AODV_rreq_tag;
    aodv.variant.rreq = forwardedRreq;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
    p->channel = channels.getPrimaryIndex(); // Use primary channel for AODV routing control packets
    p->want_ack = false;
    p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    router->sendLocal(p);
}

void AODVModule::sendRREP(uint32_t originator, uint32_t destination, uint32_t destSeqNum, uint8_t hopCount)
{
    meshtastic_RouteReply rrep = meshtastic_RouteReply_init_default;
    rrep.originator = originator;
    rrep.destination = destination;
    rrep.dest_seq_num = destSeqNum;
    rrep.hop_count = hopCount;
    rrep.lifetime = AODV_ACTIVE_ROUTE_TIMEOUT / 1000; // Convert to seconds

    meshtastic_AODV aodv = meshtastic_AODV_init_default;
    aodv.which_variant = meshtastic_AODV_rrep_tag;
    aodv.variant.rrep = rrep;

    // RREP is unicast to originator using the reverse route
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = originator;
    p->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
    p->channel = channels.getPrimaryIndex(); // Use primary channel for AODV routing control packets
    p->want_ack = false;
    p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    LOG_INFO("AODV: Sending RREP to 0x%x for dest 0x%x", originator, destination);
    router->sendLocal(p);
}

void AODVModule::forwardRREP(const meshtastic_RouteReply &rrep, uint32_t originator)
{
    meshtastic_RouteReply forwardedRrep = rrep;
    forwardedRrep.hop_count += 1;

    meshtastic_AODV aodv = meshtastic_AODV_init_default;
    aodv.which_variant = meshtastic_AODV_rrep_tag;
    aodv.variant.rrep = forwardedRrep;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = originator;
    p->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
    p->channel = channels.getPrimaryIndex(); // Use primary channel for AODV routing control packets
    p->want_ack = false;
    p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    router->sendLocal(p);
}

void AODVModule::forwardRERR(const meshtastic_RouteError &rerr)
{
    meshtastic_AODV aodv = meshtastic_AODV_init_default;
    aodv.which_variant = meshtastic_AODV_rerr_tag;
    aodv.variant.rerr = rerr;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
    p->channel = channels.getPrimaryIndex(); // Use primary channel for AODV routing control packets
    p->want_ack = false;
    p->hop_limit = config.lora.hop_limit;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    router->sendLocal(p);
}

void AODVModule::deliverBufferedPackets(uint32_t destination)
{
    auto packets = routeTable.getBufferedPackets(destination);
    
    if (packets.empty()) {
        LOG_DEBUG("AODV: No buffered packets for 0x%x", destination);
        return;
    }

    LOG_INFO("AODV: Delivering %d buffered packets to 0x%x", packets.size(), destination);
    
    for (auto packet : packets) {
        // Re-send packet now that we have a route
        router->send(packet);
    }

    // Clear the buffer (packets already sent)
    routeTable.clearBufferedPackets(destination);
}

void AODVModule::cleanupSeenRREQs()
{
    uint32_t now = millis();
    for (auto it = seenRREQs.begin(); it != seenRREQs.end();) {
        // Remove RREQs older than NET_TRAVERSAL_TIME * 2
        if (now - it->second > AODV_NET_TRAVERSAL_TIME * 2) {
            it = seenRREQs.erase(it);
        } else {
            ++it;
        }
    }
}

meshtastic_MeshPacket *AODVModule::allocReply()
{
    // AODV doesn't use the reply mechanism
    return nullptr;
}

int32_t AODVModule::runOnce()
{
    // Periodic cleanup
    routeTable.cleanup();
    cleanupSeenRREQs();

    return AODV_ROUTE_CLEANUP_INTERVAL; // Run every 5 seconds
}
