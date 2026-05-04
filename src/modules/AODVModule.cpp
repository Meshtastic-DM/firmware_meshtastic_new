#include "AODVModule.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "SDNModule.h"
#include "configuration.h"
#include "mesh/RoutingMode.h"

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
    if (getRoutingMode() != RoutingMode::AODV) {
        LOG_DEBUG("AODV: Ignoring control packet while mode=%s", routingModeToString(getRoutingMode()));
        return false;
    }

    if (router) {
        router->learnRoutingCapableNode(getFrom(&mp), "AODV");
    }

    // Process based on message type
    switch (aodv->which_variant) {
    case meshtastic_AODV_rreq_tag:
        return handleRouteRequest(mp, aodv->variant.rreq);
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

    return false; // Allow other modules to see RREP/RERR packets
}

bool AODVModule::handleRouteRequest(const meshtastic_MeshPacket &mp, const meshtastic_RouteRequest &rreq)
{
    uint8_t hopCount = mp.hop_start - mp.hop_limit;
    

    LOG_INFO(
        "AODV: Received RREQ from=0x%x dest=0x%x ID=%u hops=%d hop_start=%d hop_limit=%d relay=0x%x next_hop=0x%x",
        rreq.originator,
        rreq.destination,
        rreq.rreq_id,
        hopCount,
        mp.hop_start,
        mp.hop_limit,
        mp.relay_node,
        mp.next_hop
    );


    // Ignore RREQs from ourselves
    if (rreq.originator == nodeDB->getNodeNum()) {
        LOG_DEBUG("AODV: Ignoring our own RREQ");
        return false;
    }

    // Update reverse route to originator (for RREP to travel back)
    // Previous hop = who relayed this packet to us
    uint8_t prevHop = mp.relay_node;                 // already last-byte
    if (prevHop == 0) prevHop = nodeDB->getLastByteOfNodeNum(mp.from); // fallback only
    
    routeTable.updateRoute(rreq.originator, prevHop, hopCount + 1, rreq.originator_seq_num);

    // Are we the destination?
    if (rreq.destination == nodeDB->getNodeNum()) {
        // Check if we've already seen this RREQ from this relay path
        if (hasSeenRREQ(rreq.originator, rreq.dest_seq_num, prevHop)) {
            LOG_DEBUG("AODV: Duplicate RREQ from same path, ignoring");
            return true; // Consume duplicate to prevent rebroadcast
        }
        
        // Mark as seen and get response seq num (increments only on first, reuses for alternate paths)
        uint32_t mySeqNum = markRREQAsSeen(rreq.originator, rreq.dest_seq_num, prevHop);
        
        LOG_INFO("AODV RREQ TARGET: from=0x%x, ID=%u, dest_seq=%u, hops=%d, response_seq=%u", 
                 rreq.originator, rreq.rreq_id, rreq.dest_seq_num, hopCount, mySeqNum);
        LOG_INFO("AODV RREP SEND: to=0x%x, dest=0x%x, seq=%u, hops=0, next_hop=0x%x", 
                 rreq.originator, rreq.destination, mySeqNum, prevHop);
        sendRREP(rreq.originator, rreq.destination, mySeqNum, 0, prevHop); // 0 hops to ourselves, next_hop is who sent us the RREQ
        // Consume packet to prevent rebroadcast - we are the target
        LOG_DEBUG("AODV: Target consuming RREQ to prevent rebroadcast id=0x%x", mp.id);
        if (service->api_state == MeshService::STATE_SERIAL) {
            service->handleFromRadio(&mp);
        }
        return true; // Stop processing - prevents RoutingModule from rebroadcasting
    }

    // Forward RREQ to all nodes that might have route to destination
    // (No intermediate RREP - let destination respond directly)
    LOG_INFO("AODV RREQ RBCAST: orig=0x%x, dest=0x%x, ID=%u, hops=%d (will be %d after forward)", 
             rreq.originator, rreq.destination, rreq.rreq_id, hopCount, hopCount + 1);
    return false; // Allow RoutingModule to rebroadcast
}

void AODVModule::handleRouteReply(const meshtastic_MeshPacket &mp, const meshtastic_RouteReply &rrep)
{   

    uint8_t hopStart = mp.hop_start;
    uint8_t hopLimit = mp.hop_limit;
    uint8_t hopCount = hopStart - hopLimit;

    LOG_INFO(
        "AODV: Received RREP dest=0x%x seq=%u hop_start=%u hop_limit=%u hops=%u relay=0x%x next_hop=0x%x",
        rrep.destination,
        rrep.dest_seq_num,
        hopStart,
        hopLimit,
        hopCount,
        mp.relay_node,
        mp.next_hop
    );


    const uint8_t me = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());

    if (mp.next_hop != NO_NEXT_HOP_PREFERENCE && mp.next_hop != me) {
        LOG_DEBUG(
            "AODV: Drop RREP not for me "
            "(id=0x%x next_hop=0x%x me=0x%x relay=0x%x from=0x%x to=0x%x)",
            mp.id, mp.next_hop, me, mp.relay_node, mp.from, mp.to
        );
        return;
    }

    LOG_INFO(
        "AODV: Accept RREP "
        "(id=0x%x dest=0x%x orig=0x%x seq=%u "
        "relay=0x%x from=0x%x next_hop=0x%x hops=%u)",
        mp.id,
        rrep.destination,
        rrep.originator,
        rrep.dest_seq_num,
        mp.relay_node,
        mp.from,
        mp.next_hop,
        hopCount
    );

    // Update forward route to destination
    // Previous hop = who relayed this packet to us
    uint8_t prevHop = mp.relay_node;
    if (prevHop == 0) 
        prevHop = nodeDB->getLastByteOfNodeNum(mp.from);

    LOG_INFO(
        "AODV: Update route "
        "(dest=0x%x via=0x%x hops=%u seq=%u)",
        rrep.destination,
        prevHop,
        hopCount + 1,
        rrep.dest_seq_num
    );
        
    routeTable.updateRoute(rrep.destination, prevHop, hopCount + 1, rrep.dest_seq_num);
    uint32_t precursor = rrep.originator;
    if (precursor == nodeDB->getNodeNum()) {
        precursor = 0;
    }
    routeTable.addPrecursor(rrep.destination, precursor);
    
    // Send route update to SDN controller if available
    if (sdnModule && (sdnModule->isControllerAuthenticated() || sdnModule->isController())) {
        sdnModule->sendRouteUpdate(rrep.destination, prevHop, hopCount + 1, rrep.dest_seq_num);
    }

    AODVRouteEntry *rt = routeTable.findRoute(rrep.destination);
    if (rt) {
        LOG_INFO(
            "AODV: Route installed "
            "(dest=0x%x next_hop=0x%x hops=%u seq=%u expires_in=%ds)",
            rrep.destination,
            rt->nextHop,
            rt->hopCount,
            rt->destSeqNum,
            (rt->expiryTime - millis()) / 1000
        );
    } else {
        LOG_ERROR("AODV: Route update FAILED for dest=0x%x", rrep.destination);
    }

    // Are we the originator who requested this route?
    if (rrep.originator == nodeDB->getNodeNum()) {
        LOG_INFO("AODV RREP RECV: dest=0x%x, seq=%u, hops=%d, route_established via 0x%x", 
                 rrep.destination, rrep.dest_seq_num, hopCount, prevHop);
        
        // Remove pending RREQ
        routeTable.removePendingRREQ(rrep.destination);
        
        // Deliver any buffered packets
        deliverBufferedPackets(rrep.destination);
        return;
    }

    // Forwarding is handled by NextHopRouter for all next-hop packets, including AODV control.
    LOG_INFO("AODV RREP ROUTER_FWD: to=0x%x, dest=0x%x, seq=%u, hops=%d", 
             rrep.originator, rrep.destination, rrep.dest_seq_num, hopCount);
}

void AODVModule::handleRouteError(const meshtastic_MeshPacket &mp, const meshtastic_RouteError &rerr)
{
    LOG_INFO(
        "AODV: Received RERR "
        "(count=%d from=0x%x to=0x%x relay=0x%x next_hop=0x%x id=0x%x)",
        rerr.unreachable_destinations_count,
        mp.from,
        mp.to,
        mp.relay_node,
        mp.next_hop,
        mp.id
    );

    const uint8_t me = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());

    if (mp.to != NODENUM_BROADCAST && mp.next_hop != me) {
        LOG_DEBUG(
            "AODV: Drop RRER unicast not for me "
            "(id=0x%x next_hop=0x%x me=0x%x relay=0x%x from=0x%x to=0x%x)",
            mp.id, mp.next_hop, me, mp.relay_node, mp.from, mp.to
        );
        return;
    }

    if (rerr.unreachable_destinations_count == 0) return;

    uint32_t target = rerr.unreachable_destinations[0].node_num;
    uint32_t seq = rerr.unreachable_destinations[0].seq_num;

    LOG_INFO("AODV: RERR target=0x%x seq=%u relay=0x%x", target, seq, mp.relay_node);

    // Invalidate path via the relay node that sent this RERR
    uint8_t relayHop = mp.relay_node;
    if (relayHop == 0) {
        relayHop = nodeDB->getLastByteOfNodeNum(mp.from);
    }
    
    LOG_INFO("AODV: RERR => invalidate path to 0x%x via 0x%x", target, relayHop);
    routeTable.invalidateRoutePath(target, relayHop);
}

void AODVModule::initiateRouteDiscovery(uint32_t destination, meshtastic_MeshPacket *packet)
{
    if (getRoutingMode() != RoutingMode::AODV) {
        LOG_INFO("AODV: Skipping route discovery for 0x%x while mode=%s",
                 destination, routingModeToString(getRoutingMode()));
        packetPool.release(packet);
        return;
    }

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
    // hop_count not used - we use MeshPacket hop_start/hop_limit instead

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
    p->hop_start = p->hop_limit;
    // Send
    LOG_INFO("AODV RREQ SEND: dest=0x%x, ID=%u, seq=%u, hop_limit=%d", destination, rreqId, mySeqNum, p->hop_limit);
    router->sendLocal(p);

    // Mark as pending and update rate limit
    routeTable.addPendingRREQ(destination, rreqId);
    routeTable.updateRREQRateLimit(destination);
}

void AODVModule::handleLinkFailure(uint32_t destination)
{
    if (getRoutingMode() != RoutingMode::AODV) {
        LOG_DEBUG("AODV: Ignoring link failure for 0x%x while mode=%s",
                  destination, routingModeToString(getRoutingMode()));
        return;
    }

    LOG_INFO("AODV: Link failure detected for 0x%x", destination);

    // Get precursors who need to be notified
    AODVRouteEntry *route = routeTable.findRoute(destination);
    if (!route) {
        LOG_DEBUG("AODV: No route to notify");
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

    if (route->precursor == NODENUM_BROADCAST) {
        LOG_INFO("AODV: Broadcast precursor for 0x%x; skipping RERR", destination);
    } else {
        // Unicast case: send only to precursor
        if (route->precursor != 0) {
            meshtastic_MeshPacket *p_precursor = router->allocForSending();
            p_precursor->to = route->precursor;
            p_precursor->decoded.portnum = meshtastic_PortNum_AODV_ROUTING_APP;
            p_precursor->channel = channels.getPrimaryIndex();
            p_precursor->want_ack = false;
            p_precursor->hop_limit = config.lora.hop_limit;
            p_precursor->decoded.payload.size =
                pb_encode_to_bytes(p_precursor->decoded.payload.bytes, sizeof(p_precursor->decoded.payload.bytes),
                                   &meshtastic_AODV_msg, &aodv);

            LOG_INFO("AODV: Sending RERR for 0x%x to precursor 0x%x", destination, route->precursor);
            router->sendLocal(p_precursor);
        } else {
            LOG_INFO("AODV: No precursor to notify for 0x%x", destination);
        }
    }

    // Invalidate the route after sending
    routeTable.invalidateRoute(destination);
}

bool AODVModule::hasSeenRREQ(uint32_t originator, uint32_t destSeqNum, uint8_t relayNode)
{
    auto originatorIt = seenRREQs.find(originator);
    if (originatorIt == seenRREQs.end()) {
        return false; // Never seen this originator
    }
    
    auto& destSeqMap = originatorIt->second;
    auto destSeqIt = destSeqMap.find(destSeqNum);
    if (destSeqIt == destSeqMap.end()) {
        return false; // Never seen this dest_seq_num from this originator
    }
    
    auto& relayList = destSeqIt->second.second; // second element of pair is the vector
    
    // Check if we've reached the limit for this dest_seq_num
    if (relayList.size() >= AODV_MAX_RREQ_PER_ORIGINATOR) {
        return true; // Limit reached, reject any further relay paths
    }
    
    // Check for exact match of relayNode
    for (const auto& relayEntry : relayList) {
        if (relayEntry.first == relayNode) {
            return true; // Already seen this exact relay path
        }
    }
    
    return false;
}

uint32_t AODVModule::markRREQAsSeen(uint32_t originator, uint32_t destSeqNum, uint8_t relayNode)
{
    auto& destSeqMap = seenRREQs[originator];
    
    uint32_t responseSeqNum;
    
    auto destSeqIt = destSeqMap.find(destSeqNum);
    if (destSeqIt == destSeqMap.end()) {
        // First time seeing this dest_seq_num - generate new response seq num
        responseSeqNum = routeTable.incrementMySeqNum();
        
        // Create new entry with response_seq_num and empty relay list
        std::vector<std::pair<uint8_t, uint32_t>> relayList;
        relayList.push_back(std::make_pair(relayNode, millis()));
        
        destSeqMap[destSeqNum] = std::make_pair(responseSeqNum, relayList);
        
        LOG_DEBUG("AODV: Generated new response_seq=%u for dest_seq=%u (first path)", 
                  responseSeqNum, destSeqNum);
    } else {
        // Already have entry for this dest_seq_num - reuse response_seq_num
        responseSeqNum = destSeqIt->second.first;
        auto& relayList = destSeqIt->second.second;
        
        // Add this relay path if we haven't reached the limit
        if (relayList.size() < AODV_MAX_RREQ_PER_ORIGINATOR) {
            relayList.push_back(std::make_pair(relayNode, millis()));
            LOG_DEBUG("AODV: Reusing response_seq=%u for dest_seq=%u (alternate path #%d)", 
                      responseSeqNum, destSeqNum, (int)relayList.size());
        } else {
            LOG_WARN("AODV: Max relay paths reached for dest_seq=%u from orig=0x%x", 
                     destSeqNum, originator);
        }
    }
    
    return responseSeqNum;
}

void AODVModule::sendRREP(uint32_t originator, uint32_t destination, uint32_t destSeqNum, uint8_t hopCount, uint8_t nextHop)
{
    meshtastic_RouteReply rrep = meshtastic_RouteReply_init_default;
    rrep.originator = originator;
    rrep.destination = destination;
    rrep.dest_seq_num = destSeqNum;
    // hop_count not used - we use MeshPacket hop_start/hop_limit instead
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
    p->next_hop = nextHop; // Set next hop to relay node from RREQ
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_AODV_msg, &aodv);

    LOG_INFO("AODV: Sending RREP to 0x%x for dest 0x%x, next_hop=0x%x", originator, destination, nextHop);
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
    
    // CRITICAL: Clear buffer BEFORE sending to transfer ownership.
    // After router->send(), router owns and will free the packets.
    // If we clear after, clearBufferedPackets() will double-free them.
    routeTable.clearBufferedPacketsWithoutFreeing(destination);
    
    for (auto packet : packets) {
        // Router takes ownership and will eventually free this packet
        router->send(packet);
    }
}

void AODVModule::cleanupSeenRREQs()
{
    uint32_t now = millis();
    
    for (auto originatorIt = seenRREQs.begin(); originatorIt != seenRREQs.end();) {
        auto& destSeqMap = originatorIt->second;
        
        // Clean up each dest_seq_num entry
        for (auto destSeqIt = destSeqMap.begin(); destSeqIt != destSeqMap.end();) {
            auto& relayList = destSeqIt->second.second;
            
            // Remove expired relay entries
            relayList.erase(
                std::remove_if(relayList.begin(), relayList.end(),
                    [now](const std::pair<uint8_t, uint32_t>& relayEntry) {
                        return now - relayEntry.second > AODV_NET_TRAVERSAL_TIME * 2;
                    }),
                relayList.end()
            );
            
            // Remove dest_seq_num entry if no relay paths remain
            if (relayList.empty()) {
                destSeqIt = destSeqMap.erase(destSeqIt);
            } else {
                ++destSeqIt;
            }
        }
        
        // Remove originator entry if no dest_seq_num entries remain
        if (destSeqMap.empty()) {
            originatorIt = seenRREQs.erase(originatorIt);
        } else {
            ++originatorIt;
        }
    }
}

void AODVModule::resetState()
{
    routeTable.reset();
    seenRREQs.clear();
    lastCleanupTime = 0;
    LOG_INFO("AODV: Module state reset");
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
