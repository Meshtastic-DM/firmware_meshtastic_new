# AODV Routing Implementation for Meshtastic

## Overview

This implementation adds **AODV (Ad-hoc On-Demand Distance Vector)** routing protocol to the Meshtastic firmware. AODV is a reactive routing protocol that discovers routes only when needed, making it efficient for mesh networks.

## Architecture

### Class Hierarchy
```
Router (base)
  └── FloodingRouter
       └── AODVRouter
```

### Key Components

1. **AODVRouter.h/cpp** - Main AODV routing implementation
2. **Route Table** - Stores discovered routes with sequence numbers
3. **RREQ Cache** - Prevents duplicate RREQ processing
4. **Pending RREQ Queue** - Manages route discovery attempts

## AODV Protocol Features

### Route Discovery (RREQ/RREP)
- **Route Request (RREQ)**: Broadcast when no route exists
- **Route Reply (RREP)**: Unicast response with route information
- **Sequence Numbers**: Ensure loop-free and fresh routes
- **Expanding Ring Search**: Starts with small TTL, expands on retry

### Route Maintenance
- **Route Timeouts**: Routes expire after 3 seconds of inactivity
- **Route Error (RERR)**: Notifies upstream nodes of broken links
- **Local Repair**: Attempts to fix broken routes locally

### Key Parameters
```cpp
AODV_ACTIVE_ROUTE_TIMEOUT = 3000ms    // Route lifetime
AODV_NET_TRAVERSAL_TIME = 2000ms      // Max network traversal time
AODV_RREQ_RETRIES = 2                 // Number of RREQ attempts
AODV_RREQ_RATELIMIT = 10000ms         // Min time between RREQs
```

## Data Structures

### Route Entry
```cpp
struct AODVRouteEntry {
    NodeNum destination;
    uint32_t destSeqNum;        // Destination sequence number
    bool validDestSeqNum;
    uint8_t hopCount;
    NodeNum nextHop;
    uint32_t lifetime;
    bool routeValid;
}
```

### RREQ Message Format
Uses `RouteDiscovery` protobuf with custom encoding:
- `route[0]` = RREQ ID
- `route[1]` = Originator Sequence Number
- `route[2]` = Destination Sequence Number
- `route[3]` = Hop Count
- `route[4]` = Flags

### RREP Message Format
- `route[0]` = RREQ ID (for matching)
- `route[1]` = Destination Sequence Number
- `route[2]` = Hop Count

## Building for LilyGO T-Beam

### Configuration

The AODV routing is enabled for T-Beam devices via the `USE_AODV_ROUTING` build flag.

**File**: `variants/esp32/tbeam/platformio.ini`
```ini
build_flags = 
  ${esp32_base.build_flags}
  -D TBEAM_V10
  -D USE_AODV_ROUTING  # Enable AODV routing
  -I variants/esp32/tbeam
```

### Build Commands

```bash
# Build for T-Beam with AODV
pio run -e tbeam

# Upload to device
pio run -e tbeam -t upload

# Monitor serial output
pio device monitor -e tbeam
```

### PowerShell (Windows)
```powershell
# Build for T-Beam
pio run -e tbeam

# Upload
pio run -e tbeam -t upload

# Monitor
pio device monitor -e tbeam
```

## Usage

### Automatic Operation

AODV operates automatically once enabled:

1. **Sending a packet**: If no route exists, AODV initiates route discovery
2. **Receiving RREQ**: Processes and forwards/replies to route requests
3. **Receiving RREP**: Updates routing table and sends buffered packets
4. **Route Expiry**: Automatically removes stale routes

### Monitoring

Enable debug logging to see AODV operation:

```cpp
// In serial monitor, you'll see:
AODV: No route to 0x12345678, initiating route discovery
AODV: Sending RREQ for dest 0x12345678 with TTL 3
AODV: Received RREQ id=1 from 0x87654321 to 0x12345678 (hops=2)
AODV: Route discovery complete for 0x12345678
AODV: Valid route found to 0x12345678 via 0xABCDEF (hops: 3)
```

## Protocol Flow

### Route Discovery Example

```
Node A wants to send to Node D:

1. A broadcasts RREQ(dest=D, seq=1, hops=0)
2. B receives RREQ, creates reverse route to A
3. B forwards RREQ(dest=D, seq=1, hops=1)
4. C receives RREQ, creates reverse route to A via B
5. C forwards RREQ(dest=D, seq=1, hops=2)
6. D receives RREQ, sends RREP(seq=5, hops=0) to A
7. C receives RREP, creates route to D, forwards RREP(hops=1)
8. B receives RREP, creates route to D, forwards RREP(hops=2)
9. A receives RREP, creates route to D, sends buffered packet
```

### Sequence Numbers

- Each node maintains its own sequence number
- Incremented before each RREQ origination
- Used to determine route freshness
- Prevents routing loops

## Advantages of AODV

1. **On-Demand**: Only discovers routes when needed
2. **Loop-Free**: Sequence numbers prevent loops
3. **Scalable**: Low overhead for static networks
4. **Adaptive**: Responds to topology changes
5. **Bandwidth Efficient**: No periodic updates

## Differences from Default Meshtastic Routing

| Feature | Default (ReliableRouter) | AODV Router |
|---------|-------------------------|-------------|
| Route Discovery | Passive (learns from traffic) | Active (RREQ/RREP) |
| Routing Table | Next-hop only | Full route table with sequence numbers |
| Loop Prevention | Hop limit | Sequence numbers |
| Route Maintenance | Implicit | Explicit (RERR) |
| Overhead | Lower | Higher (control messages) |
| Convergence | Slower | Faster |

## Testing

### Basic Functionality Test

1. **Setup**: Flash 3+ T-Beam devices with AODV firmware
2. **Test 1**: Send message from A to B (direct link)
   - Should see immediate delivery
3. **Test 2**: Send message from A to C (via B)
   - Should see RREQ/RREP exchange
   - Message delivered after route discovery
4. **Test 3**: Move nodes to test route changes
   - Should see new RREQs and route updates

### Debug Commands

```cpp
// Add to your test code:
LOG_DEBUG("Route count: %d", routingTable.size());
LOG_DEBUG("Pending RREQ count: %d", pendingRREQs.size());
```

## Troubleshooting

### Issue: Route Discovery Fails

**Symptoms**: Packets not delivered, no RREP received

**Solutions**:
1. Check hop limit is sufficient
2. Verify nodes are in radio range
3. Check for channel/encryption mismatch
4. Monitor for RREQ/RREP in serial output

### Issue: High Overhead

**Symptoms**: Excessive RREQ broadcasts

**Solutions**:
1. Increase `AODV_RREQ_RATELIMIT`
2. Increase `AODV_ACTIVE_ROUTE_TIMEOUT`
3. Check for unstable links causing frequent route breaks

### Issue: Routes Expire Too Quickly

**Symptoms**: Frequent re-discovery for same destination

**Solutions**:
1. Increase `AODV_ACTIVE_ROUTE_TIMEOUT`
2. Verify routes are being refreshed on use
3. Check network stability

## Future Enhancements

1. **Precursor Lists**: Track nodes using a route for efficient RERR
2. **Route Repair**: Local repair of broken routes
3. **HELLO Messages**: Maintain neighbor awareness
4. **Blacklist**: Track problematic nodes
5. **Metrics**: Add link quality metrics to route selection
6. **Energy Awareness**: Consider battery levels in routing

## Performance Tuning

### For Dense Networks
```cpp
AODV_ACTIVE_ROUTE_TIMEOUT = 5000ms  // Longer route lifetime
AODV_RREQ_RATELIMIT = 15000ms       // Less frequent RREQs
```

### For Sparse Networks
```cpp
AODV_RREQ_RETRIES = 3               // More retry attempts
AODV_NET_TRAVERSAL_TIME = 3000ms    // Longer wait for RREP
```

### For Mobile Networks
```cpp
AODV_ACTIVE_ROUTE_TIMEOUT = 2000ms  // Shorter timeout
AODV_MAX_REPAIR_TTL = 5             // Larger repair scope
```

## References

- RFC 3561: Ad hoc On-Demand Distance Vector (AODV) Routing
- Meshtastic Protocol Documentation
- Original implementation: `src/mesh/AODVRouter.{h,cpp}`

## License

This implementation follows the Meshtastic project license (GPL-3.0).
