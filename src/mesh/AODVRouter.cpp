#include "AODVRouter.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "configuration.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include "modules/RoutingModule.h"

AODVRouter::AODVRouter() 
    : FloodingRouter(), sequenceNumber(1), rreqIdCounter(1), aodvEnabled(true), lastRoutingTablePrint(0)
{
    LOG_INFO("AODV Router initialized");
    
    // HYBRID ROUTING: Enable AODV for ALL roles
    // - UNICAST messages (direct messaging) use AODV route discovery
    // - BROADCAST messages use traditional flooding
    // This gives best of both worlds: efficient unicast + reliable broadcast
    
    auto role = config.device.role;
    
    // Always enable AODV - it's smart enough to use flooding when appropriate
    aodvEnabled = true;
    
    if (role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
        role == meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT ||
        role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE ||
        role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
        LOG_INFO("AODV enabled for ROUTER role %d - full routing capability", role);
    } else {
        LOG_INFO("AODV enabled for CLIENT role %d - hybrid mode (AODV unicast + flooding broadcast)", role);
    }
}

/**
 * Send a packet using AODV routing
 * HYBRID APPROACH:
 * - Broadcast packets: Use flooding (reliable for group messages)
 * - Unicast packets: Use AODV route discovery (efficient for direct messages)
 */
ErrorCode AODVRouter::send(meshtastic_MeshPacket *p)
{
    // If AODV is disabled, use pure flooding
    if (!aodvEnabled) {
        return FloodingRouter::send(p);
    }
    
    // ALWAYS use flooding for broadcast packets
    // Reason: Broadcasts need to reach everyone, flooding is more reliable
    if (p->to == NODENUM_BROADCAST || p->to == 0) {
        LOG_DEBUG("AODV: Broadcast packet, using flooding");
        return FloodingRouter::send(p);
    }

    // For UNICAST packets (direct messages), use AODV routing
    LOG_DEBUG("AODV: Unicast packet to 0x%x, attempting route discovery", p->to);
    
    // Find route to destination
    AODVRouteEntry *route = findRoute(p->to);
    
    if (route && route->routeValid) {
        // We have a valid route, use it
        LOG_INFO("AODV: Valid route found to 0x%x via 0x%x (hops: %d)", 
                  p->to, route->nextHop, route->hopCount);
        route->lastUsedTime = millis();
        return forwardWithRoute(p, route);
    } else {
        // No valid route, initiate route discovery
        LOG_INFO("AODV: No route to 0x%x, initiating route discovery", p->to);
        initiateRouteDiscovery(p);
        return ERRNO_OK; // Packet will be sent when route is discovered
    }
}

/**
 * Forward packet using discovered route
 */
ErrorCode AODVRouter::forwardWithRoute(meshtastic_MeshPacket *p, const AODVRouteEntry *route)
{
    // Set next hop preference
    p->next_hop = route->nextHop;
    
    // Update hop limit based on known hop count
    if (p->hop_limit == 0 || p->hop_limit > route->hopCount + 2) {
        p->hop_limit = route->hopCount + 2; // Add margin
    }
    
    // Send via base router
    return FloodingRouter::send(p);
}

/**
 * Initiate route discovery for a destination
 */
void AODVRouter::initiateRouteDiscovery(meshtastic_MeshPacket *p)
{
    NodeNum destination = p->to;
    
    // Check if we already have a pending RREQ for this destination
    auto it = pendingRREQs.find(destination);
    if (it != pendingRREQs.end()) {
        LOG_DEBUG("AODV: RREQ already pending for 0x%x, buffering packet", destination);
        // Buffer this packet too (free old one if exists)
        if (it->second.bufferedPacket) {
            packetPool.release(it->second.bufferedPacket);
        }
        it->second.bufferedPacket = p;
        return;
    }
    
    // Create new pending RREQ
    PendingRREQ pending;
    pending.destination = destination;
    pending.rreqId = getNextRREQId();
    pending.ttl = 3; // Start with TTL of 3
    pending.retryCount = 0;
    pending.nextRetryTime = millis() + AODV_NET_TRAVERSAL_TIME;  //need to change according to maximum time takes to go through the network in worst case scenario
    pending.bufferedPacket = p;
    
    pendingRREQs[destination] = pending;
    
    // Send RREQ
    sendRREQ(destination, pending.ttl);
}

/**
 * Send Route Request (RREQ)
 */
void AODVRouter::sendRREQ(NodeNum destination, uint8_t ttl)
{
    LOG_INFO("AODV: Sending RREQ for dest 0x%x with TTL %d", destination, ttl);
    
    auto it = pendingRREQs.find(destination);
    if (it == pendingRREQs.end()) {
        LOG_WARN("AODV: No pending RREQ found for 0x%x", destination);
        return;
    }
    
    // Get destination sequence number if we have it
    uint32_t destSeqNum = 0;
    bool hasDestSeqNum = false;
    AODVRouteEntry *oldRoute = findRoute(destination);
    if (oldRoute && oldRoute->validDestSeqNum) {
        destSeqNum = oldRoute->destSeqNum;
        hasDestSeqNum = true;
    }
    
    // Create RREQ message
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    routing.which_variant = meshtastic_Routing_route_request_tag;
    
    meshtastic_RouteDiscovery *rreq = &routing.route_request;
    rreq->route_count = 0; // Empty route initially
    
    // We'll encode AODV-specific data in the route array as follows:
    // route[0] = RREQ ID
    // route[1] = Originator Sequence Number
    // route[2] = Destination Sequence Number (if valid)
    // route[3] = Hop Count (starts at 0)
    // route[4] = Flags (bit 0: has dest seq num)
    rreq->route[0] = it->second.rreqId;
    rreq->route[1] = getNextSequenceNumber();
    rreq->route[2] = destSeqNum;
    rreq->route[3] = 0; // Hop count starts at 0
    rreq->route[4] = hasDestSeqNum ? 1 : 0;
    rreq->route_count = 5;
    
    // Add to RREQ cache
    addRREQToCache(nodeDB->getNodeNum(), it->second.rreqId);
    
    // Send RREQ as broadcast
    sendAODVMessage(&routing, NODENUM_BROADCAST, ttl);
}

/**
 * Handle received RREQ
 */
void AODVRouter::handleRREQ(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing)
{
    const meshtastic_RouteDiscovery *rreq = &routing->route_request;
    
    if (rreq->route_count < 5) {
        LOG_WARN("AODV: Invalid RREQ format");
        return;
    }
    
    uint32_t rreqId = rreq->route[0];
    uint32_t origSeqNum = rreq->route[1];
    uint32_t destSeqNum = rreq->route[2];
    uint8_t hopCount = (uint8_t)rreq->route[3];
    bool hasDestSeqNum = rreq->route[4] != 0;
    
    NodeNum originator = getFrom(p);
    NodeNum destination = p->to;
    
    LOG_DEBUG("AODV: Received RREQ id=%u from 0x%x to 0x%x (hops=%d)", 
              rreqId, originator, destination, hopCount);
    
    // Check if we've already seen this RREQ
    if (isRREQCached(originator, rreqId)) {
        LOG_DEBUG("AODV: RREQ already processed, ignoring");
        return;
    }
    
    // Add to cache
    addRREQToCache(originator, rreqId);
    
    // Create/update reverse route to originator
    NodeNum prevHop = p->relay_node != 0 ? p->relay_node : originator;
    addRoute(originator, prevHop, hopCount + 1, origSeqNum, 
             millis() + AODV_ACTIVE_ROUTE_TIMEOUT);
    
    // Are we the destination?
    if (destination == nodeDB->getNodeNum()) {
        LOG_INFO("AODV: We are the destination, sending RREP");
        
        // Send RREP back to originator
        meshtastic_Routing replyRouting = meshtastic_Routing_init_zero;
        replyRouting.which_variant = meshtastic_Routing_route_reply_tag;
        
        meshtastic_RouteDiscovery *rrep = &replyRouting.route_reply;
        rrep->route[0] = rreqId;
        rrep->route[1] = getNextSequenceNumber(); // Our sequence number
        rrep->route[2] = 0; // Hop count from destination (starts at 0)
        rrep->route_count = 3;
        
        sendAODVMessage(&replyRouting, originator, hopCount + 2);
        return;
    }
    
    // Do we have a valid route to the destination?
    AODVRouteEntry *destRoute = findRoute(destination);
    if (destRoute && destRoute->routeValid && destRoute->validDestSeqNum) {
        // Check if our route is fresh enough
        if (!hasDestSeqNum || destRoute->destSeqNum >= destSeqNum) {
            LOG_INFO("AODV: Have valid route to dest, sending intermediate RREP");
            
            // Send RREP as intermediate node
            meshtastic_Routing replyRouting = meshtastic_Routing_init_zero;
            replyRouting.which_variant = meshtastic_Routing_route_reply_tag;
            
            meshtastic_RouteDiscovery *rrep = &replyRouting.route_reply;
            rrep->route[0] = rreqId;
            rrep->route[1] = destRoute->destSeqNum;
            rrep->route[2] = destRoute->hopCount;
            rrep->route_count = 3;
            
            sendAODVMessage(&replyRouting, originator, hopCount + 2);
            return;
        }
    }
    
    // Forward RREQ
    if (p->hop_limit > 1) {
        LOG_DEBUG("AODV: Forwarding RREQ");
        
        meshtastic_Routing fwdRouting = meshtastic_Routing_init_zero;
        fwdRouting.which_variant = meshtastic_Routing_route_request_tag;
        
        meshtastic_RouteDiscovery *fwdRreq = &fwdRouting.route_request;
        *fwdRreq = *rreq;
        fwdRreq->route[3] = hopCount + 1; // Increment hop count
        
        sendAODVMessage(&fwdRouting, NODENUM_BROADCAST, p->hop_limit - 1);
    }
}

/**
 * Handle received RREP
 */
void AODVRouter::handleRREP(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing)
{
    const meshtastic_RouteDiscovery *rrep = &routing->route_reply;
    
    if (rrep->route_count < 3) {
        LOG_WARN("AODV: Invalid RREP format");
        return;
    }
    
    uint32_t rreqId = rrep->route[0];
    uint32_t destSeqNum = rrep->route[1];
    uint8_t hopCount = (uint8_t)rrep->route[2];
    
    NodeNum destination = getFrom(p);
    NodeNum nextHop = p->relay_node != 0 ? p->relay_node : destination;
    
    LOG_INFO("AODV: Received RREP from 0x%x (hops=%d, seq=%u)", 
             destination, hopCount, destSeqNum);
    
    // Create/update forward route to destination
    addRoute(destination, nextHop, hopCount + 1, destSeqNum,
             millis() + AODV_ACTIVE_ROUTE_TIMEOUT);
    
    // Are we the originator of the RREQ?
    auto it = pendingRREQs.find(destination);
    if (it != pendingRREQs.end() && it->second.rreqId == rreqId) {
        LOG_INFO("AODV: Route discovery complete for 0x%x", destination);
        
        // Send buffered packet if any
        sendBufferedPacket(destination);
        
        // Remove from pending
        pendingRREQs.erase(it);
        return;
    }
    
    // Forward RREP towards originator if we're an intermediate node
    if (p->to != nodeDB->getNodeNum() && p->hop_limit > 1) {
        NodeNum rrep_dest = p->to;
        AODVRouteEntry *route = findRoute(rrep_dest);
        
        if (route && route->routeValid) {
            LOG_DEBUG("AODV: Forwarding RREP to 0x%x via 0x%x", rrep_dest, route->nextHop);
            
            meshtastic_Routing fwdRouting = meshtastic_Routing_init_zero;
            fwdRouting.which_variant = meshtastic_Routing_route_reply_tag;
            fwdRouting.route_reply = *rrep;
            fwdRouting.route_reply.route[2] = hopCount + 1;
            
            sendAODVMessage(&fwdRouting, rrep_dest, p->hop_limit - 1);
        }
    }
}

/**
 * Handle received RERR (Route Error)
 */
void AODVRouter::handleRERR(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing)
{
    // RERR uses error_reason variant, unreachable destinations in route array
    LOG_INFO("AODV: Received RERR");
    
    // In a full implementation, we would:
    // 1. Parse unreachable destinations
    // 2. Invalidate routes to those destinations
    // 3. Propagate RERR to precursors
    // For now, this is a simplified version
}

/**
 * Process packets for AODV routing updates
 */
void AODVRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    // Handle AODV routing messages
    if (c && p->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        if (c->which_variant == meshtastic_Routing_route_request_tag) {
            handleRREQ(p, c);
            return; // Don't pass to parent, this is AODV control traffic
        } else if (c->which_variant == meshtastic_Routing_route_reply_tag) {
            handleRREP(p, c);
            return;
        } else if (c->which_variant == meshtastic_Routing_error_reason_tag) {
            handleRERR(p, c);
            return;
        }
    }
    
    // Call parent implementation
    FloodingRouter::sniffReceived(p, c);
}

/**
 * Check if packet should be filtered
 */
bool AODVRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    // Let AODV control messages through
    if (p->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        return false;
    }
    
    return FloodingRouter::shouldFilterReceived(p);
}

/**
 * Check if we should rebroadcast
 */
bool AODVRouter::perhapsRebroadcast(const meshtastic_MeshPacket *p)
{
    // Don't rebroadcast packets from/to us
    if (isToUs(p) || isFromUs(p)) {
        return false;
    }
    
    // Don't rebroadcast if hop limit reached
    if (p->hop_limit == 0) {
        return false;
    }
    
    // Don't rebroadcast 0 id broadcasts
    if (p->id == 0) {
        LOG_DEBUG("Ignore 0 id broadcast");
        return false;
    }
    
    // Check if we should rebroadcast based on role
    if (!isRebroadcaster()) {
        LOG_DEBUG("No rebroadcast: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
        return false;
    }
    
    // For AODV-routed unicast messages with next_hop set, only rebroadcast if we're the next hop
    if (p->to != NODENUM_BROADCAST && p->to != 0 && p->next_hop != NO_NEXT_HOP_PREFERENCE) {
        uint8_t ourLastByte = nodeDB->getLastByteOfNodeNum(getNodeNum());
        if (p->next_hop != ourLastByte) {
            // Not intended for us to relay
            return false;
        }
    }
    
    // Rebroadcast the packet
    meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);
    LOG_INFO("AODV rebroadcast message from 0x%x to 0x%x", p->from, p->to);
    
    // Decrement hop limit if needed
    if (shouldDecrementHopLimit(p)) {
        tosend->hop_limit--;
    } else {
        LOG_INFO("Preserving hop_limit for ROUTER/CLIENT_BASE rebroadcast");
    }
    
    // Send the packet
    FloodingRouter::send(tosend);
    
    return true;
}

/**
 * Periodic maintenance
 */
int32_t AODVRouter::runOnce()
{
    uint32_t now = millis();
    
    // Process pending RREQs (retries and timeouts)
    processPendingRREQs();
    
    // Clean up expired routes
    deleteExpiredRoutes();
    
    // Clean up old RREQ cache entries
    cleanRREQCache();
    
    // Print routing table every 60 seconds (1 minute)
    if (now - lastRoutingTablePrint > 60000) {
        printRoutingTable();
        lastRoutingTablePrint = now;
    }
    
    // Call parent
    return FloodingRouter::runOnce();
}

/**
 * Find route to destination
 */
AODVRouteEntry* AODVRouter::findRoute(NodeNum destination)
{
    auto it = routingTable.find(destination);
    if (it != routingTable.end()) {
        return &it->second;
    }
    return nullptr;
}

/**
 * Add new route
 */
void AODVRouter::addRoute(NodeNum destination, NodeNum nextHop, uint8_t hopCount,
                          uint32_t destSeqNum, uint32_t lifetime)
{
    AODVRouteEntry &entry = routingTable[destination];
    
    // Only update if this is fresher or same freshness but shorter
    if (!entry.validDestSeqNum || 
        destSeqNum > entry.destSeqNum ||
        (destSeqNum == entry.destSeqNum && hopCount < entry.hopCount)) {
        
        entry.destination = destination;
        entry.destSeqNum = destSeqNum;
        entry.validDestSeqNum = true;
        entry.hopCount = hopCount;
        entry.nextHop = nextHop;
        entry.lifetime = lifetime;
        entry.lastUsedTime = millis();
        entry.routeValid = true;
        
        LOG_DEBUG("AODV: Route added/updated: dest=0x%x, next=0x%x, hops=%d, seq=%u",
                  destination, nextHop, hopCount, destSeqNum);
    }
}

/**
 * Update existing route
 */
void AODVRouter::updateRoute(NodeNum destination, NodeNum nextHop, uint8_t hopCount,
                             uint32_t destSeqNum, uint32_t lifetime)
{
    addRoute(destination, nextHop, hopCount, destSeqNum, lifetime);
}

/**
 * Invalidate route
 */
void AODVRouter::invalidateRoute(NodeNum destination)
{
    auto it = routingTable.find(destination);
    if (it != routingTable.end()) {
        it->second.routeValid = false;
        LOG_DEBUG("AODV: Route invalidated for 0x%x", destination);
    }
}

/**
 * Delete expired routes
 */
void AODVRouter::deleteExpiredRoutes()
{
    uint32_t now = millis();
    
    for (auto it = routingTable.begin(); it != routingTable.end(); ) {
        if (it->second.routeValid && now > it->second.lifetime) {
            LOG_DEBUG("AODV: Route to 0x%x expired", it->first);
            it = routingTable.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Check if RREQ is in cache
 */
bool AODVRouter::isRREQCached(NodeNum originator, uint32_t rreqId)
{
    RREQCacheKey key{originator, rreqId};
    return rreqCache.find(key) != rreqCache.end();
}

/**
 * Add RREQ to cache
 */
void AODVRouter::addRREQToCache(NodeNum originator, uint32_t rreqId)
{
    RREQCacheKey key{originator, rreqId};
    rreqCache[key] = RREQCacheEntry(originator, rreqId, millis());
}

/**
 * Clean old RREQ cache entries
 */
void AODVRouter::cleanRREQCache()
{
    uint32_t now = millis();
    uint32_t timeout = AODV_NET_TRAVERSAL_TIME * 2;
    
    for (auto it = rreqCache.begin(); it != rreqCache.end(); ) {
        if (now - it->second.timestamp > timeout) {
            it = rreqCache.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Process pending RREQs (retries and timeouts)
 */
void AODVRouter::processPendingRREQs()
{
    uint32_t now = millis();
    
    for (auto it = pendingRREQs.begin(); it != pendingRREQs.end(); ) {
        if (now >= it->second.nextRetryTime) {
            if (it->second.retryCount < AODV_RREQ_RETRIES) {
                // Retry with expanded TTL
                it->second.retryCount++;
                uint8_t newTtl = it->second.ttl * 2;
                it->second.ttl = (newTtl < HOP_MAX) ? newTtl : (uint8_t)HOP_MAX;
                it->second.nextRetryTime = now + AODV_NET_TRAVERSAL_TIME;
                
                LOG_INFO("AODV: Retrying RREQ for 0x%x (retry %d, TTL %d)",
                         it->second.destination, it->second.retryCount, it->second.ttl);
                
                sendRREQ(it->second.destination, it->second.ttl);
                ++it;
            } else {
                // Give up, free buffered packet
                LOG_WARN("AODV: Route discovery failed for 0x%x", it->second.destination);
                
                if (it->second.bufferedPacket) {
                    packetPool.release(it->second.bufferedPacket);
                }
                
                it = pendingRREQs.erase(it);
            }
        } else {
            ++it;
        }
    }
}

/**
 * Send buffered packet after route discovery
 */
void AODVRouter::sendBufferedPacket(NodeNum destination)
{
    auto it = pendingRREQs.find(destination);
    if (it != pendingRREQs.end() && it->second.bufferedPacket) {
        LOG_INFO("AODV: Sending buffered packet to 0x%x", destination);
        
        meshtastic_MeshPacket *p = it->second.bufferedPacket;
        it->second.bufferedPacket = nullptr; // Clear reference before sending
        
        // Now send the packet (will use the newly discovered route)
        send(p);
    }
}

/**
 * Get next RREQ ID
 */
uint32_t AODVRouter::getNextRREQId()
{
    return rreqIdCounter++;
}

/**
 * Get next sequence number
 */
uint32_t AODVRouter::getNextSequenceNumber()
{
    return sequenceNumber++;
}

/**
 * Send AODV control message
 */
void AODVRouter::sendAODVMessage(const meshtastic_Routing *routing, NodeNum to, uint8_t hopLimit)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_ERROR("AODV: Failed to allocate packet");
        return;
    }
    
    p->to = to;
    p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p->decoded.want_response = false;
    p->want_ack = false;
    p->hop_limit = hopLimit;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    
    // Encode routing message
    p->decoded.payload.size = pb_encode_to_bytes(
        p->decoded.payload.bytes, 
        sizeof(p->decoded.payload.bytes),
        &meshtastic_Routing_msg, 
        routing
    );
    
    if (p->decoded.payload.size == 0) {
        LOG_ERROR("AODV: Failed to encode routing message");
        packetPool.release(p);
        return;
    }
    
    // Send via base class (don't use our send() to avoid recursion)
    FloodingRouter::send(p);
}

/**
 * Print routing table for debugging
 */
void AODVRouter::printRoutingTable()
{
    uint32_t now = millis();
    int totalRoutes = routingTable.size();
    int validRoutes = 0;
    int expiredRoutes = 0;
    
    LOG_INFO("=== AODV Routing Table ===");
    LOG_INFO("AODV Status: %s", aodvEnabled ? "ENABLED" : "DISABLED");
    LOG_INFO("Total Routes: %d", totalRoutes);
    
    if (totalRoutes == 0) {
        LOG_INFO("No routes in table");
        LOG_INFO("==========================");
        return;
    }
    
    LOG_INFO("%-12s %-12s %-8s %-8s %-8s %-12s", 
             "Destination", "NextHop", "HopCount", "SeqNum", "Valid", "Lifetime(s)");
    LOG_INFO("------------------------------------------------------------------------");
    
    for (auto &entry : routingTable) {
        NodeNum dest = entry.first;
        AODVRouteEntry &route = entry.second;
        
        bool isValid = route.routeValid && (route.lifetime == 0 || route.lifetime > now);
        if (isValid) {
            validRoutes++;
        } else {
            expiredRoutes++;
        }
        
        int remainingTime = 0;
        if (route.lifetime > now) {
            remainingTime = (route.lifetime - now) / 1000;
        }
        
        LOG_INFO("0x%08x   0x%08x   %-8d %-8d %-8s %-12d", 
                 dest,
                 route.nextHop,
                 route.hopCount,
                 route.destSeqNum,
                 isValid ? "YES" : "NO",
                 remainingTime);
    }
    
    LOG_INFO("------------------------------------------------------------------------");
    LOG_INFO("Summary: Valid=%d, Expired=%d, Total=%d", validRoutes, expiredRoutes, totalRoutes);
    LOG_INFO("==========================");
}
