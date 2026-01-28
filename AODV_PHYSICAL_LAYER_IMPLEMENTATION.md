# AODV Physical Layer Implementation

## Overview
This document describes the header-based AODV implementation that operates at the physical/link layer instead of the application layer.

## Problem Solved
**Original Issue**: AODV routing information was encoded in the encrypted payload using Protocol Buffers (`RouteDiscovery` messages). This meant intermediate nodes couldn't read RREQ/RREP data without decryption keys, making true on-demand routing impossible.

**Solution**: Move AODV routing information to the **unencrypted packet header** fields that are sent over LoRa radio.

---

## Packet Header Structure (16 bytes)

```c
typedef struct {
    NodeNum to;        // 4 bytes - Destination address
    NodeNum from;      // 4 bytes - Source address  
    PacketId id;       // 4 bytes - Packet ID
    uint8_t flags;     // 1 byte  - Flags (hop_limit, packet type, hop_start)
    uint8_t channel;   // 1 byte  - Channel hash
    uint8_t next_hop;  // 1 byte  - Next hop / AODV-specific data
    uint8_t relay_node;// 1 byte  - Relay node / AODV-specific data
} PacketHeader;
```

### Flags Byte Layout
```
Bits 0-2: hop_limit  (unchanged from original)
Bits 3-4: PACKET TYPE (NEW!)
          00 = DATA packet
          01 = RREQ packet  
          10 = RREP packet
          11 = RERR packet
Bits 5-7: hop_start (unchanged from original)
```

---

## RREQ (Route Request) Packet Format

### Header Fields:
- `to`: `NODENUM_BROADCAST` (0xFFFFFFFF)
- `from`: Originator NodeNum (who initiated the route request)
- `id`: RREQ ID (unique identifier for this route discovery)
- `flags`: Set packet type bits to `01` (RREQ)
- `channel`: Channel hash (as usual)
- `next_hop`: **Destination NodeNum (low byte)** - WHO we're looking for
- `relay_node`: **Hop Count** - starts at 0, incremented by each forwarding node

### Payload (8 bytes, unencrypted):
```c
Bytes 0-3: Originator Sequence Number (uint32_t)
Bytes 4-7: Destination Sequence Number (uint32_t) - 0 if unknown
```

### Packet Identification:
- `decoded.portnum` = `AODV_PORTNUM_RREQ` (250)

### Example RREQ Flow:
1. **Node A** wants to reach **Node Z**
2. Node A sends RREQ:
   ```
   from=0xAAAAAAAA, to=0xFFFFFFFF, id=0x12345678
   next_hop=0xZZ (low byte of Node Z address)
   relay_node=0 (hop count)
   payload=[seqNum_A, seqNum_Z]
   ```
3. **Node B** receives and forwards:
   ```
   from=0xAAAAAAAA (UNCHANGED - keep originator)
   to=0xFFFFFFFF
   id=0x12345678 (UNCHANGED - keep RREQ ID)
   next_hop=0xZZ (UNCHANGED - keep destination)
   relay_node=1 (INCREMENTED - now 1 hop from originator)
   payload=[seqNum_A, seqNum_Z] (UNCHANGED)
   ```
4. **Node C** receives and forwards:
   ```
   relay_node=2 (INCREMENTED again)
   ```
5. **Node Z** matches `next_hop` byte and generates RREP

---

## RREP (Route Reply) Packet Format

### Header Fields:
- `to`: Originator NodeNum (who sent the RREQ)
- `from`: Current node (destination or intermediate node with route)
- `id`: RREQ ID (matches the RREQ being replied to)
- `flags`: Set packet type bits to `10` (RREP)
- `channel`: Channel hash
- `next_hop`: **Next hop towards originator** (reverse route direction)
- `relay_node`: **Hop Count to destination**

### Payload (8 bytes, unencrypted):
```c
Bytes 0-3: Destination Sequence Number (uint32_t)
Bytes 4-7: Destination NodeNum (uint32_t) - full address of final destination
```

### Packet Identification:
- `decoded.portnum` = `AODV_PORTNUM_RREP` (251)

### Example RREP Flow:
1. **Node Z** generates RREP:
   ```
   from=0xZZZZZZZZ, to=0xAAAAAAAA
   id=0x12345678 (same as RREQ)
   next_hop=0xCC (low byte of Node C - reverse route)
   relay_node=0 (0 hops to destination since we ARE the dest)
   payload=[seqNum_Z, 0xZZZZZZZZ]
   ```
2. RREP travels back via reverse route to Node A

---

## Key Implementation Changes

### 1. RadioInterface.h
Added packet type flags and helper macros:
```c
#define PACKET_FLAGS_TYPE_MASK 0x18
#define PACKET_FLAGS_TYPE_SHIFT 3
#define PACKET_TYPE_DATA 0x00
#define PACKET_TYPE_RREQ 0x01
#define PACKET_TYPE_RREP 0x02
#define PACKET_TYPE_RERR 0x03

#define GET_PACKET_TYPE(flags) (((flags) & PACKET_FLAGS_TYPE_MASK) >> PACKET_FLAGS_TYPE_SHIFT)
#define SET_PACKET_TYPE(flags, type) (...)
```

### 2. AODVRouter.h
Added custom port numbers to identify AODV packets:
```c
#define AODV_PORTNUM_RREQ 250
#define AODV_PORTNUM_RREP 251  
#define AODV_PORTNUM_RERR 252
```

### 3. AODVRouter.cpp - sendRREQ()
- Creates packet with header-based RREQ data
- Sets `decoded.portnum = AODV_PORTNUM_RREQ`
- Stores destination in `next_hop`, hop count in `relay_node`
- Payload contains only sequence numbers (unencrypted)

### 4. AODVRouter.cpp - sendRREP()
- New signature: `sendRREP(originatorAddr, destinationAddr, rreqId, destSeqNum, hopCountToDest, nextHopToOrig)`
- Creates packet with header-based RREP data
- Sets `decoded.portnum = AODV_PORTNUM_RREP`
- Stores reverse route in `next_hop`, hop count in `relay_node`

### 5. AODVRouter.cpp - handleRREQ()
- Extracts RREQ data from packet header fields instead of protobuf payload
- Reads sequence numbers from payload bytes
- Creates reverse route to originator
- Forwards RREQ with incremented hop count

### 6. AODVRouter.cpp - sniffReceived()
- Detects AODV packets by checking `decoded.portnum`
- Routes to appropriate handler (handleRREQ/handleRREP)
- No decryption needed for AODV control packets

---

## Advantages of This Approach

1. **No Encryption Issues**: Intermediate nodes can read AODV headers without decryption keys
2. **Smaller Packets**: No protobuf overhead for routing information
3. **True Physical Layer Routing**: AODV operates at the link layer, closer to the radio
4. **Backward Compatible**: Can still use old protobuf-based routing if needed
5. **Efficient**: Routing info in header, only 8 bytes of payload needed

---

## Limitations & Future Work

### Current Limitations:
1. **Destination Address**: Only 1 byte (low byte) stored in `next_hop` field
   - **Risk**: Address collision if multiple nodes have same low byte
   - **Mitigation**: Most mesh networks are small enough that this isn't an issue
   - **TODO**: Could use 2 bytes of payload to store full destination address

2. **Packet Type in Flags**: Not yet implemented in RadioInterface encoding/decoding
   - Currently using `decoded.portnum` to identify packet type
   - **TODO**: Modify `RadioLibInterface::beginSending()` to set flags bits 3-4
   - **TODO**: Modify `RadioLibInterface::handleReceiveInterrupt()` to read packet type

### Next Steps (Not Yet Implemented):

#### Step 1: RadioInterface Integration
Modify `RadioLibInterface.cpp` to:
- **beginSending()**: Read `decoded.portnum`, set packet type in flags byte
- **handleReceiveInterrupt()**: Read flags byte, set appropriate `decoded.portnum`
- Skip encryption for AODV_PORTNUM_RREQ/RREP/RERR packets

#### Step 2: Node Info Handshake (Your Suggestion)
After RREP received, originator should:
1. Send **encrypted NodeInfo packet** to destination via discovered route
2. Exchange encryption keys and node information
3. Then send buffered data packets

This solves the encryption key exchange problem!

#### Step 3: Update handleRREP()
Current handleRREP() still expects old protobuf format. Need to update it to:
- Extract RREP data from header fields
- Read destination info from payload
- Create forward route
- Trigger NodeInfo handshake
- Send buffered packets

---

## Testing Plan

### Phase 1: Header Encoding (Current Status)
- ✅ sendRREQ() creates header-based packets
- ✅ sendRREP() creates header-based packets  
- ✅ handleRREQ() decodes from header
- ⚠️ Need to update handleRREP() for header decoding
- ⚠️ Need RadioInterface integration

### Phase 2: Basic Route Discovery
- Test 2-hop RREQ/RREP without encryption
- Verify reverse routes created correctly
- Verify forward routes created correctly

### Phase 3: NodeInfo Handshake
- Implement handshake after RREP
- Test encryption key exchange
- Verify data packets work after handshake

### Phase 4: Multi-hop Testing
- Test 3+ hop scenarios
- Verify hop count increments correctly
- Verify routes are optimal

---

## Code Files Modified

1. `src/mesh/RadioInterface.h` - Added packet type flags
2. `src/mesh/AODVRouter.h` - Added AODV portnums, updated sendRREP signature
3. `src/mesh/AODVRouter.cpp` - Completely rewrote sendRREQ, sendRREP, handleRREQ, sniffReceived

## Code Files Needing Update

1. `src/mesh/RadioLibInterface.cpp` - Need to integrate packet type encoding/decoding
2. `src/mesh/AODVRouter.cpp` - Need to update handleRREP() for header-based format
3. `src/mesh/AODVRouter.cpp` - Need to implement NodeInfo handshake after RREP

---

## Questions for Review

1. **Is 1 byte enough for destination address?** Or should we use 2 bytes of payload?
2. **When to integrate with RadioInterface?** Need to modify beginSending/handleReceiveInterrupt
3. **NodeInfo handshake timing?** Should it happen immediately after RREP or with a delay?
4. **Backward compatibility?** Should we support old protobuf-based AODV for a transition period?

---

## Summary

This implementation moves AODV from the **application layer** (encrypted protobuf payload) to the **physical layer** (unencrypted packet headers). This allows intermediate nodes to participate in routing without needing encryption keys, enabling true on-demand routing in a mesh network.

The key insight is that **routing information doesn't need encryption** - it needs to be readable by all nodes. Only the **actual data payload** needs encryption, which happens after route discovery via a NodeInfo handshake.
