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

    /* ALWAYS use flooding for broadcast packets
        * Reason: Broadcasts need to reach everyone, flooding is more reliable
    */

    if (p->to == NODENUM_BROADCAST) {
        LOG_DEBUG("AODV: Broadcast packet, using flooding");
        return FloodingRouter::send(p);
    }

    /* 
        For UNICAST packets (direct messages), use AODV routing
    */
    LOG_INFO("AODV: Unicast packet to 0x%x, from 0x%x, id=%d", p->to, p->from, p->id);
    
    // Find route to destination
    AODVRouteEntry *route = findRoute(p->to);
    
    if (route && route->routeValid) {
        // We have a valid route, use it
        LOG_INFO("AODV: Valid route found to 0x%x via 0x%x (hops: %d)", 
                  p->to, route->nextHop, route->hopCount);
        route->lastUsedTime = millis();
        LOG_INFO("AODV: Forwarding packet id=%d to next_hop 0x%x", p->id, route->nextHop);
        return forwardWithRoute(p, route);
    } else {
        // No valid route, initiate route discovery
        LOG_INFO("AODV: No route to 0x%x, initiating route discovery for packet id=%d", p->to, p->id);
        initiateRouteDiscovery(p);
        return ERRNO_OK; // Packet will be sent when route is discovered
    }
}

/**
 * Forward packet using discovered route
 */
ErrorCode AODVRouter::forwardWithRoute(meshtastic_MeshPacket *p, const AODVRouteEntry *route)
{
    LOG_INFO("AODV: forwardWithRoute() called for packet id=%d to dest 0x%x", p->id, p->to);
    
    // Set next hop preference
    p->next_hop = route->nextHop;
    
    // Update hop limit based on known hop count
    if (p->hop_limit == 0 || p->hop_limit < route->hopCount + 2) {
        p->hop_limit = route->hopCount + 2; // Add margin
    }
    
    LOG_INFO("AODV: Sending packet id=%d via FloodingRouter, next_hop=0x%x, hop_limit=%d", 
             p->id, p->next_hop, p->hop_limit);
    
    // Send via base router
    ErrorCode result = FloodingRouter::send(p);
    LOG_INFO("AODV: FloodingRouter::send() returned result=%d for packet id=%d", result, p->id);
    return result;
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
    // IMPORTANT: RREQ ID is assigned HERE and will be reused for ALL retries
    // Only when this pending RREQ is removed (timeout or success) and a NEW
    // route discovery is initiated will a new RREQ ID be allocated
    PendingRREQ pending;
    pending.destination = destination;
    pending.rreqId = getNextRREQId(); // Allocate NEW RREQ ID for this route discovery
    pending.ttl = 3; // Start with TTL of 3
    pending.retryCount = 0;
    pending.nextRetryTime = millis() + AODV_NET_TRAVERSAL_TIME;  //need to change according to maximum time takes to go through the network in worst case scenario
    pending.bufferedPacket = p;
    
    pendingRREQs[destination] = pending;
    
    LOG_INFO("AODV: Buffered packet id=%d for dest 0x%x, sending RREQ id=%d", 
             p->id, destination, pending.rreqId);
    
    // Send RREQ
    sendRREQ(destination, pending.ttl);
}

/**
 * Send Route Request (RREQ) - Header-based (Physical Layer)
 * 
 * RREQ packet structure (ALL DATA IN HEADER - NO PAYLOAD):
 * Header (sent over LoRa, unencrypted, 16 bytes):
 *   - to: BROADCAST (0xFFFFFFFF) - 4 bytes
 *   - from: Originator NodeNum (full address) - 4 bytes
 *   - id: RREQ ID - 4 bytes
 *   - flags: hop_limit, want_ack, etc - 1 byte
 *   - channel: Hop Count (starts at 0, incremented by forwarders) - 1 byte
 *   - next_hop: Destination NodeNum (low byte) - WHO we're looking for - 1 byte
 *   - relay_node: Current Node (low byte of who is forwarding) - 1 byte
 * Payload (8 bytes, unencrypted):
 *   - 4 bytes: Originator Sequence Number
 *   - 4 bytes: Destination Sequence Number (0 if unknown)
 */
void AODVRouter::sendRREQ(NodeNum destination, uint8_t ttl)
{
    LOG_INFO("AODV: Sending header-based RREQ for dest 0x%x with TTL %d", destination, ttl);
    
    auto it = pendingRREQs.find(destination);
    if (it == pendingRREQs.end()) {
        LOG_WARN("AODV: No pending RREQ found for 0x%x", destination);
        return;
    }
    
    // Get destination sequence number if we have it
    uint8_t destSeqNum = 0;
    AODVRouteEntry *oldRoute = findRoute(destination);
    if (oldRoute && oldRoute->validDestSeqNum) {
        destSeqNum = oldRoute->destSeqNum;
    }
    
    uint8_t origSeqNum = getNextSequenceNumber();
    //NodeNum currentNode = nodeDB->getNodeNum();
    NodeNum Originator = nodeDB->getNodeNum();// Originator is us   
    
    // Allocate packet
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_ERROR("AODV: Failed to allocate RREQ packet");
        return;
    }
    
    // Set header fields for RREQ - ALL AODV DATA IN HEADER
    p->to = NODENUM_BROADCAST;
    p->from = Originator;  // Originator (full 4 bytes)
    p->id = Originator; // RREQ orginator
    p->hop_limit = ttl;
    p->want_ack = false;
    // p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    
    // CRITICAL: AODV data in header bytes:
    p->channel = 0;  // Hop count starts at 0, incremented by each forwarder
    p->next_hop = destination & 0xFF;  // Destination node low byte (who we're looking for)
    // p->tx_after = destination;  // Destination node full id
    p->relay_node = it->second.rreqId;  // RREQ ID stored in relay_node byte
    
    // Use custom portnum to mark this as AODV RREQ (won't be encrypted)
    p->decoded.portnum = AODV_PORTNUM_RREQ;
    p->decoded.want_response = false;
    
    // originator sequence number
    p->hop_start = origSeqNum;
    // Encode sequence numbers in 8-byte payload
    // uint8_t seqNums[2] = {origSeqNum, destSeqNum};
    // p->decoded.payload.size = 2;
    // memcpy(p->decoded.payload.bytes, seqNums, 2);

    // Update the pending RREQ entry (already exists from initiateRouteDiscovery)
    // it->second.nextRetryTime = millis() + AODV_NET_TRAVERSAL_TIME;
    
    // Add to RREQ cache
    addRREQToCache(Originator, it->second.rreqId);
    
    LOG_INFO("AODV: Sending RREQ: id=0x%x, from=0x%x, dest=0x%x, origSeq=%u, destSeq=%u, currentNode=0x%x, hopCount=0", 
             it->second.rreqId, Originator, destination, origSeqNum, destSeqNum, Originator);
    
    // Send via base class
    FloodingRouter::send(p);
}


/**
 * Handle received RREQ - Header-based (Physical Layer)
 * 
 * Extracts RREQ data from packet header fields:
 *   - p->from = previous hop (full 4 bytes, constant)
 *   - p->to = BROADCAST
 *   - p->id = originator address (full 4 bytes)
 *   - p->channel = Hop count
 *   - p->next_hop = Destination (1 byte)
 *   - p->relay_node = RREQ ID
 * Payload (8 bytes, unencrypted):
 *   - 4 bytes: Originator Sequence Number
 *   - 4 bytes: Destination Sequence Number (0 if unknown)
 */
void AODVRouter::handleRREQ(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing)
{
    // Extract RREQ data from header fields
    NodeNum previous = p->from;  // Full previous node address (4 bytes)
    uint32_t rreqId = p->relay_node; // RREQ ID stored in relay_node byte
    NodeNum originator = p->id; // Originator is the sender of RREQ
    uint8_t hopCount = p->channel;  // Hop count stored in channel byte
    // uint8_t prevHopLowByte = p->relay_node;  // Previous node low byte
    uint8_t origSeqNum = p->hop_start;      // store the originator sequence number
    
    // Extract sequence numbers from payload
    // if (p->decoded.payload.size < 8) {
    //     LOG_WARN("AODV: Invalid RREQ payload size: %d (expected 8)", p->decoded.payload.size);
    //     return;
    // }
    
    // uint32_t origSeqNum, destSeqNum;
    // memcpy(&origSeqNum, p->decoded.payload.bytes, 4);
    // memcpy(&destSeqNum, p->decoded.payload.bytes + 4, 4);
    
    // Match prevHopLowByte against NodeDB to get full previous hop address
    // NodeNum prevHop = originator;  // Default to originator if we can't find match
    // for (int i = 0; i < nodeDB->getNumMeshNodes(); i++) {
    //     meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
    //     if (node && node->has_user && (node->num & 0xFF) == prevHopLowByte) {
    //         prevHop = node->num;
    //         break;
    //     }
    // }
    
    // Get destination - only have low byte, need to match against NodeDB
    uint8_t destLowByte = p->next_hop;
    // Find full destination address from NodeDB
    // uint32_t dest = p->tx_after;

    NodeNum ourAddr = nodeDB->getNodeNum();
    
    LOG_INFO("AODV: Received header-based RREQ id=0x%x from 0x%x, dest=0x%08x, hops=%d",
             rreqId, originator, destLowByte, hopCount);
    
    // Check if we've already seen this RREQ
    if (isRREQCached(originator, rreqId)) {
        LOG_WARN("AODV: RREQ already processed, ignoring");
        return;
    }

    addRREQToCache(originator, rreqId);

    // Create reverse route to originator
    AODVRouteEntry *routeToOriginator = findRoute(originator);
    if (routeToOriginator && routeToOriginator->routeValid) {
        if (hopCount + 1 < routeToOriginator->hopCount) {
            LOG_INFO("AODV: Updating route to originator 0x%x via 0x%x", originator, previous);
            updateRoute(originator, previous, hopCount + 1, origSeqNum,
                        millis() + AODV_ACTIVE_ROUTE_TIMEOUT);
        }
    } else {
        LOG_INFO("AODV: Adding route to originator 0x%x via 0x%x", originator, previous);
        addRoute(originator, previous, hopCount + 1, origSeqNum,
                 millis() + AODV_ACTIVE_ROUTE_TIMEOUT);
    }

    // Check if we are the destination (compare low byte)
    // prev check ourAddr & 0xFF
    if ((ourAddr & 0xFF) == destLowByte) {
        LOG_INFO("AODV: We are the destination (matched low byte), sending RREP");
        
        addRREPToCache(ourAddr, rreqId);
        
        // Send header-based RREP back to originator
        sendRREP(originator, ourAddr, rreqId, origSeqNum, hopCount, previous); // change to RREQ sequence number
        return;
    }

    // Forward RREQ if hop limit allows
    if (p->hop_limit > 0) {
        LOG_DEBUG("AODV: Forwarding RREQ");
        
        meshtastic_MeshPacket *fwdP = router->allocForSending();
        if (!fwdP) {
            LOG_ERROR("AODV: Failed to allocate packet for RREQ forwarding");
            return;
        }
        
        NodeNum currentNode = nodeDB->getNodeNum();
        
        // Copy header fields with incremented hop count
        fwdP->to = NODENUM_BROADCAST;
        fwdP->from = currentNode;  // Keep current node as sender
        fwdP->id = originator; // RREQ originator
        fwdP->hop_limit = p->hop_limit - 1;
        fwdP->want_ack = false;
        fwdP->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        
        // Update AODV header fields
        fwdP->channel = hopCount + 1;  // INCREMENT hop count
        fwdP->next_hop = destLowByte;  // Keep destination low byte
        fwdP->relay_node = rreqId;  // Update to current node low byte
        
        fwdP->decoded.portnum = AODV_PORTNUM_RREQ;
        fwdP->decoded.want_response = false;
        
        // Copy payload unchanged (sequence numbers)
        // fwdP->decoded.payload.size = 8;
        // memcpy(fwdP->decoded.payload.bytes, p->decoded.payload.bytes, 8);
        
        LOG_INFO("AODV: Forwarding RREQ: hopCount=%d, currentNode=0x%x", hopCount + 1, currentNode);
        FloodingRouter::send(fwdP);
    }
}

/**
 * Handle received RREP - Header-based (Physical Layer)
 * 
 * Extracts RREP data from packet header fields:
 *   - p->from = Destination (full 4 bytes, constant)
 *   - p->to = Originator (who sent RREQ)
 *   - p->id = RREQ ID
 *   - p->channel = Hop count to destination
 *   - p->next_hop = Next hop towards originator (low byte)
 *   - p->relay_node = Current/previous node (low byte who forwarded to us)
 *   - payload[0-3] = Destination SeqNum
 */
void AODVRouter::handleRREP(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing)
{
    // Extract RREP data from header fields
    NodeNum destination = p->to;        // RREP destination from previous packet
    NodeNum prev_Hop = p->from;     // Previour hop of RREP
    uint32_t dest_rreq = p->id;           // Destination of RREQ
    uint8_t hopCountToDest = p->channel;  // Hop count to destination (from channel byte)
    uint8_t rreqId = p->relay_node;  // Previous node low byte
    uint8_t destSeqNum = p->hop_start;      // store the destination sequence number
    
    // Extract destSeqNum from payload
    // if (p->decoded.payload.size < 4) {
    //     LOG_WARN("AODV: Invalid RREP payload size: %d (expected 4)", p->decoded.payload.size);
    //     return;
    // }
    
    // uint32_t destSeqNum;
    // memcpy(&destSeqNum, p->decoded.payload.bytes, 4);
    
    // Match prevHopLowByte against NodeDB to get full previous hop address
    // NodeNum rreq_org = p->next_hop;  // Default to destination if we can't find match
    // for (int i = 0; i < nodeDB->getNumMeshNodes(); i++) {
    //     meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
    //     if (node && node->has_user && (node->num & 0xFF) == prevHopLowByte) {
    //         prevHop = node->num;
    //         break;
    //     }
    // }

    NodeNum rreq_org = p->next_hop; // RREQ originator low byte

    //need to get full address of rreq_org
    // for (int i = 0; i < nodeDB->getNumMeshNodes(); i++) {
    //     meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
    //     if (node && node->has_user && (node->num & 0xFF) == rreq_org) {
    //         rreq_org = node->num;
    //         break;
    //     }
    // }
    
    NodeNum thisNode = nodeDB->getNodeNum();
    
    LOG_INFO("AODV: Received header-based RREP id=0x%x, dest=0x%x, originator=0x%x, hops=%d",
             rreqId, dest_rreq, rreq_org, hopCountToDest);
    
    // Cache RREP to prevent duplicate processing
    if (isRREPCached(dest_rreq, rreqId)) {
        LOG_WARN("AODV: RREP already processed, ignoring");
        return;
    }
    addRREPToCache(dest_rreq, rreqId);
    
    // Create/Update forward route to destination
    AODVRouteEntry *existingRoute = findRoute(dest_rreq);
    if (existingRoute && existingRoute->routeValid) { 
        if (hopCountToDest + 1 < existingRoute->hopCount) {
            LOG_INFO("AODV: Updating route to destination 0x%x via 0x%x", 
                    dest_rreq, prev_Hop);
            updateRoute(dest_rreq, prev_Hop, hopCountToDest + 1, destSeqNum,
                        millis() + AODV_ACTIVE_ROUTE_TIMEOUT);
        }
    } else {
        LOG_INFO("AODV: Adding route to destination 0x%x via 0x%x", 
                    dest_rreq, prev_Hop);
        addRoute(dest_rreq, prev_Hop, hopCountToDest + 1, destSeqNum,
            millis() + AODV_ACTIVE_ROUTE_TIMEOUT);
    }
    
    // Check if we are the RREQ originator
    auto it = pendingRREQs.find(dest_rreq);
    if (it != pendingRREQs.end() && it->second.rreqId == rreqId && (thisNode & 0xFF) == rreq_org) {
        LOG_INFO("AODV: Route discovery complete for 0x%x", rreq_org);
        
        // Extract buffered packet
        meshtastic_MeshPacket *bufferedPacket = it->second.bufferedPacket;
        it->second.bufferedPacket = nullptr;
        pendingRREQs.erase(it);
        
        // TODO: Send NodeInfo handshake here before sending data packet
        // This allows encryption key exchange between originator and destination
        
        // Send buffered packet using discovered route
        if (bufferedPacket) {
            LOG_INFO("AODV: Sending buffered packet id=%d to 0x%x", 
                     bufferedPacket->id, rreq_org);
            ErrorCode result = send(bufferedPacket);
            if (result != ERRNO_OK) {
                LOG_WARN("AODV: Failed to send buffered packet, result=%d", result);
            }
        }
        return;
    }
    
    //only need to forward that only if we are the destination
    if (thisNode == destination) {
        LOG_INFO("AODV: We are the destination of RREP, forward to the next node");
        // We are intermediate node, forward RREP towards originator
        AODVRouteEntry *originatorRoute = findRoute(rreq_org);

        // if route to originator exists, forward RREP
        // no need to make a new RREP just forward it looking at routing table

        if (originatorRoute && originatorRoute->routeValid) {
            LOG_INFO("AODV: Forwarding RREP to originator 0x%x via 0x%x", 
                     rreq_org, originatorRoute->nextHop);
            
            meshtastic_MeshPacket *fwdP = router->allocForSending();
            if (!fwdP) {
                LOG_ERROR("AODV: Failed to allocate packet for RREP forwarding");
                return;
            }
            
            NodeNum currentNode = nodeDB->getNodeNum();

            // get the next hop towards originator
            NodeNum nextHopToOrig = originatorRoute->nextHop;

            // get the hop count to originator
            uint8_t hopCountToOrig = originatorRoute->hopCount;

            // make the RREP forwarding packet
            fwdP->to = nextHopToOrig;  // Send to RREQ originator
            fwdP->from = currentNode;  // From current node
            fwdP->id = dest_rreq;  // Destination address
            fwdP->hop_limit = hopCountToOrig + 2;   // Enough hops to reach originator
            fwdP->want_ack = false;
            fwdP->priority = meshtastic_MeshPacket_Priority_RELIABLE;
            fwdP->next_hop = rreq_org; // next hop towards originator low byte
            fwdP->relay_node = rreqId; // RREQ ID
            fwdP->channel = hopCountToDest + 1;  // Hop count to destination
            fwdP->decoded.portnum = AODV_PORTNUM_RREP;
            fwdP->decoded.want_response = false;
            fwdP->hop_start = destSeqNum;
            
        }
        else {
            LOG_WARN("AODV: No route to originator 0x%x, cannot forward RREP", rreq_org);
        }
    }
    else{
        LOG_INFO("AODV: Not the destination of RREP, no further action taken");
    }
    
}

//send RREP to next hop
/**
 * Send Route Reply (RREP) - Header-based (Physical Layer)
 * 
 * RREP packet structure:
 * Header (sent over LoRa, unencrypted):
 *   - to: Originator NodeNum (who initiated RREQ)
 *   - from: Current node (destination or intermediate with route)
 *   - id: RREQ ID (matches the RREQ we're replying to)
 *   - flags: will be set with PACKET_TYPE_RREP in bits 3-4  
 *   - channel: channel hash
 *   - next_hop: Next hop towards originator (reverse route)
 *   - relay_node: HopCount to final destination
 * Payload (4 bytes, unencrypted):
 *   - 4 bytes: Destination Sequence Number
 */
void AODVRouter::sendRREP(NodeNum originatorAddr, NodeNum destinationAddr, uint32_t rreqId, 
                         uint32_t destSeqNum, uint8_t hopCountToDest, NodeNum nextHopToOrig)
{
    NodeNum currentNode = nodeDB->getNodeNum();
    LOG_INFO("AODV: Sending header-based RREP to originator 0x%x via next_hop 0x%x", 
             originatorAddr, nextHopToOrig);
    
    // Allocate packet
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_ERROR("AODV: Failed to allocate RREP packet");
        return;
    }
    
    // Set header fields for RREP - ALL AODV DATA IN HEADER
    p->to = nextHopToOrig;              // Send to RREQ originator
    p->from = currentNode;           // From current node
    p->id = destinationAddr;                      // Destination address
    p->hop_limit = hopCountToDest + 2;   // Enough hops to reach originator
    p->want_ack = false;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    
    // CRITICAL: AODV data in header bytes:
    p->channel = 0;  // Hop count to destination
    p->next_hop = originatorAddr & 0xFF;  // Next hop towards originator (low byte)
    p->relay_node = rreqId;  // Current node low byte (who is sending)
    
    // Use custom portnum to mark this as AODV RREP (won't be encrypted)
    p->decoded.portnum = AODV_PORTNUM_RREP;
    p->decoded.want_response = false;
    
    // sequecne number in hop_start
    p->hop_start = destSeqNum;
    // Encode only destSeqNum in 4-byte payload
    // p->decoded.payload.size = 4;
    // memcpy(p->decoded.payload.bytes, &destSeqNum, 4);


    
    LOG_INFO("AODV: Sending RREP: id=0x%x, to=0x%x, from=0x%x, destSeq=%u, hopCount=%d, currentNode=0x%x",
             rreqId, originatorAddr, destinationAddr, destSeqNum, hopCountToDest, currentNode);
    
    // Send via base class
    FloodingRouter::send(p);
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
 * Now handles header-based AODV packets (RREQ/RREP via custom portnums)
 */
void AODVRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    // Handle new header-based AODV packets (identified by custom portnum)
    if (p->decoded.portnum == AODV_PORTNUM_RREQ) {
        LOG_DEBUG("AODV: Detected header-based RREQ packet");
        handleRREQ(p, nullptr);  // No routing structure, data in header
        return; // Don't pass to parent, this is AODV control traffic
    } else if (p->decoded.portnum == AODV_PORTNUM_RREP) {
        LOG_DEBUG("AODV: Detected header-based RREP packet");
        handleRREP(p, nullptr);  // No routing structure, data in header
        return;
    } else if (p->decoded.portnum == AODV_PORTNUM_RERR) {
        LOG_DEBUG("AODV: Detected header-based RERR packet");
        handleRERR(p, nullptr);
        return;
    }
    
    // Legacy: Handle old payload-based AODV messages (for backward compatibility)
    if (c && p->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        if (c->which_variant == meshtastic_Routing_route_request_tag) {
            LOG_WARN("AODV: Received legacy payload-based RREQ (deprecated)");
            return;
        } else if (c->which_variant == meshtastic_Routing_route_reply_tag) {
            LOG_WARN("AODV: Received legacy payload-based RREP (deprecated)");
            return;
        }
    }
    
    // Call parent implementation for regular data packets
    FloodingRouter::sniffReceived(p, c);
}

/**
 * Check if packet should be filtered
 */
bool AODVRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    // Let AODV control messages through
    if (p->decoded.portnum == AODV_PORTNUM_RREQ || 
        p->decoded.portnum == AODV_PORTNUM_RREP ||
        p->decoded.portnum == AODV_PORTNUM_RERR ||
        p->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
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

    // Clean up old RREP cache entries
    cleanRREPCache();
    
    // Print routing table every 60 seconds (1 minute)
    if (now - lastRoutingTablePrint > 15000) {
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
 * check if RREP is in cache
 */
bool AODVRouter::isRREPCached(NodeNum originator, uint32_t rrepId)
{
    RREPCacheKey key{originator, rrepId};
    return rrepCache.find(key) != rrepCache.end();
}

/**
 * Add RREQ to cache
 */
void AODVRouter::addRREQToCache(NodeNum originator, uint32_t rreqId)
{
    RREQCacheKey key{originator, rreqId};
    rreqCache[key] = RREQCacheEntry(originator, rreqId, millis());
}

/***
 * Make a RREP cache
 */
void AODVRouter::addRREPToCache(NodeNum originator, uint32_t rrepId)
{
    RREPCacheKey key{originator, rrepId};
    rrepCache[key] = RREPCacheEntry(originator, rrepId, millis());
}

/**
 * Clean old RREQ cache entries
 */
void AODVRouter::cleanRREQCache()
{
    uint32_t now = millis();
    uint32_t timeout = AODV_CACHE_CLEANUP_INTERVAL;
    
    for (auto it = rreqCache.begin(); it != rreqCache.end(); ) {
        if (now - it->second.timestamp > timeout) {
            it = rreqCache.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Clean old RREP cache entries
 */
void AODVRouter::cleanRREPCache()
{
    uint32_t now = millis();
    uint32_t timeout = AODV_CACHE_CLEANUP_INTERVAL;
    
    for (auto it = rrepCache.begin(); it != rrepCache.end(); ) {
        if (now - it->second.timestamp > timeout) {
            it = rrepCache.erase(it);
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
            // Check if we now have a valid route (RREP might have arrived)
            AODVRouteEntry *route = findRoute(it->second.destination);
            if (route && route->routeValid) {
                LOG_DEBUG("AODV: Route now available for 0x%x, canceling pending RREQ", it->second.destination);
                
                // Free buffered packet if it exists (should have been sent by handleRREP)
                if (it->second.bufferedPacket) {
                    packetPool.release(it->second.bufferedPacket);
                }
                
                it = pendingRREQs.erase(it);
                continue;
            }
            
            if (it->second.retryCount < AODV_RREQ_RETRIES) {
                // Retry with expanded TTL but SAME RREQ ID (no new ID allocated)
                it->second.retryCount++;
                uint8_t newTtl = it->second.ttl * 2;
                it->second.ttl = (newTtl < HOP_MAX) ? newTtl : (uint8_t)HOP_MAX;
                it->second.nextRetryTime = now + AODV_NET_TRAVERSAL_TIME;
                
                LOG_INFO("AODV: Retrying RREQ for 0x%x (retry %d, TTL %d, REUSING RREQ ID=%d)",
                         it->second.destination, it->second.retryCount, it->second.ttl, it->second.rreqId);
                
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
 * NOTE: This function is now deprecated - buffered packets are sent directly in handleRREP
 * Kept for backward compatibility but should not be called
 */
void AODVRouter::sendBufferedPacket(NodeNum destination)
{
    auto it = pendingRREQs.find(destination);
    if (it != pendingRREQs.end() && it->second.bufferedPacket) {
        LOG_WARN("AODV: sendBufferedPacket called (should be handled in handleRREP)");
        
        meshtastic_MeshPacket *p = it->second.bufferedPacket;
        it->second.bufferedPacket = nullptr; // Clear reference before sending
        
        // Now send the packet (will use the newly discovered route)
        ErrorCode result = send(p);
        
        if (result != ERRNO_OK) {
            LOG_WARN("AODV: Failed to send buffered packet, result=%d", result);
        }
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
uint8_t AODVRouter::getNextSequenceNumber()
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
    
    //  FIX: Set relay_node to track the path correctly
    // This allows intermediate nodes to know who the previous hop was
    // Critical for AODV reverse route creation
    // p->relay_node = nodeDB->getNodeNum();
    
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
