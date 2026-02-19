#pragma once

#include "ProtobufModule.h"
#include "AODVRouteTable.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/aodv.pb.h"
#include <map>
#include <tuple>

/*
 * AODVModule - Handles AODV routing protocol messages
 * Processes RREQ, RREP, and RERR packets for dynamic route discovery
 */
class AODVModule : public ProtobufModule<meshtastic_AODV>, public concurrency::OSThread
{
  private:
    AODVRouteTable routeTable;
    
    // Track seen RREQs at target node to prevent processing duplicates and enforce per-originator limit
    // Outer Key: originator node number
    // Inner Key: dest_seq_num
    // Value: pair of (response_seq_num, vector of (relay_node, timestamp) pairs - max 3 relay paths per dest_seq_num)
    std::map<uint32_t, std::map<uint32_t, std::pair<uint32_t, std::vector<std::pair<uint8_t, uint32_t>>>>> seenRREQs;
    
    // Cleanup interval for route table maintenance
    uint32_t lastCleanupTime;

    // RREQ Processing
    void handleRouteRequest(const meshtastic_MeshPacket &mp, const meshtastic_RouteRequest &rreq);
    bool hasSeenRREQ(uint32_t originator, uint32_t destSeqNum, uint8_t relayNode);
    uint32_t markRREQAsSeen(uint32_t originator, uint32_t destSeqNum, uint8_t relayNode);
    void forwardRREQ(const meshtastic_MeshPacket &receivedPacket, const meshtastic_RouteRequest &rreq);
    void sendRREP(uint32_t originator, uint32_t destination, uint32_t destSeqNum, uint8_t hopCount, uint8_t nextHop);

    // RREP Processing
    void handleRouteReply(const meshtastic_MeshPacket &mp, const meshtastic_RouteReply &rrep);
    void forwardRREP(const meshtastic_MeshPacket &receivedPacket, const meshtastic_RouteReply &rrep, uint32_t originator);
    void deliverBufferedPackets(uint32_t destination);

    // RERR Processing
    void handleRouteError(const meshtastic_MeshPacket &mp, const meshtastic_RouteError &rerr);
    void forwardRERR(const meshtastic_RouteError &rerr);

    // Cleanup
    void cleanupSeenRREQs();

  public:
    AODVModule();

    // Route table access for NextHopRouter
    AODVRouteTable *getRouteTable() { return &routeTable; }

    // Initiate route discovery
    void initiateRouteDiscovery(uint32_t destination, meshtastic_MeshPacket *packet);

    // Called when link failure detected (no ACK after retries)
    void handleLinkFailure(uint32_t destination);

    // MeshModule overrides
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_AODV *aodv) override;
    virtual meshtastic_MeshPacket *allocReply() override;

    // OSThread override for periodic cleanup
    virtual int32_t runOnce() override;

  protected:
    virtual void alterReceived(meshtastic_MeshPacket &p) override {}
};

extern AODVModule *aodvModule;
