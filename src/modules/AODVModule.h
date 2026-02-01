#pragma once

#include "ProtobufModule.h"
#include "AODVRouteTable.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/aodv.pb.h"
#include <map>

/*
 * AODVModule - Handles AODV routing protocol messages
 * Processes RREQ, RREP, and RERR packets for dynamic route discovery
 */
class AODVModule : public ProtobufModule<meshtastic_AODV>, public concurrency::OSThread
{
  private:
    AODVRouteTable routeTable;
    
    // Track seen RREQs to prevent processing duplicates: (originator, rreq_id) -> timestamp
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> seenRREQs;
    
    // Cleanup interval for route table maintenance
    uint32_t lastCleanupTime;

    // RREQ Processing
    void handleRouteRequest(const meshtastic_MeshPacket &mp, const meshtastic_RouteRequest &rreq);
    bool hasSeenRREQ(uint32_t originator, uint32_t rreqId);
    void markRREQAsSeen(uint32_t originator, uint32_t rreqId);
    void forwardRREQ(const meshtastic_RouteRequest &rreq, uint8_t hopCount);
    void sendRREP(uint32_t originator, uint32_t destination, uint32_t destSeqNum, uint8_t hopCount);

    // RREP Processing
    void handleRouteReply(const meshtastic_MeshPacket &mp, const meshtastic_RouteReply &rrep);
    void forwardRREP(const meshtastic_RouteReply &rrep, uint32_t originator);
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
