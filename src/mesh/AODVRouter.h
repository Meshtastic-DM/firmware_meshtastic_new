#pragma once

#include "FloodingRouter.h"
#include <unordered_map>
#include <map>

// AODV Protocol Constants
#define AODV_ACTIVE_ROUTE_TIMEOUT 300000    //5 minutes
#define AODV_NET_TRAVERSAL_TIME 2000      // 2 seconds  
#define AODV_NODE_TRAVERSAL_TIME 40       // 40 milliseconds
#define AODV_RREQ_RETRIES 2               // Number of RREQ retries
#define AODV_RREQ_RATELIMIT 10000         // 10 seconds between RREQs for same destination
#define AODV_MAX_REPAIR_TTL 3             // Maximum TTL for local repair

/**
 * AODV Route Entry
 */
struct AODVRouteEntry {
    NodeNum destination;          // Destination node
    uint32_t destSeqNum;          // Destination sequence number
    bool validDestSeqNum;         // Is destination sequence number valid
    uint8_t hopCount;             // Number of hops to destination
    NodeNum nextHop;              // Next hop towards destination
    uint32_t lifetime;            // Route lifetime (milliseconds from epoch)
    uint32_t lastUsedTime;        // Last time route was used
    bool routeValid;              // Is this route currently valid
    
    AODVRouteEntry() : destination(0), destSeqNum(0), validDestSeqNum(false),
                       hopCount(255), nextHop(0), lifetime(0), 
                       lastUsedTime(0), routeValid(false) {}
};

/**
 * AODV RREQ (Route Request) Cache Entry
 */
struct RREQCacheEntry {
    NodeNum originator;           // RREQ originator
    uint32_t rreqId;              // RREQ ID
    uint32_t timestamp;           // When we saw this RREQ
    
    RREQCacheEntry() : originator(0), rreqId(0), timestamp(0) {}
    
    RREQCacheEntry(NodeNum orig, uint32_t id, uint32_t ts) 
        : originator(orig), rreqId(id), timestamp(ts) {}
};

/**
 * AODV RREQ ID Cache Key
 */
struct RREQCacheKey {
    NodeNum originator;
    uint32_t rreqId;
    
    bool operator==(const RREQCacheKey &other) const {
        return originator == other.originator && rreqId == other.rreqId;
    }
};

/**
 * Hash function for RREQ Cache Key
 */
struct RREQCacheKeyHash {
    size_t operator()(const RREQCacheKey &k) const {
        return std::hash<NodeNum>()(k.originator) ^ std::hash<uint32_t>()(k.rreqId);
    }
};

/**
 * Pending RREQ Entry
 */
struct PendingRREQ {
    NodeNum destination;
    uint32_t rreqId;
    uint8_t ttl;
    uint8_t retryCount;
    uint32_t nextRetryTime;
    meshtastic_MeshPacket *bufferedPacket;  // Packet waiting for route
    
    PendingRREQ() : destination(0), rreqId(0), ttl(1), retryCount(0), 
                    nextRetryTime(0), bufferedPacket(nullptr) {}
};

/**
 * AODVRouter - Implementation of Ad-hoc On-Demand Distance Vector Routing
 * 
 * This router implements the AODV routing protocol for mesh networks.
 * Key features:
 * - On-demand route discovery using RREQ/RREP
 * - Sequence numbers for loop prevention and freshness
 * - Route maintenance with RERR messages
 * - Local repair capability
 * - Precursor lists for efficient RERR propagation
 */
class AODVRouter : public FloodingRouter
{
  public:
    /**
     * Constructor
     */
    AODVRouter();
    
    /**
     * Enable or disable AODV routing at runtime
     * When disabled, falls back to pure flooding
     */
    void setAODVEnabled(bool enabled) { aodvEnabled = enabled; }
    bool isAODVEnabled() const { return aodvEnabled; }

    /**
     * Send a packet
     */
    virtual ErrorCode send(meshtastic_MeshPacket *p) override;

    /**
     * Do periodic maintenance (route timeouts, retries, etc.)
     */
    virtual int32_t runOnce() override;

  protected:
    /**
     * Should this incoming packet be dropped?
     */
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *p) override;

    /**
     * Process received packets for AODV routing information
     */
    virtual void sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c) override;

    /**
     * Check if we should rebroadcast this packet
     */
    virtual bool perhapsRebroadcast(const meshtastic_MeshPacket *p) override;

  private:
    // Routing table: destination -> route entry
    std::map<NodeNum, AODVRouteEntry> routingTable;
    
    // RREQ cache to prevent processing duplicate RREQs
    std::unordered_map<RREQCacheKey, RREQCacheEntry, RREQCacheKeyHash> rreqCache;
    
    // Pending RREQs waiting for RREPs
    std::map<NodeNum, PendingRREQ> pendingRREQs;
    
    // Sequence number for this node
    uint32_t sequenceNumber;
    
    // RREQ ID counter
    uint32_t rreqIdCounter;
    
    // Flag to enable/disable AODV (can be toggled at runtime)
    bool aodvEnabled;
    
    // Last time we printed the routing table (for periodic logging)
    uint32_t lastRoutingTablePrint;

    // Route Discovery Methods
    void initiateRouteDiscovery(meshtastic_MeshPacket *p);
    void sendRREQ(NodeNum destination, uint8_t ttl);
    void handleRREQ(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing);
    void handleRREP(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing);
    void handleRERR(const meshtastic_MeshPacket *p, const meshtastic_Routing *routing);
    
    // Route Management Methods
    AODVRouteEntry* findRoute(NodeNum destination);
    void addRoute(NodeNum destination, NodeNum nextHop, uint8_t hopCount, 
                  uint32_t destSeqNum, uint32_t lifetime);
    void updateRoute(NodeNum destination, NodeNum nextHop, uint8_t hopCount,
                     uint32_t destSeqNum, uint32_t lifetime);
    void invalidateRoute(NodeNum destination);
    void deleteExpiredRoutes();
    
    // Utility Methods
    bool isRREQCached(NodeNum originator, uint32_t rreqId);
    void addRREQToCache(NodeNum originator, uint32_t rreqId);
    void cleanRREQCache();
    void processPendingRREQs();
    void sendBufferedPacket(NodeNum destination);
    uint32_t getNextRREQId();
    uint32_t getNextSequenceNumber();
    
    // Helper to send AODV control messages
    void sendAODVMessage(const meshtastic_Routing *routing, NodeNum to, 
                         uint8_t hopLimit = HOP_RELIABLE);
    
    // Forward packet using discovered route
    ErrorCode forwardWithRoute(meshtastic_MeshPacket *p, const AODVRouteEntry *route);
    
    // Print routing table to serial monitor (for debugging)
    void printRoutingTable();
};
