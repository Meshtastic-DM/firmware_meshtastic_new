# AODV Routing Implementation Summary

## Implementation Complete ✅

AODV (Ad-hoc On-Demand Distance Vector) routing has been successfully implemented in the Meshtastic firmware for LilyGO T-Beam devices.

## Files Created/Modified

### New Files
1. **src/mesh/AODVRouter.h** - AODV router class definition
2. **src/mesh/AODVRouter.cpp** - AODV routing protocol implementation
3. **AODV_IMPLEMENTATION.md** - Detailed technical documentation
4. **QUICK_START_AODV.md** - Quick start guide for users

### Modified Files
1. **src/main.cpp** - Added AODVRouter initialization
2. **variants/esp32/tbeam/platformio.ini** - Added USE_AODV_ROUTING flag

## Architecture Overview

```
┌─────────────────────────────────────────┐
│           Application Layer             │
│         (User Messages/Data)            │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│          AODVRouter Class               │
│  ┌────────────────────────────────┐    │
│  │  Route Discovery (RREQ/RREP)   │    │
│  │  - Send RREQ when no route     │    │
│  │  - Process RREQ, create route  │    │
│  │  - Send RREP back to source    │    │
│  └────────────────────────────────┘    │
│  ┌────────────────────────────────┐    │
│  │  Routing Table Management      │    │
│  │  - Store routes with seq nums  │    │
│  │  - Timeout expired routes      │    │
│  │  - Select best route           │    │
│  └────────────────────────────────┘    │
│  ┌────────────────────────────────┐    │
│  │  Route Maintenance             │    │
│  │  - RERR on link breaks         │    │
│  │  - RREQ cache for duplicates   │    │
│  │  - Retry logic with backoff    │    │
│  └────────────────────────────────┘    │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│       FloodingRouter (parent)           │
│     (Handles broadcast, filtering)      │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│            Radio Layer                  │
│         (LoRa transmission)             │
└─────────────────────────────────────────┘
```

## Key Features Implemented

### ✅ Route Discovery
- **RREQ (Route Request)**: Broadcast to find routes
- **RREP (Route Reply)**: Unicast response with route info
- **Expanding Ring Search**: Start with small TTL, expand on retry
- **Sequence Numbers**: Prevent loops and ensure freshness

### ✅ Routing Table
- Stores destination, next hop, hop count
- Sequence numbers for route validation
- Lifetime management (3-second default)
- Automatic expiration of stale routes

### ✅ Route Maintenance
- **RREQ Cache**: Prevents duplicate processing
- **Retry Mechanism**: Up to 2 retries with increasing TTL
- **Buffering**: Holds packets during route discovery
- **Clean-up**: Removes expired routes and cache entries

### ✅ Integration
- Seamlessly integrates with existing Meshtastic router architecture
- Uses existing protobuf messages (RouteDiscovery)
- Conditional compilation via USE_AODV_ROUTING flag
- Backward compatible (can disable AODV)

## Protocol Message Format

### RREQ Message
```
RouteDiscovery {
  route[0] = RREQ ID
  route[1] = Originator Sequence Number
  route[2] = Destination Sequence Number
  route[3] = Hop Count
  route[4] = Flags (has dest seq num)
}
```

### RREP Message
```
RouteDiscovery {
  route[0] = RREQ ID (for matching)
  route[1] = Destination Sequence Number
  route[2] = Hop Count from destination
}
```

## Configuration

### Build Flags (platformio.ini)
```ini
-D USE_AODV_ROUTING    # Enable AODV routing
```

### Tunable Parameters (AODVRouter.h)
```cpp
AODV_ACTIVE_ROUTE_TIMEOUT = 3000ms    // Route lifetime
AODV_NET_TRAVERSAL_TIME = 2000ms      // Max network delay
AODV_RREQ_RETRIES = 2                 // Retry attempts
AODV_RREQ_RATELIMIT = 10000ms         // Rate limiting
```

## Build & Flash

### For LilyGO T-Beam

```powershell
# Build firmware
pio run -e tbeam

# Flash to device
pio run -e tbeam -t upload

# Monitor serial output
pio device monitor -e tbeam
```

## Testing Checklist

- [ ] Firmware builds without errors
- [ ] AODV Router initializes on boot
- [ ] Direct communication (1 hop) works
- [ ] Multi-hop communication (2+ hops) works
- [ ] RREQ/RREP exchange visible in logs
- [ ] Routes are cached and reused
- [ ] Routes expire after timeout
- [ ] Route rediscovery on expiration
- [ ] Handles node mobility/topology changes

## Performance Characteristics

### Advantages
- ✅ Fast route discovery
- ✅ Loop-free routing (sequence numbers)
- ✅ Adapts to topology changes
- ✅ Low memory overhead per route
- ✅ Explicit route error handling

### Considerations
- ⚠️ RREQ floods can cause overhead in dense networks
- ⚠️ Route discovery latency for first packet
- ⚠️ Control message overhead (RREQ/RREP/RERR)
- ⚠️ Requires buffering during route discovery

## Comparison with Default Routing

| Metric | Default (ReliableRouter) | AODV Router |
|--------|-------------------------|-------------|
| Route Discovery | Passive (learns) | Active (RREQ/RREP) |
| First Packet Latency | Low | Higher (discovery) |
| Subsequent Packets | Medium | Low (cached route) |
| Topology Adaptation | Slow | Fast |
| Control Overhead | Low | Medium |
| Memory Usage | Low | Medium |
| Loop Prevention | Hop limit | Sequence numbers |
| Best For | Static, sparse | Dynamic, mobile |

## Future Enhancements

### Potential Improvements
1. **Precursor Lists**: Track upstream nodes for efficient RERR
2. **Local Repair**: Fix broken routes locally without full rediscovery
3. **HELLO Messages**: Proactive neighbor discovery
4. **Link Quality Metrics**: Consider SNR/RSSI in route selection
5. **Multipath**: Maintain alternate routes for resilience
6. **Energy Awareness**: Consider battery levels in routing decisions

### Advanced Features
- Route aggregation for reduced table size
- Multicast support (AODV-M)
- QoS-aware routing
- Security enhancements (encrypted RREQ/RREP)

## Documentation

- **AODV_IMPLEMENTATION.md** - Full technical documentation
- **QUICK_START_AODV.md** - User guide and testing procedures
- **Code comments** - Inline documentation in source files

## References

- **RFC 3561**: AODV Routing Protocol Specification
- **Meshtastic Docs**: https://meshtastic.org/docs/
- **Source Code**: `src/mesh/AODVRouter.{h,cpp}`

## License

This implementation follows the Meshtastic project license (GPL-3.0).

## Contributors

- AODV Implementation: Part of FYP project
- Based on: RFC 3561 and Meshtastic firmware architecture

---

**Status**: ✅ Implementation Complete and Ready for Testing

**Next Steps**: 
1. Build and flash firmware to T-Beam devices
2. Follow QUICK_START_AODV.md for testing
3. Monitor and tune parameters based on network behavior
4. Compare with default routing performance
