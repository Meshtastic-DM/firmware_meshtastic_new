#pragma once

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/sdn.pb.h"
#include <map>

/**
 * Per-relay reception statistics for PDR calculation
 */
struct NeighborLinkStats {
    uint8_t relayNode;        // Last byte of relay node ID
    uint32_t rxGood;          // Successful receptions from this relay
    uint32_t rxBad;           // Failed receptions from this relay
    uint32_t lastReportTime;  // millis() when last reported
    
    NeighborLinkStats() : relayNode(0), rxGood(0), rxBad(0), lastReportTime(0) {}
    
    float getPDR() const {
        uint32_t total = rxGood + rxBad;
        return total > 0 ? (float)rxGood / total : 0.0f;
    }
    
    uint32_t getTotalPackets() const {
        return rxGood + rxBad;
    }
};

/**
 * SDN (Software Defined Networking) Module
 * 
 * Allows a controller node to:
 * - Broadcast periodic announcements with HMAC authentication
 * - Collect route information from mesh nodes
 * - Distribute its public key for future secure communication
 */
class SDNModule : public ProtobufModule<meshtastic_SDN>, private concurrency::OSThread
{
  private:
    // Configuration
    bool isSDNController;              // Is this node an SDN controller?
    uint32_t announcementInterval;     // Seconds between announcements
    
    // Controller tracking
    uint32_t sdnControllerNode;        // Known SDN controller node ID (0 = unknown)
    uint8_t sdnPublicKey[32];          // SDN controller's public key
    bool sdnAuthenticated;             // Has controller been authenticated?
    
    // Announcement state (for controller nodes)
    uint32_t lastAnnouncementTime;     // Last time we sent announcement (millis())
    
    // HMAC secret for authentication
    uint8_t hmacSecret[32];            // Shared secret for HMAC verification
    size_t hmacSecretLen;              // Length of secret
    
    // Route installation tracking
    uint8_t nextInstallId;             // Next installation ID (0-255, auto-wraps)
    
    // Per-relay link quality tracking
    std::map<uint8_t, NeighborLinkStats> neighborStats;
    uint32_t linkQualityReportInterval;   // Seconds between reports (default 300)
    uint32_t lastLinkQualityReport;       // millis() of last report
    uint32_t minRelayNodesForReporting;   // Min relay nodes to trigger reporting (default 2)
    
  public:
    SDNModule();
    
    /**
     * Handle received SDN protobuf messages
     */
    bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_SDN *sdn) override;
    
    /**
     * Handle SDN announcement from controller
     */
    void handleSDNAnnouncement(const meshtastic_MeshPacket &mp, const meshtastic_SDNAnnouncement &ann);
    
    /**
     * Handle route update sent to controller
     */
    void handleSDNRouteUpdate(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteUpdate &update);
    
    /**
     * Handle route command from controller
     */
    void handleSDNRouteCommand(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteCommand &cmd);
    
    /**
     * Handle route installation command from controller
     */
    void handleSDNRouteInstall(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteInstall &install);
    
    /**
     * Handle route set message (cascading hop-by-hop)
     */
    void handleSDNRouteSet(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteSet &routeSet);
    
    /**
     * Handle route set confirmation from destination
     */
    void handleSDNRouteSetConfirm(const meshtastic_MeshPacket &mp, const meshtastic_SDNRouteSetConfirm &confirm);
    
    /**
     * Handle link quality metrics from nodes
     */
    void handleSDNLinkQuality(const meshtastic_MeshPacket &mp, const meshtastic_SDNLinkQuality &lq);
    
    /**
     * Send announcement broadcast (controller only)
     */
    void sendAnnouncement();
    
    /**
     * Send route update to SDN controller
     */
    void sendRouteUpdate(uint32_t destination, uint8_t nextHop, uint8_t hopCount, uint32_t destSeqNum);
    
    /**
     * Send route command to target node (activate backup path)
     */
    void sendRouteCommand(uint32_t targetNode, uint32_t destination, uint8_t nextHop);
    
    /**
     * Send route installation command to start node
     * Initiates cascading route setup through specified hop path
     * @param destination Final destination node (32-bit full node ID)
     * @param hopPath Vector of 1-byte node IDs (max 8 hops)
     */
    void sendRouteInstall(uint32_t destination, const std::vector<uint8_t> &hopPath);
    
    /**
     * Record packet reception from relay node (called from RadioLibInterface)
     */
    void recordReception(uint8_t relayNode, bool success);
    
    /**
     * Send link quality reports to SDN controller
     * Only sends if node has 2+ active relay nodes (routing participant)
     */
    void sendLinkQualityReports();
    
    /**
     * Get count of unique relay nodes seen
     */
    size_t getActiveRelayCount() const { return neighborStats.size(); }
    
    /**
     * Check if SDN controller is authenticated
     */
    bool isControllerAuthenticated() const { return sdnAuthenticated && sdnControllerNode != 0; }
    
    /**
     * Get SDN controller node number
     */
    uint32_t getSDNController() const { return sdnControllerNode; }
    
    /**
     * Get SDN controller's public key
     */
    const uint8_t* getControllerPublicKey() const { return sdnPublicKey; }
    
  protected:
    /**
     * Periodic thread execution - sends announcements if controller
     */
    virtual int32_t runOnce() override;
    
  private:
    /**
     * Generate HMAC-SHA256 hash (first 16 bytes)
     */
    void generateHMAC(const uint8_t *secret, size_t secretLen, uint32_t timestamp, uint8_t *output);
    
    /**
     * Verify HMAC hash
     */
    bool verifyHMAC(const uint8_t *secret, size_t secretLen, uint32_t timestamp, const uint8_t *hash);
    
    /**
     * Install SDN controller's public key as admin key and enable remote administration
     * Note: SDN controller uses admin_key[0] (slot 0) for priority access
     */
    void installAdminKey();
};

extern SDNModule *sdnModule;
