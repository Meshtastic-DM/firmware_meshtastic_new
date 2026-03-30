#include "NextHopRouter.h"
#include "MeshTypes.h"
#include "meshUtils.h"
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif
#include "NodeDB.h"
#include "modules/AODVModule.h"

NextHopRouter::NextHopRouter() {}

PendingPacket::PendingPacket(meshtastic_MeshPacket *p, uint8_t numRetransmissions)
{
    packet = p;
    this->numRetransmissions = numRetransmissions - 1; // We subtract one, because we assume the user just did the first send
}

/**
 * Send a packet
 */
ErrorCode NextHopRouter::send(meshtastic_MeshPacket *p)
{
    // Add any messages _we_ send to the seen message list (so we will ignore all retransmissions we see)
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // First set the relayer to us
    wasSeenRecently(p);                                         // FIXME, move this to a sniffSent method

    // Check if packet is decoded and what type of control traffic it is
    bool isDecoded = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag);
    bool isAodvControl = isDecoded && (p->decoded.portnum == meshtastic_PortNum_AODV_ROUTING_APP);
    bool isSdnControl = isDecoded && (p->decoded.portnum == meshtastic_PortNum_SDN_APP);
    bool isRoutingCtrl = isDecoded && (p->decoded.portnum == meshtastic_PortNum_ROUTING_APP);

    // Check if this is an AODV control packet from us with explicit next_hop set
    if (isAodvControl && isFromUs(p) && p->next_hop != NO_NEXT_HOP_PREFERENCE) {
        // Preserve the next_hop set by AODVModule (for RREP routing)
        LOG_DEBUG("Preserving AODV control next hop for dest 0x%x to 0x%x", p->to, p->next_hop);
    } else {
        // Calculate next_hop using route table or default behavior
        p->next_hop = getNextHop(p->to, p->relay_node);
        LOG_DEBUG("Setting next hop for packet with dest %x to %x", p->to, p->next_hop);
    }
    
    // Log data packet routing
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum != meshtastic_PortNum_AODV_ROUTING_APP &&
        p->decoded.portnum != meshtastic_PortNum_SDN_APP &&
        p->decoded.portnum != meshtastic_PortNum_ROUTING_APP) {
        
        if (isFromUs(p)) {
            if (isBroadcast(p->to)) {
                LOG_INFO("DATA BCAST: port=%d, id=0x%x, hop_limit=%d", 
                         p->decoded.portnum, p->id, p->hop_limit);
            } else if (p->next_hop != NO_NEXT_HOP_PREFERENCE) {
                LOG_INFO("DATA SEND: port=%d, dest=0x%x, next_hop=0x%x, id=0x%x",
                         p->decoded.portnum, p->to, p->next_hop, p->id);
            }
        }
    }

    // If no route exists and we're sending from local node, trigger AODV route discovery
    // But never trigger discovery for AODV control packets, SDN control packets, or routing protocol packets
    if (isFromUs(p) && !isBroadcast(p->to) && p->next_hop == NO_NEXT_HOP_PREFERENCE && aodvModule && isDecoded &&
        !isAodvControl && !isSdnControl && !isRoutingCtrl) {
        LOG_INFO("AODV: No route to 0x%x, initiating route discovery", p->to);
        aodvModule->initiateRouteDiscovery(p->to, packetPool.allocCopy(*p));
        
        // Stop retransmissions - packet is now managed by AODV buffer
        // AODV will deliver it once route is found, no need for original retransmission
        LOG_DEBUG("AODV: Stopping retransmission for buffered packet id=0x%x", p->id);
        stopRetransmission(getFrom(p), p->id);
        
        // Original packet will be buffered by AODV module, release this one
        packetPool.release(p);
        return ERRNO_OK;
    }

    // If it's from us, ReliableRouter already handles retransmissions if want_ack is set. If a next hop is set and hop limit is
    // not 0 or want_ack is set, start retransmissions
    if ((!isFromUs(p) || !p->want_ack) && p->next_hop != NO_NEXT_HOP_PREFERENCE && (p->hop_limit > 0 || p->want_ack))
        startRetransmission(packetPool.allocCopy(*p)); // start retransmission for relayed packet

    return Router::send(p);
}

bool NextHopRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    bool wasFallback = false;
    bool weWereNextHop = false;
    bool wasUpgraded = false;
    bool seenRecently = wasSeenRecently(p, true, &wasFallback, &weWereNextHop,
                                        &wasUpgraded); // Updates history; returns false when an upgrade is detected

    // Handle hop_limit upgrade scenario for rebroadcasters
    if (wasUpgraded && perhapsHandleUpgradedPacket(p)) {
        return true; // we handled it, so stop processing
    }

    if (seenRecently) {
        printPacket("Ignore dupe incoming msg", p);

        if (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA) {
            rxDupe++;
            // For packets from us, keep unicast retransmissions until an explicit routing ACK/NAK is received.
            if (!isFromUs(p) || isBroadcast(p->to) || !p->want_ack) {
                stopRetransmission(p->from, p->id);
            }
        }

        // If it was a fallback to flooding, try to relay again
        if (wasFallback) {
            LOG_INFO("Fallback to flooding from relay_node=0x%x", p->relay_node);
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id)) {
                reprocessPacket(p);
                perhapsRebroadcast(p);
            }
        } else {
            bool isRepeated = p->hop_start > 0 && p->hop_start == p->hop_limit;
            // If repeated and not in Tx queue anymore, try relaying again, or if we are the destination, send the ACK again
            if (isRepeated) {
                if (!findInTxQueue(p->from, p->id)) {
                    reprocessPacket(p);
                    if (!perhapsRebroadcast(p) && isToUs(p) && p->want_ack) {
                        LOG_INFO("ACK SEND: to=0x%x, for_id=0x%x, hop_limit=0 (repeated packet)", getFrom(p), p->id);
                        sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0);
                    }
                }
            } else if (!weWereNextHop) {
                perhapsCancelDupe(p); // If it's a dupe, cancel relay if we were not explicitly asked to relay
            }
        }
        return true;
    }

    return Router::shouldFilterReceived(p);
}

void NextHopRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_AODV_ROUTING_APP) {
        auto insertedNode = aodvNodes.insert(getFrom(p));
        if (insertedNode.second) {
            LOG_INFO("AODV NODE LEARNED: node=0x%x", getFrom(p));
        }
    }

    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
                        (p->decoded.request_id != 0 || p->decoded.reply_id != 0);
    if (isAckorReply) {
        // ACK-based route learning has been REMOVED - AODV manages routes dynamically
        // Just handle ACK cancellation for rebroadcast and stop retransmissions
        if (!isToUs(p)) {
            LOG_DEBUG("ACK/Reply overhear: from=0x%x, to=0x%x, request_id=0x%x, canceling rebroadcast",
                      p->from, p->to, p->decoded.request_id);
            Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
            // stop retransmission for the original packet
            stopRetransmission(p->to, p->decoded.request_id); // for original packet, from = to and id = request_id
        }
    }

    perhapsRebroadcast(p);

    // handle the packet as normal
    Router::sniffReceived(p, c);
}

/* Helper to get our own last byte node number */
static inline uint8_t myLastByte()
{
    return nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
}

/* Check if we should be rebroadcasting this packet if so, do so. */
bool NextHopRouter::perhapsRebroadcast(const meshtastic_MeshPacket *p)
{
    if (isToUs(p) || isFromUs(p) || p->hop_limit == 0 || p->id == 0)
        return false;

    if (!isRebroadcaster()) {
        LOG_DEBUG("No rebroadcast: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
        return false;
    }

    const uint8_t me = myLastByte();

    // Case A: Packet is explicitly routed (unicast next-hop style)
    if (p->next_hop != NO_NEXT_HOP_PREFERENCE) {

        // AODV rule: only the intended next hop forwards
        if (p->next_hop != me) {
            LOG_INFO("DM DROP: dest=0x%x, from=0x%x, next_hop=0x%02x, me=0x%02x, relay=0x%02x",
                     p->to, p->from, p->next_hop, me, p->relay_node);
            return false;
        }

        // Log when forwarding packets
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
            if (p->decoded.portnum == meshtastic_PortNum_AODV_ROUTING_APP) {
                LOG_INFO("AODV CTRL FWD: from=0x%x, to=0x%x, id=0x%x, hop_limit=%d",
                         p->from, p->to, p->id, p->hop_limit);
            } else if (p->decoded.portnum != meshtastic_PortNum_ROUTING_APP) {
                LOG_INFO("DATA FWD: port=%d, from=0x%x, to=0x%x, id=0x%x, hop_limit=%d", 
                         p->decoded.portnum, p->from, p->to, p->id, p->hop_limit);
            }
        }

        meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);

        // We are forwarding now
        tosend->relay_node = me;

        // Decrement hop_limit using shared logic
        if (shouldDecrementHopLimit(p)) {
            tosend->hop_limit--;
        }

#if USERPREFS_EVENT_MODE
        if (tosend->hop_limit > 2) {
            tosend->hop_start -= (tosend->hop_limit - 2);
            tosend->hop_limit = 2;
        }
#endif

        // Send as next-hop routed unicast
        // NextHopRouter::send() will recompute next_hop for the next leg
        NextHopRouter::send(tosend);
        return true;
    }

    // Case B: Broadcast packets use flooding-style behavior
    if (isBroadcast(p->to)) {
        meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);

        LOG_INFO("Flood-rebroadcast received message coming from %x", p->relay_node);

        if (shouldDecrementHopLimit(p)) {
            tosend->hop_limit--;
        } else {
            LOG_INFO("favorite-ROUTER/CLIENT_BASE-to-ROUTER/CLIENT_BASE rebroadcast: preserving hop_limit");
        }

#if USERPREFS_EVENT_MODE
        if (tosend->hop_limit > 2) {
            tosend->hop_start -= (tosend->hop_limit - 2);
            tosend->hop_limit = 2;
        }
#endif

        FloodingRouter::send(tosend);
        return true;
    }

    // Case C: Unicast packet with no next_hop set.
    const bool isAodvControl =
        (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
        (p->decoded.portnum == meshtastic_PortNum_AODV_ROUTING_APP);
    if (isAodvControl) {
        const uint8_t recoveredNextHop = getNextHop(p->to, p->relay_node, false);
        if (recoveredNextHop == NO_NEXT_HOP_PREFERENCE) {
            LOG_INFO("AODV NO_NEXT_HOP: dest=0x%x, from=0x%x, relay=0x%02x, me=0x%02x",
                     p->to, p->from, p->relay_node, me);
            return false;
        }

        LOG_INFO("AODV REROUTE: dest=0x%x, from=0x%x, relay=0x%02x, me=0x%02x, route_next_hop=0x%02x",
                 p->to, p->from, p->relay_node, me, recoveredNextHop);

        meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);

        // We are forwarding now
        tosend->relay_node = me;

        // Decrement hop_limit using shared logic
        if (shouldDecrementHopLimit(p)) {
            tosend->hop_limit--;
        }

#if USERPREFS_EVENT_MODE
        if (tosend->hop_limit > 2) {
            tosend->hop_start -= (tosend->hop_limit - 2);
            tosend->hop_limit = 2;
        }
#endif

        // NextHopRouter::send() will recompute next_hop for the next leg.
        NextHopRouter::send(tosend);
        return true;
    }

    if (aodvNodes.find(getFrom(p)) == aodvNodes.end()) {
        meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);

        LOG_INFO("DM FALLBACK_FLOOD: dest=0x%x, from=0x%x, relay=0x%02x, me=0x%02x",
                 p->to, p->from, p->relay_node, me);

        if (shouldDecrementHopLimit(p)) {
            tosend->hop_limit--;
        } else {
            LOG_INFO("favorite-ROUTER/CLIENT_BASE-to-ROUTER/CLIENT_BASE rebroadcast: preserving hop_limit");
        }

#if USERPREFS_EVENT_MODE
        if (tosend->hop_limit > 2) {
            tosend->hop_start -= (tosend->hop_limit - 2);
            tosend->hop_limit = 2;
        }
#endif

        FloodingRouter::send(tosend);
        return true;
    }

    const uint8_t recoveredNextHop = getNextHop(p->to, p->relay_node, false);
    const uint8_t sourceNextHop = getNextHop(p->from, me, false);
    if (recoveredNextHop == NO_NEXT_HOP_PREFERENCE || sourceNextHop == NO_NEXT_HOP_PREFERENCE) {
        LOG_INFO("DM AODV_DROP_NO_ROUTE: dest=0x%x, from=0x%x, relay=0x%02x, me=0x%02x, route_to_dest=0x%02x, route_to_src=0x%02x",
                 p->to, p->from, p->relay_node, me, recoveredNextHop, sourceNextHop);
        return false;
    }

    LOG_INFO("DM AODV_REROUTE: dest=0x%x, from=0x%x, relay=0x%02x, me=0x%02x, route_next_hop=0x%02x, route_to_src=0x%02x",
             p->to, p->from, p->relay_node, me, recoveredNextHop, sourceNextHop);

    meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);

    // We are forwarding now
    tosend->relay_node = me;

    // Decrement hop_limit using shared logic
    if (shouldDecrementHopLimit(p)) {
        tosend->hop_limit--;
    }

#if USERPREFS_EVENT_MODE
    if (tosend->hop_limit > 2) {
        tosend->hop_start -= (tosend->hop_limit - 2);
        tosend->hop_limit = 2;
    }
#endif

    // NextHopRouter::send() will recompute next_hop for the next leg.
    NextHopRouter::send(tosend);
    return true;
}

/**
 * Get the next hop for a destination using AODV routing
 * @return the node number of the next hop, 0 if no preference (fallback to FloodingRouter)
 */
uint8_t NextHopRouter::getNextHop(NodeNum to, uint8_t relay_node, bool refreshRouteOnUse)
{
    if (isBroadcast(to))
        return NO_NEXT_HOP_PREFERENCE;

    // Use AODV route table instead of NodeDB next_hop field
    if (aodvModule) {
        AODVRouteEntry *route = aodvModule->getRouteTable()->findRoute(to);
        if (route) {
            // We are careful not to return the relay node as the next hop
            if (route->nextHop != relay_node) {
                LOG_INFO("AODV: Route found to 0x%x via next_hop=0x%x, hops=%d, expires in %ds", 
                         to, route->nextHop, route->hopCount, (route->expiryTime - millis()) / 1000);
                
                // Extend route expiry when actively used (keep-alive)
                if (refreshRouteOnUse) {
                    aodvModule->getRouteTable()->refreshRouteOnUse(to);
                }
                
                return route->nextHop;
            } else {
                LOG_WARN("AODV: Next hop for 0x%x is 0x%x, same as relayer; no preference", to, route->nextHop);
            }
        } else {
            LOG_DEBUG("AODV: No route found for 0x%x", to);
        }
    }
    
    return NO_NEXT_HOP_PREFERENCE;
}

PendingPacket *NextHopRouter::findPendingPacket(GlobalPacketId key)
{
    auto old = pending.find(key); // If we have an old record, someone messed up because id got reused
    if (old != pending.end()) {
        return &old->second;
    } else
        return NULL;
}

/**
 * Stop any retransmissions we are doing of the specified node/packet ID pair
 */
bool NextHopRouter::stopRetransmission(NodeNum from, PacketId id)
{
    auto key = GlobalPacketId(from, id);
    return stopRetransmission(key);
}

bool NextHopRouter::roleAllowsCancelingFromTxQueue(const meshtastic_MeshPacket *p)
{
    // Return true if we're allowed to cancel a packet in the txQueue (so we may never transmit it even once)

    // Return false for roles like ROUTER, ROUTER_LATE which should always transmit the packet at least once.

    return roleAllowsCancelingDupe(p); // same logic as FloodingRouter::roleAllowsCancelingDupe
}

bool NextHopRouter::stopRetransmission(GlobalPacketId key)
{
    auto old = findPendingPacket(key);
    if (old) {
        auto p = old->packet;
        /* Only when we already transmitted a packet via LoRa, we will cancel the packet in the Tx queue
          to avoid canceling a transmission if it was ACKed super fast via MQTT */
        if (old->numRetransmissions < NUM_RELIABLE_RETX - 1) {
            // We only cancel it if we are the original sender or if we're not a router(_late)
            if (isFromUs(p) || roleAllowsCancelingFromTxQueue(p)) {
                // remove the 'original' (identified by originator and packet->id) from the txqueue and free it
                LOG_DEBUG("stopRetransmission: canceling txQueue for from=0x%x, id=0x%x, to=0x%x, retries_left=%d",
                          getFrom(p), p->id, p->to, old->numRetransmissions);
                cancelSending(getFrom(p), p->id);
            }
        }

        // Regardless of whether or not we canceled this packet from the txQueue, remove it from our pending list so it
        // doesn't get scheduled again. (This is the core of stopRetransmission.)
        auto numErased = pending.erase(key);
        assert(numErased == 1);

        // When we remove an entry from pending, always be sure to release the copy of the packet that was allocated in the
        // call to startRetransmission.
        packetPool.release(p);

        return true;
    } else
        return false;
}

/**
 * Add p to the list of packets to retransmit occasionally.  We will free it once we stop retransmitting.
 */
PendingPacket *NextHopRouter::startRetransmission(meshtastic_MeshPacket *p, uint8_t numReTx)
{
    auto id = GlobalPacketId(p);
    auto rec = PendingPacket(p, numReTx);

    stopRetransmission(getFrom(p), p->id);

    setNextTx(&rec);
    pending[id] = rec;

    return &pending[id];
}

/**
 * Do any retransmissions that are scheduled (FIXME - for the time being called from loop)
 */
int32_t NextHopRouter::doRetransmissions()
{
    uint32_t now = millis();
    int32_t d = INT32_MAX;

    // FIXME, we should use a better datastructure rather than walking through this map.
    // for(auto el: pending) {
    for (auto it = pending.begin(), nextIt = it; it != pending.end(); it = nextIt) {
        ++nextIt; // we use this odd pattern because we might be deleting it...
        auto &p = it->second;

        bool stillValid = true; // assume we'll keep this record around

        // FIXME, handle 51 day rolloever here!!!
        if (p.nextTxMsec <= now) {
            if (p.numRetransmissions == 0) {
                if (isFromUs(p.packet)) {
                    LOG_INFO("NAK SEND: to=0x%x, for_id=0x%x, err=MAX_RETRANSMIT (reliable send failed fr=0x%x)", 
                             getFrom(p.packet), p.packet->id, p.packet->from);
                    sendAckNak(meshtastic_Routing_Error_MAX_RETRANSMIT, getFrom(p.packet), p.packet->id, p.packet->channel);
                    
                    // Notify AODV of link failure for route repair
                    if (aodvModule && !isBroadcast(p.packet->to)) {
                        LOG_INFO("AODV: Notifying link failure for 0x%x", p.packet->to);
                        aodvModule->handleLinkFailure(p.packet->to);
                    }
                }
                // Note: we don't stop retransmission here, instead the Nak packet gets processed in sniffReceived
                stopRetransmission(it->first);
                stillValid = false; // just deleted it
            } else {
                LOG_DEBUG("Sending retransmission fr=0x%x,to=0x%x,id=0x%x, tries left=%d", p.packet->from, p.packet->to,
                          p.packet->id, p.numRetransmissions);

                if (!isBroadcast(p.packet->to)) {
                    // For AODV: No fallback to flooding on last retry
                    // Instead, rely on AODV RERR to propagate and new RREQ to find alternate route
                    NextHopRouter::send(packetPool.allocCopy(*p.packet));
                } else {
                    // Note: we call the superclass version because we don't want to have our version of send() add a new
                    // retransmission record
                    FloodingRouter::send(packetPool.allocCopy(*p.packet));
                }

                // Queue again
                --p.numRetransmissions;
                setNextTx(&p);
            }
        }

        if (stillValid) {
            // Update our desired sleep delay
            int32_t t = p.nextTxMsec - now;

            d = min(t, d);
        }
    }

    return d;
}

void NextHopRouter::setNextTx(PendingPacket *pending)
{
    assert(iface);
    auto d = iface->getRetransmissionMsec(pending->packet);
    pending->nextTxMsec = millis() + d;
    LOG_DEBUG("Setting next retransmission in %u msecs: ", d);
    printPacket("", pending->packet);
    setReceivedMessage(); // Run ASAP, so we can figure out our correct sleep time
}
