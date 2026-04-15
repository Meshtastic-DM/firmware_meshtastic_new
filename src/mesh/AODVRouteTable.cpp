#include "AODVRouteTable.h"
#include "NodeDB.h"
#include "RTC.h"
#include "configuration.h"
#include <algorithm>

AODVRouteTable::AODVRouteTable() : mySeqNum(1), nextRREQId(1) {}

AODVRouteEntry *AODVRouteTable::findRoute(uint32_t destination)
{
    auto it = routes.find(destination);
    if (it != routes.end() && !it->second.empty()) {
        // Clean up expired routes first
        auto &routeList = it->second;
        routeList.erase(std::remove_if(routeList.begin(), routeList.end(),
                                      [](AODVRouteEntry &r) {
                                          if (r.isExpired()) {
                                              r.isValid = false;
                                              return true;
                                          }
                                          return false;
                                      }),
                       routeList.end());
        
        // Update pathId after cleanup
        for (size_t i = 0; i < routeList.size(); i++) {
            routeList[i].pathId = i;
        }
        
        // Return primary route (first valid route)
        if (!routeList.empty() && routeList[0].isValid && !routeList[0].isExpired()) {
            return &routeList[0];
        }
    }
    return nullptr;
}

std::vector<AODVRouteEntry> *AODVRouteTable::getAllRoutes(uint32_t destination)
{
    auto it = routes.find(destination);
    if (it != routes.end() && !it->second.empty()) {
        return &it->second;
    }
    return nullptr;
}

size_t AODVRouteTable::getRoutePathCount(uint32_t destination) const
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        return it->second.size();
    }
    return 0;
}

bool AODVRouteTable::hasValidRoute(uint32_t destination)
{
    return findRoute(destination) != nullptr;
}

void AODVRouteTable::addRoute(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum)
{
    uint32_t expiry = millis() + AODV_ACTIVE_ROUTE_TIMEOUT;
    std::vector<AODVRouteEntry> routeList;
    routeList.emplace_back(destination, nextHop, hopCount, destSeqNum, expiry, 0);
    routes[destination] = routeList;
    LOG_INFO("AODV ROUTE ADD: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u, pathId=0 (first path)", 
             destination, nextHop, hopCount, destSeqNum);
}

void AODVRouteTable::updateRoute(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum)
{
    auto it = routes.find(destination);
    
    if (it != routes.end()) {
        auto &routeList = it->second;
        
        // Check if this is a new sequence number (fresher route)
        if (!routeList.empty() && destSeqNum != 0 && destSeqNum != routeList[0].destSeqNum) {
            // Clear old routes - new seq invalidates all old routes
            LOG_INFO("AODV: New seq %u > old %u, clearing old routes to 0x%x", 
                     destSeqNum, routeList[0].destSeqNum, destination);
            routeList.clear();
        }
        
        // Special handling for SDN-authoritative routes (destSeqNum == 0)
        if (!routeList.empty() && destSeqNum == 0) {
            // SDN routes are authoritative - inherit existing seq_num, install as active route with fresh expiry
            uint32_t inheritedSeqNum = routeList[0].destSeqNum;
            uint32_t expiry = millis() + AODV_ACTIVE_ROUTE_TIMEOUT;
            
            // Check if route via this nextHop already exists
            auto existingRoute = std::find_if(routeList.begin(), routeList.end(),
                                             [nextHop](const AODVRouteEntry &r) {
                                                 return r.nextHop == nextHop;
                                             });
            
            if (existingRoute != routeList.end()) {
                // Update existing route and move to front (make active)
                existingRoute->hopCount = hopCount;
                existingRoute->destSeqNum = inheritedSeqNum;
                existingRoute->isValid = true;
                existingRoute->expiryTime = expiry;
                
                if (existingRoute != routeList.begin()) {
                    // Rotate to front
                    std::rotate(routeList.begin(), existingRoute, existingRoute + 1);
                    // Update pathIds: front is now 0, rest increment
                    for (size_t i = 0; i < routeList.size(); i++) {
                        routeList[i].pathId = i;
                    }
                }
                LOG_INFO("AODV SDN ROUTE UPDATE: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u (inherited), pathId=0 (active)", 
                         destination, nextHop, hopCount, inheritedSeqNum);
            } else if (routeList.size() < AODV_MAX_RREQ_PER_ORIGINATOR) {
                // Insert new route at front (pathId=0, active)
                routeList.emplace(routeList.begin(), destination, nextHop, hopCount, inheritedSeqNum, expiry, 0);
                // Update pathIds for displaced routes
                for (size_t i = 1; i < routeList.size(); i++) {
                    routeList[i].pathId = i;
                }
                LOG_INFO("AODV SDN ROUTE ADD: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u (inherited), pathId=0 (active, total paths=%d)", 
                         destination, nextHop, hopCount, inheritedSeqNum, (int)routeList.size());
            } else {
                // Replace worst route (last one) with SDN route at front
                routeList.pop_back();
                routeList.emplace(routeList.begin(), destination, nextHop, hopCount, inheritedSeqNum, expiry, 0);
                // Update pathIds
                for (size_t i = 0; i < routeList.size(); i++) {
                    routeList[i].pathId = i;
                }
                LOG_INFO("AODV SDN ROUTE REPLACE: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u (inherited), pathId=0 (active, replaced last route)", 
                         destination, nextHop, hopCount, inheritedSeqNum);
            }
            return;
        }
        
        // Check if we already have a route via this nextHop
        auto existingRoute = std::find_if(routeList.begin(), routeList.end(),
                                         [nextHop](const AODVRouteEntry &r) {
                                             return r.nextHop == nextHop;
                                         });
        
        if (existingRoute != routeList.end()) {
            // Update existing path (refresh expiry and update metrics if improved)
            if (destSeqNum >= existingRoute->destSeqNum && hopCount <= existingRoute->hopCount) {
                existingRoute->hopCount = hopCount;
                existingRoute->destSeqNum = destSeqNum;
                existingRoute->isValid = true;
                existingRoute->refreshExpiry();
                LOG_INFO("AODV ROUTE UPDATE: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u, pathId=%d (refreshed)", 
                         destination, nextHop, hopCount, destSeqNum, existingRoute->pathId);
            }
        } else if (routeList.size() < AODV_MAX_RREQ_PER_ORIGINATOR) {
            // Add new path (up to 3 total) - first come first serve, no sorting
            // pathId assigned based on arrival order: first=0 (primary), second=1, third=2
            uint8_t newPathId = routeList.size();
            uint32_t expiry = millis() + AODV_ACTIVE_ROUTE_TIMEOUT;
            routeList.emplace_back(destination, nextHop, hopCount, destSeqNum, expiry, newPathId);
            
            LOG_INFO("AODV ROUTE ADD: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u, pathId=%d (total paths=%d)", 
                     destination, nextHop, hopCount, destSeqNum, newPathId, (int)routeList.size());
        } else {
            // Already have 3 paths - ignore new route (first-come-first-serve)
            LOG_DEBUG("AODV ROUTE IGNORE: dest=0x%x, next_hop=0x%x (already have 3 paths)", destination, nextHop);
        }
    } else {
        // No existing routes - add first one
        addRoute(destination, nextHop, hopCount, destSeqNum);
    }
}

void AODVRouteTable::invalidateRoute(uint32_t destination)
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        LOG_INFO("AODV: Invalidated all %d routes to 0x%x", (int)it->second.size(), destination);
        routes.erase(it);
    }
}

void AODVRouteTable::invalidateRoutePath(uint32_t destination, uint8_t nextHop)
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        auto &routeList = it->second;
        auto route = std::find_if(routeList.begin(), routeList.end(),
                                  [nextHop](const AODVRouteEntry &r) {
                                      return r.nextHop == nextHop;
                                  });
        
        if (route != routeList.end()) {
            LOG_INFO("AODV: Invalidated path to 0x%x via 0x%x (pathId=%d)", destination, nextHop, route->pathId);
            routeList.erase(route);
            
            // Update pathId for remaining routes
            for (size_t i = 0; i < routeList.size(); i++) {
                routeList[i].pathId = i;
            }
            
            if (routeList.empty()) {
                routes.erase(it);
            }
        }
    }
}

void AODVRouteTable::refreshRouteOnUse(uint32_t destination)
{
    auto route = findRoute(destination);  // Gets primary route
    if (route) {
        route->extendRouteLifetime();
        LOG_DEBUG("AODV ROUTE KEEPALIVE: dest=0x%x, extended by %ds", 
                 destination, AODV_ROUTE_KEEPALIVE_TIMEOUT / 1000);
    }
}

void AODVRouteTable::removeExpiredRoutes()
{
    for (auto it = routes.begin(); it != routes.end();) {
        auto &routeList = it->second;
        
        // Remove expired routes from the list
        routeList.erase(std::remove_if(routeList.begin(), routeList.end(),
                                      [](const AODVRouteEntry &r) {
                                          return r.isExpired();
                                      }),
                       routeList.end());
        
        // Update pathId
        for (size_t i = 0; i < routeList.size(); i++) {
            routeList[i].pathId = i;
        }
        
        // Remove destination if no valid routes remain
        if (routeList.empty()) {
            LOG_DEBUG("AODV: Removing expired routes to 0x%x", it->first);
            it = routes.erase(it);
        } else {
            ++it;
        }
    }
}

void AODVRouteTable::addPrecursor(uint32_t destination, uint32_t precursorNode)
{
    auto route = findRoute(destination);  // Gets primary route
    if (route) {
        route->precursor = precursorNode;
    }
}

bool AODVRouteTable::activateBackupRoute(uint32_t destination, uint8_t nextHop)
{
    auto it = routes.find(destination);
    if (it == routes.end() || it->second.empty()) {
        LOG_DEBUG("AODV: Cannot activate backup route - no routes to dest=0x%x", destination);
        return false;
    }

    auto &routeList = it->second;
    
    // Clean up expired routes first
    routeList.erase(std::remove_if(routeList.begin(), routeList.end(),
                                  [](AODVRouteEntry &r) {
                                      if (r.isExpired()) {
                                          r.isValid = false;
                                          return true;
                                      }
                                      return false;
                                  }),
                   routeList.end());
    
    if (routeList.empty()) {
        LOG_DEBUG("AODV: Cannot activate backup route - all routes expired to dest=0x%x", destination);
        return false;
    }
    
    // Find the backup route with matching nextHop
    int backupIndex = -1;
    for (size_t i = 0; i < routeList.size(); i++) {
        if (routeList[i].nextHop == nextHop && routeList[i].isValid && !routeList[i].isExpired()) {
            backupIndex = i;
            break;
        }
    }
    
    if (backupIndex == -1) {
        LOG_WARN("AODV: Cannot activate backup route - next_hop=0x%x not found for dest=0x%x", 
                 nextHop, destination);
        return false;
    }
    
    if (backupIndex == 0) {
        LOG_DEBUG("AODV: Route next_hop=0x%x already primary for dest=0x%x", nextHop, destination);
        return true; // Already primary
    }
    
    // Swap backup route to primary position
    uint8_t oldPrimaryNextHop = routeList[0].nextHop;
    std::swap(routeList[0], routeList[backupIndex]);
    
    // Update pathId after swap
    for (size_t i = 0; i < routeList.size(); i++) {
        routeList[i].pathId = i;
    }
    
    LOG_INFO("AODV: Activated backup route for dest=0x%x: next_hop=0x%x (was 0x%x), hops=%u, pathId=%u->0",
             destination, nextHop, oldPrimaryNextHop, routeList[0].hopCount, backupIndex);
    
    return true;
}

bool AODVRouteTable::hasPendingRREQ(uint32_t destination)
{
    auto it = pendingRREQs.find(destination);
    if (it != pendingRREQs.end()) {
        if (!it->second.isExpired()) {
            return true;
        } else {
            // Clean up expired pending RREQ
            pendingRREQs.erase(it);
        }
    }
    return false;
}

void AODVRouteTable::addPendingRREQ(uint32_t destination, uint32_t rreqId)
{
    pendingRREQs[destination] = PendingRREQ(destination, rreqId, AODV_RREQ_RETRIES);
    LOG_DEBUG("AODV: Added pending RREQ for 0x%x, ID=%u", destination, rreqId);
}

void AODVRouteTable::removePendingRREQ(uint32_t destination)
{
    auto it = pendingRREQs.find(destination);
    if (it != pendingRREQs.end()) {
        LOG_DEBUG("AODV: Removed pending RREQ for 0x%x", destination);
        pendingRREQs.erase(it);
    }
}

bool AODVRouteTable::canSendRREQ(uint32_t destination)
{
    auto it = rreqRateLimit.find(destination);
    if (it != rreqRateLimit.end()) {
        uint32_t timeSinceLastRREQ = millis() - it->second;
        return timeSinceLastRREQ >= AODV_RREQ_RATE_LIMIT;
    }
    return true; // No rate limit entry, can send
}

void AODVRouteTable::updateRREQRateLimit(uint32_t destination)
{
    rreqRateLimit[destination] = millis();
}

void AODVRouteTable::bufferPacket(uint32_t destination, meshtastic_MeshPacket *packet)
{
    auto &buffer = packetBuffer[destination];
    
    // Remove oldest packet if buffer is full
    if (buffer.size() >= AODV_MAX_PENDING_PACKETS_PER_DEST) {
        LOG_WARN("AODV: Packet buffer full for 0x%x, dropping oldest", destination);
        packetPool.release(buffer.front().packet);
        buffer.erase(buffer.begin());
    }
    
    buffer.emplace_back(packet);
    LOG_DEBUG("AODV: Buffered packet for 0x%x, buffer size=%d", destination, buffer.size());
}

std::vector<meshtastic_MeshPacket *> AODVRouteTable::getBufferedPackets(uint32_t destination)
{
    std::vector<meshtastic_MeshPacket *> packets;
    auto it = packetBuffer.find(destination);
    if (it != packetBuffer.end()) {
        for (auto &buffered : it->second) {
            packets.push_back(buffered.packet);
        }
    }
    return packets;
}

void AODVRouteTable::clearBufferedPackets(uint32_t destination)
{
    auto it = packetBuffer.find(destination);
    if (it != packetBuffer.end()) {
        LOG_DEBUG("AODV: Clearing %d buffered packets for 0x%x", it->second.size(), destination);
        // Release all buffered packets
        for (auto &buffered : it->second) {
            packetPool.release(buffered.packet);
        }
        packetBuffer.erase(it);
    }
}

void AODVRouteTable::clearBufferedPacketsWithoutFreeing(uint32_t destination)
{
    auto it = packetBuffer.find(destination);
    if (it != packetBuffer.end()) {
        LOG_DEBUG("AODV: Transferring ownership of %d buffered packets for 0x%x", it->second.size(), destination);
        // Just clear the buffer without freeing - caller takes ownership
        packetBuffer.erase(it);
    }
}

void AODVRouteTable::removeExpiredBufferedPackets()
{
    for (auto it = packetBuffer.begin(); it != packetBuffer.end();) {
        auto &buffer = it->second;
        // Remove expired packets from this buffer
        buffer.erase(std::remove_if(buffer.begin(), buffer.end(),
                                   [](const BufferedPacket &bp) {
                                       if (bp.isExpired()) {
                                           LOG_DEBUG("AODV: Removing expired buffered packet");
                                           packetPool.release(bp.packet);
                                           return true;
                                       }
                                       return false;
                                   }),
                    buffer.end());
        
        // Remove empty buffers
        if (buffer.empty()) {
            it = packetBuffer.erase(it);
        } else {
            ++it;
        }
    }
}

void AODVRouteTable::reset()
{
    for (auto &entry : packetBuffer) {
        for (auto &buffered : entry.second) {
            packetPool.release(buffered.packet);
        }
    }

    routes.clear();
    pendingRREQs.clear();
    packetBuffer.clear();
    rreqRateLimit.clear();
    mySeqNum = 1;
    nextRREQId = 1;

    LOG_INFO("AODV: Route table state reset");
}

void AODVRouteTable::dumpRoutes() const
{
    int totalRoutes = 0;
    for (const auto &kv : routes) {
        totalRoutes += kv.second.size();
    }
    
    LOG_INFO("AODV: Route table dump (%d destinations, %d total paths)", (int)routes.size(), totalRoutes);

    for (const auto &kv : routes) {
        for (const auto &r : kv.second) {
            // Avoid negative underflow if expiryTime already passed
            int32_t expiresInMs = (int32_t)(r.expiryTime - millis());
            int32_t expiresInS  = expiresInMs > 0 ? (expiresInMs / 1000) : 0;

            LOG_INFO("  dest=0x%x via=0x%x hops=%u seq=%u pathId=%u valid=%d expires_in=%ds",
                     r.destination, r.nextHop, r.hopCount, r.destSeqNum, r.pathId, 
                     r.isValid ? 1 : 0, (int)expiresInS);
        }
    }
}


void AODVRouteTable::cleanup()
{
    removeExpiredRoutes();
    removeExpiredBufferedPackets();
    
    // Clean up expired pending RREQs
    for (auto it = pendingRREQs.begin(); it != pendingRREQs.end();) {
        if (it->second.isExpired()) {
            LOG_DEBUG("AODV: Removing expired pending RREQ for 0x%x", it->first);
            it = pendingRREQs.erase(it);
        } else {
            ++it;
        }
    }

    dumpRoutes();
}
