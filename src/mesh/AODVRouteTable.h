#pragma once

#include "NodeDB.h"
#include "configuration.h"
#include <map>
#include <set>
#include <vector>

// AODV Configuration Constants
#define AODV_ACTIVE_ROUTE_TIMEOUT 300000      // 5 minutes - route lifetime in ms
#define AODV_ROUTE_KEEPALIVE_TIMEOUT 120000   // 2 minutes - extend route when actively used
#define AODV_RREQ_RETRIES 3                  // Maximum RREQ retransmissions
#define AODV_RREQ_RATE_LIMIT 1000            // Minimum 1s between RREQs for same destination
#define AODV_NET_TRAVERSAL_TIME 10000         // 10s - estimated time to traverse network
#define AODV_MAX_PENDING_PACKETS_PER_DEST 5  // Buffer limit per destination
#define AODV_MAX_RREQ_PER_ORIGINATOR 3        // Maximum RREQs to process per originator at destination
#define AODV_ROUTE_CLEANUP_INTERVAL 60000     // 60s - periodic route table cleanup

/*
 * Represents a single route entry in the AODV routing table
 */
struct AODVRouteEntry {
    uint32_t destination;        // Destination node number
    uint8_t nextHop;            // Next hop (8-bit node ID)
    uint8_t hopCount;           // Number of hops to destination
    uint32_t destSeqNum;        // Destination sequence number
    uint32_t expiryTime;        // Timestamp when route expires (millis())
    bool isValid;               // Route validity flag
    uint32_t precursor;         // Single node that uses this route (for RERR)
    uint8_t pathId;             // Path identifier: 0=primary, 1=backup1, 2=backup2

    AODVRouteEntry()
        : destination(0), nextHop(0), hopCount(255), destSeqNum(0), expiryTime(0), isValid(false),
          precursor(NODENUM_BROADCAST), pathId(0)
    {
    }

    AODVRouteEntry(uint32_t dest, uint8_t next, uint8_t hops, uint32_t seqNum, uint32_t expiry, uint8_t path = 0)
        : destination(dest), nextHop(next), hopCount(hops), destSeqNum(seqNum), expiryTime(expiry), isValid(true),
          precursor(NODENUM_BROADCAST), pathId(path)
    {
    }

    // Check if route is still valid
    bool isExpired() const { return millis() > expiryTime; }

    // Extend route lifetime (full timeout - used on route updates)
    void refreshExpiry() { expiryTime = millis() + AODV_ACTIVE_ROUTE_TIMEOUT; }
    
    // Extend route lifetime when actively used (keep-alive)
    void extendRouteLifetime() { expiryTime = millis() + AODV_ROUTE_KEEPALIVE_TIMEOUT; }
};

/*
 * Tracks pending RREQ broadcasts to prevent flooding
 */
struct PendingRREQ {
    uint32_t destination;
    uint32_t rreqId;
    uint32_t expiryTime;
    uint8_t retriesLeft;

    PendingRREQ() : destination(0), rreqId(0), expiryTime(0), retriesLeft(0) {}

    PendingRREQ(uint32_t dest, uint32_t id, uint8_t retries)
        : destination(dest), rreqId(id), expiryTime(millis() + AODV_NET_TRAVERSAL_TIME), retriesLeft(retries)
    {
    }

    bool isExpired() const { return millis() > expiryTime; }
};

/*
 * Buffers packets waiting for route discovery
 */
struct BufferedPacket {
    meshtastic_MeshPacket *packet;
    uint32_t timestamp;

    BufferedPacket(meshtastic_MeshPacket *p) : packet(p), timestamp(millis()) {}

    bool isExpired() const { return millis() - timestamp > AODV_NET_TRAVERSAL_TIME * 2; }
};

/*
 * AODV Routing Table - manages routes and route discovery
 */
class AODVRouteTable
{
  private:
    std::map<uint32_t, std::vector<AODVRouteEntry>> routes;  // destination -> vector of route entries (up to 3)
    std::map<uint32_t, PendingRREQ> pendingRREQs;            // destination -> pending RREQ
    std::map<uint32_t, std::vector<BufferedPacket>> packetBuffer; // destination -> buffered packets
    std::map<uint32_t, uint32_t> rreqRateLimit;              // destination -> last RREQ time

    uint32_t mySeqNum;  // Our own sequence number
    uint32_t nextRREQId; // Counter for RREQ IDs

  public:
    AODVRouteTable();

    // Route lookup and management
    AODVRouteEntry *findRoute(uint32_t destination);  // Returns primary route only
    std::vector<AODVRouteEntry> *getAllRoutes(uint32_t destination); // Returns all routes for destination
    bool hasValidRoute(uint32_t destination);
    void addRoute(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum);
    void updateRoute(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum);
    void invalidateRoute(uint32_t destination);  // Invalidates all paths
    void invalidateRoutePath(uint32_t destination, uint8_t nextHop); // Invalidates specific path
    void refreshRouteOnUse(uint32_t destination);  // Extend route expiry when actively used
    void removeExpiredRoutes();
    void addPrecursor(uint32_t destination, uint32_t precursorNode);
    size_t getRoutePathCount(uint32_t destination) const; // Returns number of paths for destination
    bool activateBackupRoute(uint32_t destination, uint8_t nextHop); // Activate backup route as primary

    // Sequence number management
    uint32_t getMySeqNum() { return mySeqNum; }
    uint32_t incrementMySeqNum() { return ++mySeqNum; }

    // RREQ management
    uint32_t getNextRREQId() { return ++nextRREQId; }
    bool hasPendingRREQ(uint32_t destination);
    void addPendingRREQ(uint32_t destination, uint32_t rreqId);
    void removePendingRREQ(uint32_t destination);
    bool canSendRREQ(uint32_t destination); // Rate limiting check
    void updateRREQRateLimit(uint32_t destination);

    // Packet buffering
    void bufferPacket(uint32_t destination, meshtastic_MeshPacket *packet);
    std::vector<meshtastic_MeshPacket *> getBufferedPackets(uint32_t destination);
    void clearBufferedPackets(uint32_t destination);
    void clearBufferedPacketsWithoutFreeing(uint32_t destination); // Transfer ownership without freeing
    void removeExpiredBufferedPackets();
    void reset();

    // Utility
    void cleanup(); // Periodic cleanup of expired entries
    void dumpRoutes() const; // Debug: dump all routes to log
    size_t getRouteCount() const { return routes.size(); }
};
