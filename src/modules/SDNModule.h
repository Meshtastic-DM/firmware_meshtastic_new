#pragma once

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/sdn.pb.h"
#include <map>

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
