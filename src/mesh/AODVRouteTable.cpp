#include "AODVRouteTable.h"
#include "NodeDB.h"
#include "RTC.h"
#include "configuration.h"
#include <algorithm>

AODVRouteTable::AODVRouteTable() : mySeqNum(1), nextRREQId(1) {}

AODVRouteEntry *AODVRouteTable::findRoute(uint32_t destination)
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        auto &route = it->second;
        if (route.isValid && !route.isExpired()) {
            return &route;
        } else if (route.isExpired()) {
            // Mark as invalid if expired
            route.isValid = false;
        }
    }
    return nullptr;
}

bool AODVRouteTable::hasValidRoute(uint32_t destination)
{
    return findRoute(destination) != nullptr;
}

void AODVRouteTable::addRoute(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum)
{
    uint32_t expiry = millis() + AODV_ACTIVE_ROUTE_TIMEOUT;
    routes[destination] = AODVRouteEntry(destination, nextHop, hopCount, destSeqNum, expiry);
    LOG_INFO("AODV ROUTE ADD: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u, expires_in=%ds", 
             destination, nextHop, hopCount, destSeqNum, AODV_ACTIVE_ROUTE_TIMEOUT / 1000);
}

void AODVRouteTable::updateRoute(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum)
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        auto &route = it->second;
        // Only update if new sequence number is higher, or same seq but lower hop count
        if (destSeqNum > route.destSeqNum || (destSeqNum == route.destSeqNum && hopCount < route.hopCount)) {
            route.nextHop = nextHop;
            route.hopCount = hopCount;
            route.destSeqNum = destSeqNum;
            route.isValid = true;
            route.refreshExpiry();
            LOG_INFO("AODV ROUTE UPDATE: dest=0x%x, next_hop=0x%x, hops=%d, seq=%u (improved)", 
                     destination, nextHop, hopCount, destSeqNum);
        } else {
            // Just refresh expiry if route info hasn't improved
            route.refreshExpiry();
            LOG_DEBUG("AODV ROUTE REFRESH: dest=0x%x, expiry refreshed", destination);
        }
    } else {
        addRoute(destination, nextHop, hopCount, destSeqNum);
    }
}

void AODVRouteTable::invalidateRoute(uint32_t destination)
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        it->second.isValid = false;
        LOG_INFO("AODV: Invalidated route to 0x%x", destination);
    }
}

void AODVRouteTable::removeExpiredRoutes()
{
    for (auto it = routes.begin(); it != routes.end();) {
        if (it->second.isExpired()) {
            LOG_DEBUG("AODV: Removing expired route to 0x%x", it->first);
            it->second.isValid = false;
            ++it;
        } else {
            ++it;
        }
    }
}

void AODVRouteTable::addPrecursor(uint32_t destination, uint8_t precursorNode)
{
    auto it = routes.find(destination);
    if (it != routes.end()) {
        it->second.precursors.insert(precursorNode);
    }
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
}
