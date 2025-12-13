# AODV Multi-Hop Fix - Path Tracking Correction

## Problem Description

The AODV implementation was failing for routes with more than 2 hops because:

1. **RREP packets didn't carry the RREQ originator address** in the payload
2. Intermediate nodes couldn't determine where to forward RREP packets
3. RREP forwarding relied on `p->to` field which could be unreliable after multiple hops
4. Routing tables were incomplete or incorrect for multi-hop scenarios

## Root Cause

### Original RREP Format (BROKEN):
```cpp
rrep->route[0] = rreqId;           // RREQ ID
rrep->route[1] = destSeqNum;       // Destination sequence number
rrep->route[2] = hopCount;         // Hop count
rrep->route_count = 3;             // Only 3 fields
```

### Problem Scenario (4+ nodes):
```
Node A → Node B → Node C → Node D

RREQ Flow (works fine):
  A sends RREQ (hopCount=0)
  B receives, creates reverse route to A, forwards (hopCount=1)
  C receives, creates reverse route to A via B, forwards (hopCount=2)
  D receives, creates reverse route to A via C, sends RREP

RREP Flow (BROKEN):
  D sends RREP with p->to = A
  C receives RREP, needs to forward to A
    ❌ Problem: C uses p->to (=A) to lookup route
    ❌ But intermediate nodes might not have direct info
    ❌ RREP gets lost or misrouted
```

## Solution

### Fixed RREP Format:
```cpp
rrep->route[0] = rreqId;           // RREQ ID
rrep->route[1] = destSeqNum;       // Destination sequence number  
rrep->route[2] = hopCount;         // Hop count
rrep->route[3] = originator;       // ✅ NEW: RREQ originator address
rrep->route_count = 4;             // ✅ Updated to 4 fields
```

## Changes Made

### 1. RREP Creation (Destination Node)

**File:** `src/mesh/AODVRouter.cpp` (Line ~187-195)

```cpp
// When destination creates RREP
meshtastic_RouteDiscovery *rrep = &replyRouting.route_reply;
rrep->route[0] = rreqId;
rrep->route[1] = getNextSequenceNumber();
rrep->route[2] = 0;
rrep->route[3] = originator;  // ✅ Added: Store RREQ originator
rrep->route_count = 4;        // ✅ Changed from 3 to 4
```

### 2. RREP Creation (Intermediate Node with Route)

**File:** `src/mesh/AODVRouter.cpp` (Line ~203-211)

```cpp
// When intermediate node sends RREP
meshtastic_RouteDiscovery *rrep = &replyRouting.route_reply;
rrep->route[0] = rreqId;
rrep->route[1] = destRoute->destSeqNum;
rrep->route[2] = destRoute->hopCount;
rrep->route[3] = originator;  // ✅ Added: Store RREQ originator
rrep->route_count = 4;        // ✅ Changed from 3 to 4
```

### 3. RREP Reception and Validation

**File:** `src/mesh/AODVRouter.cpp` (Line ~283-290)

```cpp
void AODVRouter::handleRREP(...)
{
    const meshtastic_RouteDiscovery *rrep = &routing->route_reply;
    
    // ✅ Updated validation
    if (rrep->route_count < 4) {
        LOG_WARN("AODV: Invalid RREP format (route_count=%d, expected 4)", 
                 rrep->route_count);
        return;
    }
    
    uint32_t rreqId = rrep->route[0];
    uint32_t destSeqNum = rrep->route[1];
    uint8_t hopCount = (uint8_t)rrep->route[2];
    NodeNum rreqOriginator = rrep->route[3];  // ✅ Extract originator
}
```

### 4. RREP Forwarding Logic

**File:** `src/mesh/AODVRouter.cpp` (Line ~320-335)

```cpp
// ✅ Fixed forwarding - use originator from payload
if (rreqOriginator != nodeDB->getNodeNum() && p->hop_limit > 1) {
    // Look up route to RREQ originator (not p->to)
    AODVRouteEntry *route = findRoute(rreqOriginator);
    
    if (route && route->routeValid) {
        LOG_INFO("AODV: Forwarding RREP to originator 0x%x via 0x%x", 
                 rreqOriginator, route->nextHop);
        
        meshtastic_Routing fwdRouting = meshtastic_Routing_init_zero;
        fwdRouting.which_variant = meshtastic_Routing_route_reply_tag;
        fwdRouting.route_reply = *rrep;
        fwdRouting.route_reply.route[2] = hopCount + 1;
        
        // ✅ Send to originator (from payload, not p->to)
        sendAODVMessage(&fwdRouting, rreqOriginator, p->hop_limit - 1);
    }
}
```

## How It Works Now

### Corrected Flow (4-node scenario):

```
Node A (0x0000) → Node B (0x0001) → Node C (0x0002) → Node D (0x0003)

═══════════════════════════════════════════════════════════════════
RREQ PHASE (Route Discovery)
═══════════════════════════════════════════════════════════════════

1. Node A sends RREQ:
   - Broadcast to all
   - rreq->route[3] = 0 (hopCount)
   - rreq->route[5] = 0x0003 (destination)

2. Node B receives RREQ:
   - Creates reverse route: A is 1 hop away (direct)
   - Increments hopCount to 1
   - Rebroadcasts RREQ

3. Node C receives RREQ:
   - Creates reverse route: A is 2 hops away via B
   - Increments hopCount to 2
   - Rebroadcasts RREQ

4. Node D receives RREQ:
   - Creates reverse route: A is 3 hops away via C
   - Recognizes it's the destination
   - Sends RREP

═══════════════════════════════════════════════════════════════════
RREP PHASE (Route Reply) - ✅ NOW WORKS CORRECTLY
═══════════════════════════════════════════════════════════════════

5. Node D creates RREP:
   - rrep->route[0] = rreqId
   - rrep->route[1] = seqNum (D's sequence)
   - rrep->route[2] = 0 (hopCount from D)
   - rrep->route[3] = 0x0000 (✅ RREQ originator = A)
   - p->to = 0x0000 (A)
   - Sends to A

6. Node C receives RREP:
   - Creates forward route: D is 1 hop away (direct)
   - Reads originator from rrep->route[3] = 0x0000 (A)
   - ✅ Looks up route to 0x0000 → finds "via B"
   - Increments hopCount to 1
   - Forwards RREP to A (via B)

7. Node B receives RREP:
   - Creates forward route: D is 2 hops away via C
   - Reads originator from rrep->route[3] = 0x0000 (A)
   - ✅ Looks up route to 0x0000 → finds "direct"
   - Increments hopCount to 2
   - Forwards RREP to A

8. Node A receives RREP:
   - Creates forward route: D is 3 hops away via B
   - ✅ Route discovery complete!
   - Sends buffered data packet to D
```

## Final Routing Tables

**Node A (0x0000):**
```
Destination: 0x0003 (D)
Next Hop:    0x0001 (B)
Hop Count:   3
Valid:       YES
```

**Node B (0x0001):**
```
Reverse Route:
  Destination: 0x0000 (A)
  Next Hop:    0x0000 (direct)
  Hop Count:   1

Forward Route:
  Destination: 0x0003 (D)
  Next Hop:    0x0002 (C)
  Hop Count:   2
```

**Node C (0x0002):**
```
Reverse Route:
  Destination: 0x0000 (A)
  Next Hop:    0x0001 (B)
  Hop Count:   2

Forward Route:
  Destination: 0x0003 (D)
  Next Hop:    0x0003 (direct)
  Hop Count:   1
```

**Node D (0x0003):**
```
Reverse Route:
  Destination: 0x0000 (A)
  Next Hop:    0x0002 (C)
  Hop Count:   3
```

## Testing

### Test Scenarios

1. **2-Hop Path**: A → B → C (basic case)
2. **3-Hop Path**: A → B → C → D (multi-hop)
3. **4+ Hop Path**: Extended chains
4. **Mesh Topology**: Multiple paths available
5. **Client Mode**: All nodes in CLIENT role

### Expected Behavior

✅ **RREQ reaches destination** regardless of hop count  
✅ **RREP returns to originator** via reverse path  
✅ **Routing tables correctly populated** at all nodes  
✅ **Data packets flow** using discovered routes  
✅ **Works in CLIENT mode** (not just ROUTER mode)

### Debug Logs to Check

```
AODV: Received RREQ id=X from 0xAAAA to 0xBBBB (hops=N)
AODV: Creating reverse route: dest=0xAAAA, nextHop=0xCCCC, hops=N+1
AODV: Received RREP from 0xBBBB (hops=M, originator=0xAAAA)
AODV: Creating forward route: dest=0xBBBB, nextHop=0xDDDD, hops=M+1
AODV: Forwarding RREP to originator 0xAAAA via 0xEEEE
AODV: Route discovery complete for 0xBBBB
AODV: Sending buffered packet id=123 to 0xBBBB
```

## Backward Compatibility

❌ **NOT backward compatible** with old AODV implementation  
- Old nodes expect `route_count = 3`
- New nodes send `route_count = 4`
- Mixed deployments will fail

**Solution**: Update all AODV-enabled nodes to this version

## Benefits

✅ **Reliable multi-hop routing** (4+ hops supported)  
✅ **Correct path tracking** via originator field  
✅ **Proper RREP forwarding** using reverse routes  
✅ **Better debugging** with enhanced logging  
✅ **Client mode compatible** for all node types

## Technical Details

### RREP Payload Encoding

```
Offset  Field           Type      Description
------  -----           ----      -----------
0       RREQ ID         uint32    Identifies matching RREQ
1       Dest Seq Num    uint32    Destination's sequence number
2       Hop Count       uint32    Distance to destination
3       Originator      uint32    ✅ RREQ originator node ID
```

### Key Invariants

1. **RREQ originator never changes** throughout RREP forwarding
2. **Hop count increments** at each RREP hop
3. **Reverse routes exist** before RREP forwarding
4. **relay_node tracks** immediate previous sender

## Date

December 8, 2025

## Author

AODV Multi-Hop Fix for Meshtastic Firmware
