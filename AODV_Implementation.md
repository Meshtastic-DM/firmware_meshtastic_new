# AODV Routing Protocol Implementation in Meshtastic

## Overview

Meshtastic implements **Ad hoc On-Demand Distance Vector (AODV)** routing to enable efficient multi-hop unicast communication in the mesh network. Unlike flooding (used for broadcasts), AODV creates optimized routes on-demand, reducing network overhead and enabling reliable point-to-point communication.

**Key Characteristics**:
- **On-Demand**: Routes created only when needed
- **Distance Vector**: Uses hop count as routing metric
- **Reactive**: Discovers routes reactively, not proactively
- **Loop-Free**: Uses sequence numbers to prevent routing loops
- **Self-Healing**: Detects and repairs broken routes automatically

---

## AODV Route Table

### Structure

The route table is an **in-memory data structure** (not persistent) that stores active routes:

```cpp
class AODVRouteTable {
private:
    std::map<uint32_t, AODVRouteEntry> routes;  // destination -> route entry
    std::map<uint32_t, PendingRREQ> pendingRREQs;  // pending route requests
    std::map<uint32_t, std::vector<BufferedPacket>> packetBuffer;  // queued packets
    std::map<uint32_t, uint32_t> rreqRateLimit;  // rate limiting
    
    uint32_t mySeqNum;      // Local sequence number
    uint32_t nextRREQId;    // RREQ ID counter
};
```

### Route Entry Structure

Each route contains:

```cpp
struct AODVRouteEntry {
    uint32_t destination;        // Destination node (32-bit full node number)
    uint8_t nextHop;            // Next hop node (8-bit last byte)
    uint8_t hopCount;           // Distance to destination
    uint32_t destSeqNum;        // Destination sequence number
    uint32_t expiryTime;        // Route expiration timestamp (millis())
    bool isValid;               // Route validity flag
    std::set<uint8_t> precursors; // Upstream nodes using this route
};
```

**Field Details**:
- **destination**: Full 32-bit node number (e.g., `0x12345678`)
- **nextHop**: Last byte of next hop node (e.g., `0x78`)
- **hopCount**: Number of hops to reach destination (1 = direct neighbor)
- **destSeqNum**: Sequence number from destination (ensures freshness)
- **expiryTime**: `millis()` timestamp when route becomes invalid
- **isValid**: Flag indicating if route is currently usable
- **precursors**: Set of nodes that use this route (for RERR propagation)

### Persistence: NOT PERSISTENT

**Important**: The AODV route table is **NOT persistent**:
- ✅ **Stored in RAM** (std::map in memory)
- ❌ **Not saved to flash/disk**
- ❌ **Lost on reboot/power cycle**
- ❌ **Not shared between nodes**

**Why Not Persistent?**
1. **Dynamic Network**: Routes change frequently as nodes move
2. **Stale Data Risk**: Saved routes would be outdated after reboot
3. **Memory Efficiency**: Flash writes are expensive on embedded devices
4. **Fresh Discovery**: On-demand discovery ensures current topology

**After Reboot**: 
- Route table starts empty
- Routes rebuilt as needed through RREQ/RREP exchange
- Typically takes 1-2 seconds per destination on first use

---

## AODV Protocol Messages

### 1. RREQ (Route Request)

**Purpose**: Broadcast to discover route to destination

**When Sent**:
- Application wants to send unicast packet
- No valid route exists in table
- Rate limiting allows (min 1s between RREQs)

**Protobuf Structure**:
```protobuf
message RouteRequest {
    uint32 rreq_id = 1;              // Unique RREQ identifier
    uint32 originator = 2;           // Node initiating discovery
    uint32 originator_seq_num = 3;   // Originator's sequence number
    uint32 destination = 4;          // Target destination node
    uint32 dest_seq_num = 5;         // Last known dest seq num (0 if unknown)
}
```

**Hop Count**: Tracked in `MeshPacket.hop_start` and `MeshPacket.hop_limit`
- `hop_start`: Initial hop limit when packet sent
- `hop_limit`: Remaining hops (decremented at each hop)
- `hop_count = hop_start - hop_limit`

**RREQ Processing**:
1. **Originator**: Creates RREQ, broadcasts to mesh
2. **Intermediate Node**: 
   - Records reverse route to originator
   - Checks if it has route to destination
   - If YES: Sends RREP back to originator
   - If NO: Rebroadcasts RREQ
3. **Destination**: Sends RREP back to originator

### 2. RREP (Route Reply)

**Purpose**: Unicast response containing route information

**When Sent**:
- Destination receives RREQ
- Intermediate node has valid route to destination

**Protobuf Structure**:
```protobuf
message RouteReply {
    uint32 originator = 1;       // Original RREQ sender
    uint32 destination = 2;      // Route destination
    uint32 dest_seq_num = 3;     // Destination sequence number
    uint32 lifetime = 4;         // Route lifetime in seconds
}
```

**RREP Processing**:
1. **Destination**: Creates RREP with fresh sequence number
2. **Intermediate Node**:
   - Records forward route to destination
   - Forwards RREP toward originator using reverse route
3. **Originator**: Installs route, sends buffered packets

### 3. RERR (Route Error)

**Purpose**: Notify upstream nodes of broken link

**When Sent**:
- Link failure detected (ACK timeout, retransmission failure)
- Packet forwarding fails

**Protobuf Structure**:
```protobuf
message RouteError {
    repeated UnreachableNode unreachable_destinations = 1;
}

message UnreachableNode {
    uint32 node_num = 1;
    uint32 seq_num = 2;
}
```

**RERR Processing**:
1. **Detecting Node**: Identifies broken link
2. **Creates RERR**: Lists unreachable destinations
3. **Sends to Precursors**: Notifies upstream nodes
4. **Recipients**: Invalidate affected routes

---

## Route Discovery Process

### Example: Node A wants to send to Node C

```
Topology: A --- B --- C

Step 1: A initiates route discovery
------------------------
A: No route to C in table
A: Create RREQ (orig=A, dest=C, rreq_id=1, seq=5)
A: Broadcast RREQ
A: Buffer data packet for C

Step 2: B receives and forwards RREQ
------------------------
B: Receive RREQ from A
B: Not seen RREQ(A,1) before → mark as seen
B: Record reverse route: dest=A, next_hop=A, hops=1
B: Not destination (C)
B: No route to C
B: Rebroadcast RREQ (hop_limit decremented)

Step 3: C receives RREQ
------------------------
C: Receive RREQ from B
C: Not seen RREQ(A,1) before → mark as seen
C: Record reverse route: dest=A, next_hop=B, hops=2
C: I am destination!
C: Create RREP (orig=A, dest=C, seq=10)
C: Send RREP unicast to A using reverse route

Step 4: B forwards RREP
------------------------
B: Receive RREP from C
B: Record forward route: dest=C, next_hop=C, hops=1
B: Forward RREP to A using reverse route (next_hop=A)

Step 5: A receives RREP
------------------------
A: Receive RREP from B
A: Record route: dest=C, next_hop=B, hops=2
A: Remove pending RREQ(C)
A: Deliver buffered packets to C
A: Route established!

Step 6: Data transmission
------------------------
A: Send data to C
A: next_hop=B (from route table)
A: Packet forwarded via B to C
```

### Sequence Diagram

```
A (orig)          B (relay)         C (dest)
   |                |                |
   |---RREQ bcst--->|                |
   |                |---RREQ bcst--->|
   |                |                |
   |                |<---RREP--------|
   |<---RREP--------|                |
   |                |                |
   |===DATA========>|                |
   |                |===DATA========>|
```

---

## Route Table Operations

### 1. Route Lookup

```cpp
AODVRouteEntry* findRoute(uint32_t destination) {
    auto it = routes.find(destination);
    if (it != routes.end()) {
        if (route.isValid && !route.isExpired()) {
            return &route;  // Valid route found
        } else {
            route.isValid = false;  // Mark expired route invalid
        }
    }
    return nullptr;  // No valid route
}
```

### 2. Route Addition

```cpp
void addRoute(uint32_t destination, uint8_t nextHop, 
              uint8_t hopCount, uint32_t destSeqNum) {
    uint32_t expiry = millis() + AODV_ACTIVE_ROUTE_TIMEOUT;  // 5 minutes
    routes[destination] = AODVRouteEntry(destination, nextHop, 
                                         hopCount, destSeqNum, expiry);
}
```

### 3. Route Update

```cpp
void updateRoute(uint32_t destination, uint8_t nextHop, 
                 uint8_t hopCount, uint32_t destSeqNum) {
    auto &route = routes[destination];
    
    // Only update if:
    // - Higher sequence number (fresher)
    // - Same seq but lower hop count (shorter path)
    if (destSeqNum > route.destSeqNum || 
        (destSeqNum == route.destSeqNum && hopCount < route.hopCount)) {
        route.nextHop = nextHop;
        route.hopCount = hopCount;
        route.destSeqNum = destSeqNum;
        route.refreshExpiry();  // Reset expiry timer
        route.isValid = true;
    }
}
```

### 4. Route Invalidation

```cpp
void invalidateRoute(uint32_t destination) {
    auto it = routes.find(destination);
    if (it != routes.end()) {
        it->second.isValid = false;
        it->second.expiryTime = millis();  // Expire immediately
    }
}
```

### 5. Route Cleanup

```cpp
void removeExpiredRoutes() {
    uint32_t now = millis();
    for (auto it = routes.begin(); it != routes.end();) {
        if (it->second.isExpired()) {
            it = routes.erase(it);  // Remove expired route
        } else {
            ++it;
        }
    }
}
```

**Cleanup Interval**: Every 60 seconds (AODV_ROUTE_CLEANUP_INTERVAL)

---

## Packet Buffering

### Why Buffer?

When an application wants to send a packet but no route exists:
1. **Initiate route discovery** (send RREQ)
2. **Buffer the packet** (don't drop it)
3. **Wait for RREP** (route reply)
4. **Deliver buffered packets** (once route established)

### Buffer Structure

```cpp
struct BufferedPacket {
    meshtastic_MeshPacket *packet;  // Pointer to packet
    uint32_t timestamp;             // When buffered
    
    bool isExpired() const {
        return millis() - timestamp > AODV_NET_TRAVERSAL_TIME * 2;  // 20s
    }
};

std::map<uint32_t, std::vector<BufferedPacket>> packetBuffer;
// destination -> list of buffered packets
```

### Buffer Limits

- **Per Destination**: Max 5 packets (`AODV_MAX_PENDING_PACKETS_PER_DEST`)
- **Timeout**: 20 seconds (2 × `AODV_NET_TRAVERSAL_TIME`)
- **Overflow**: Oldest packet dropped if buffer full

### Buffer Operations

```cpp
// Add packet to buffer
void bufferPacket(uint32_t destination, meshtastic_MeshPacket *packet) {
    auto &buffer = packetBuffer[destination];
    
    if (buffer.size() >= AODV_MAX_PENDING_PACKETS_PER_DEST) {
        // Drop oldest packet
        packetPool.release(buffer.front().packet);
        buffer.erase(buffer.begin());
    }
    
    buffer.push_back(BufferedPacket(packet));
}

// Deliver all buffered packets
void deliverBufferedPackets(uint32_t destination) {
    auto packets = getBufferedPackets(destination);
    clearBufferedPacketsWithoutFreeing(destination);  // Transfer ownership
    
    for (auto packet : packets) {
        router->send(packet);  // Router takes ownership
    }
}
```

---

## Sequence Numbers

### Purpose

Sequence numbers prevent:
- **Routing loops** (packets circulating forever)
- **Stale routes** (using outdated path information)

### Types

1. **Destination Sequence Number** (`destSeqNum`)
   - Maintained by each node for itself
   - Incremented when sending RREP
   - Ensures route freshness

2. **Originator Sequence Number** (`originator_seq_num`)
   - Included in RREQ
   - Helps intermediate nodes maintain fresh reverse routes

### Rules

**Route Selection**:
```cpp
// Prefer route with:
// 1. Higher sequence number (fresher)
// 2. If same seq, lower hop count (shorter)

if (new_seq > old_seq) {
    update_route();
} else if (new_seq == old_seq && new_hops < old_hops) {
    update_route();
} else {
    ignore();  // Older or longer route
}
```

**Sequence Number Increment**:
- On boot: Start at 1
- Before sending RREP: Increment by 1
- After route timeout: No change (preserve)

---

## Rate Limiting and Loop Prevention

### 1. RREQ Rate Limiting

Prevents RREQ flooding for same destination:

```cpp
bool canSendRREQ(uint32_t destination) {
    auto it = rreqRateLimit.find(destination);
    if (it != rreqRateLimit.end()) {
        uint32_t elapsed = millis() - it->second;
        return elapsed >= AODV_RREQ_RATE_LIMIT;  // Min 1 second
    }
    return true;  // First RREQ allowed
}
```

### 2. RREQ Duplicate Detection

Each node tracks seen RREQs to prevent rebroadcast loops:

```cpp
std::map<std::pair<uint32_t, uint32_t>, uint32_t> seenRREQs;
// (originator, rreq_id) -> timestamp

bool hasSeenRREQ(uint32_t originator, uint32_t rreqId) {
    auto key = std::make_pair(originator, rreqId);
    return seenRREQs.find(key) != seenRREQs.end();
}
```

**Cleanup**: Entries older than 20 seconds removed periodically

### 3. Next-Hop Enforcement

Only designated next hop forwards unicast AODV packets:

```cpp
// In NextHopRouter::perhapsRebroadcast()
if (p->next_hop != NO_NEXT_HOP_PREFERENCE) {
    if (p->next_hop != myLastByte()) {
        return false;  // Not for me, drop
    }
    // Forward packet
}
```

---

## Timeouts and Constants

### Configuration Constants

```cpp
#define AODV_ACTIVE_ROUTE_TIMEOUT 300000      // 5 minutes
#define AODV_RREQ_RETRIES 3                  // Max retransmissions
#define AODV_RREQ_RATE_LIMIT 1000            // 1 second between RREQs
#define AODV_NET_TRAVERSAL_TIME 10000         // 10 seconds
#define AODV_MAX_PENDING_PACKETS_PER_DEST 5  // Buffer size
#define AODV_ROUTE_CLEANUP_INTERVAL 60000     // 60 seconds
```

### Timeout Behavior

| Event | Timeout | Action |
|-------|---------|--------|
| Route unused | 5 minutes | Mark invalid, remove on cleanup |
| RREQ pending | 10 seconds | Retry or fail |
| Buffered packet | 20 seconds | Drop if no route |
| RREQ seen | 20 seconds | Forget (allow future RREQs) |

---

## Integration with Routing Stack

### Router Hierarchy

```
Router (base class)
  ├─ NextHopRouter (AODV unicast)
  │    └─ Uses AODVModule for route discovery
  └─ FloodingRouter (broadcast only)
```

### Packet Send Flow with AODV

```cpp
// Application sends packet
router->send(packet);

// NextHopRouter::send()
if (!isBroadcast(packet->to)) {
    // Unicast - check AODV route table
    packet->next_hop = aodvModule->getRouteTable()->getNextHop(packet->to);
    
    if (packet->next_hop == NO_NEXT_HOP_PREFERENCE) {
        // No route - initiate discovery
        aodvModule->initiateRouteDiscovery(packet->to, packet);
        return OK;  // Packet buffered, will send after RREP
    }
}

// Route exists - send with next_hop set
Router::send(packet);  // Continues to transmission
```

### Packet Receive Flow with AODV

```cpp
// Packet received from radio
Router::handleReceived(packet);

// Decrypt packet
perhapsDecode(packet);

// Call modules
MeshModule::callModules(packet);
  └─ AODVModule::handleReceivedProtobuf(packet, aodv_msg);
      ├─ If RREQ: handleRouteRequest()
      ├─ If RREP: handleRouteReply()
      └─ If RERR: handleRouteError()

// NextHopRouter::perhapsRebroadcast()
if (packet->next_hop == myLastByte()) {
    // Recompute next_hop for next leg
    packet->next_hop = aodvModule->getRouteTable()->getNextHop(packet->to);
    NextHopRouter::send(packet);  // Forward
}
```

---

## Link Failure Detection

### When Detected

1. **Retransmission Failure**: Max retries exceeded
2. **ACK Timeout**: No acknowledgment from next hop
3. **Manual Trigger**: Application reports unreachable node

### Link Failure Handling

```cpp
void AODVModule::handleLinkFailure(uint32_t destination) {
    // 1. Invalidate route
    routeTable.invalidateRoute(destination);
    
    // 2. Get precursors (upstream nodes using this route)
    AODVRouteEntry *route = routeTable.findRoute(destination);
    
    // 3. Create RERR
    meshtastic_RouteError rerr;
    rerr.unreachable_destinations[0].node_num = destination;
    rerr.unreachable_destinations[0].seq_num = route->destSeqNum;
    
    // 4. Broadcast RERR to precursors
    sendRERR(rerr);
}
```

### Precursor Lists

Tracks which nodes depend on each route:

```cpp
std::set<uint8_t> precursors;  // In AODVRouteEntry

// When node A forwards packet from B to C via route to D:
routeTable.findRoute(D)->precursors.insert(lastByteOf(B));
```

**Purpose**: Know who to notify when route breaks

---

## AODV vs Flooding Comparison

| Aspect | AODV (Unicast) | Flooding (Broadcast) |
|--------|----------------|---------------------|
| **Discovery** | On-demand route discovery | No discovery needed |
| **State** | Route table maintained | Stateless |
| **Overhead** | RREQ/RREP messages | Every broadcast flooded |
| **Efficiency** | O(hops) per packet | O(nodes) per packet |
| **Scalability** | Good (targeted) | Poor (network-wide) |
| **Use Case** | DMs, unicast data | Group messages, announcements |
| **Persistence** | Routes cached 5 min | No caching |
| **Hop Limit** | Based on route | Broadcast hop limit |

---

## Example Scenarios

### Scenario 1: Simple 3-Hop Route

```
Network: A --- B --- C --- D

A wants to send to D (never communicated before)

Timeline:
t=0s:   A broadcasts RREQ(dest=D)
t=0.5s: B receives, rebroadcasts RREQ
t=1.0s: C receives, rebroadcasts RREQ
t=1.5s: D receives RREQ, sends RREP
t=2.0s: C forwards RREP to B
t=2.5s: B forwards RREP to A
t=3.0s: A receives RREP, route installed
        A->B->C->D (3 hops)
t=3.1s: A sends buffered data packet
        Packet travels A->B->C->D using next_hop fields

Route in A's table:
  destination: 0x...D
  next_hop: 0xB (last byte)
  hop_count: 3
  expiry: t=3.0 + 5min
```

### Scenario 2: Intermediate Reply

```
Network: A --- B --- C --- D
         Route exists: B knows route to D (1 hop via C)

A wants to send to D

Timeline:
t=0s:   A broadcasts RREQ(dest=D)
t=0.5s: B receives RREQ
        B has route to D! (via C, 2 hops)
        B sends RREP(dest=D, hops=2) to A
t=1.0s: A receives RREP from B
        Route installed: A->B->D (reported as 3 hops)
        
Note: C and D never see the RREQ (B replied immediately)
```

### Scenario 3: Link Failure and Recovery

```
Network: A --- B --- C
         Active route: A->B->C

t=0s:   A sends data to C (using route via B)
t=5s:   B moves out of range
t=10s:  A tries to send packet
        Retransmissions to B fail
        handleLinkFailure(C) triggered
        
t=10.1s: A invalidates route to C
         A sends RERR(unreachable=C)
         
t=10.2s: A initiates new route discovery
         RREQ broadcast
         
t=12s:   Alternative route found: A->D->C
         RREP received, new route installed
         Buffered packets delivered via new path
```

---

## Performance Characteristics

### Memory Usage

**Per Route Entry**: ~64 bytes
```
destination:  4 bytes
nextHop:      1 byte
hopCount:     1 byte
destSeqNum:   4 bytes
expiryTime:   4 bytes
isValid:      1 byte
precursors:   ~40 bytes (std::set overhead + entries)
```

**Typical Table Size**:
- 10 routes: ~640 bytes
- 50 routes: ~3.2 KB
- 100 routes: ~6.4 KB

### Latency

**Route Discovery** (first packet):
- Best case: 2 × hop_count × airtime (~2-10 seconds)
- Worst case: RREQ_RETRIES × NET_TRAVERSAL_TIME (~30 seconds)

**Subsequent Packets** (route exists):
- Latency: hop_count × airtime (no discovery overhead)

### Network Overhead

**Messages per Route Discovery**:
- RREQ broadcasts: O(nodes) in network
- RREP unicast: O(hops) on path
- Total: ~10-100 packets depending on density

**Maintenance**:
- RERR on link failure: O(precursors)
- Periodic cleanup: No network traffic

---

## Debugging and Monitoring

### Log Labels

```
AODV RREQ SEND:   Initiating route discovery
AODV RREQ RBCAST: Rebroadcasting RREQ
AODV RREQ TARGET: Received RREQ for us
AODV RREQ RECV:   Intermediate has route

AODV RREP SEND:   Sending route reply
AODV RREP FWD:    Forwarding RREP
AODV RREP RECV:   Route established

AODV ROUTE ADD:   New route installed
AODV ROUTE UPDATE: Existing route improved
AODV ROUTE REFRESH: Route lifetime extended

DATA SEND:        Sending via AODV route
DATA FWD:         Forwarding data packet
```

### Useful Metrics

```
Routes in table:  routeTable.getRouteCount()
Pending RREQs:    pendingRREQs.size()
Buffered packets: Sum of packetBuffer sizes
Average hops:     Mean of route.hopCount
```

---

## Key Takeaways

1. **Route table is NOT persistent** - rebuilt on demand after reboot
2. **On-demand discovery** - routes created when needed, not proactively
3. **5-minute lifetime** - routes expire after inactivity
4. **Sequence numbers** - prevent loops and ensure freshness
5. **Packet buffering** - queues data during route discovery
6. **Rate limiting** - prevents RREQ storms (min 1s between attempts)
7. **Link failure detection** - automatic route repair via RERR
8. **Next-hop routing** - only designated next hop forwards packets
9. **Integration with NextHopRouter** - seamless unicast routing
10. **Complements flooding** - AODV for unicast, flooding for broadcast
