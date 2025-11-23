# Quick Start: AODV Routing on LilyGO T-Beam

## Prerequisites

- LilyGO T-Beam v1.0 or later
- USB cable for programming
- PlatformIO installed
- Git (to clone repository)

## Step-by-Step Setup

### 1. Build the Firmware

```powershell
# Navigate to firmware directory
cd d:\Work\FYP\firmware_meshtastic_new

# Build for T-Beam with AODV
pio run -e tbeam
```

### 2. Flash to T-Beam

```powershell
# Connect T-Beam via USB
# Flash the firmware
pio run -e tbeam -t upload
```

### 3. Monitor Operation

```powershell
# Open serial monitor to see AODV logs
pio device monitor -e tbeam
```

## What to Expect

After flashing, you should see in serial monitor:

```
...
[INFO] AODV Router initialized
[INFO] Using AODV routing protocol
...
```

## Testing AODV

### Minimum Setup: 3 Nodes

```
Node A <---> Node B <---> Node C
```

### Test Scenario 1: Direct Communication

1. Send message from Node A to Node B
2. **Expected**: Message delivered immediately (direct link)
3. **Logs on A**:
   ```
   AODV: Valid route found to 0xXXXXXXXX via 0xXXXXXXXX (hops: 1)
   ```

### Test Scenario 2: Multi-hop Communication

1. Place Node C out of range of Node A (but in range of B)
2. Send message from Node A to Node C
3. **Expected**: 
   - RREQ broadcast from A
   - RREP from C via B
   - Message delivered through B

4. **Logs on A**:
   ```
   AODV: No route to 0xXXXXXXXX, initiating route discovery
   AODV: Sending RREQ for dest 0xXXXXXXXX with TTL 3
   AODV: Route discovery complete for 0xXXXXXXXX
   AODV: Valid route found to 0xXXXXXXXX via 0xYYYYYYYY (hops: 2)
   ```

5. **Logs on B** (intermediate node):
   ```
   AODV: Received RREQ id=1 from 0xXXXXXXXX to 0xZZZZZZZZ (hops=1)
   AODV: Forwarding RREQ
   AODV: Received RREP from 0xZZZZZZZZ (hops=0, seq=2)
   AODV: Forwarding RREP to 0xXXXXXXXX via 0xXXXXXXXX
   ```

6. **Logs on C** (destination):
   ```
   AODV: Received RREQ id=1 from 0xXXXXXXXX to 0xZZZZZZZZ (hops=2)
   AODV: We are the destination, sending RREP
   ```

## Understanding the Logs

### RREQ (Route Request)
```
AODV: Sending RREQ for dest 0x12345678 with TTL 3
```
- Node is looking for a route to destination `0x12345678`
- Starting with hop limit (TTL) of 3

### RREP (Route Reply)
```
AODV: Received RREP from 0x87654321 (hops=2, seq=5)
```
- Found route to `0x87654321`
- Route is 2 hops away
- Route sequence number is 5 (for freshness)

### Route Found
```
AODV: Valid route found to 0xABCDEF via 0x123456 (hops: 3)
```
- Route to destination `0xABCDEF` is ready
- Next hop is `0x123456`
- Total path is 3 hops

## Comparing with Default Routing

To test the difference, build without AODV:

```powershell
# Edit variants/esp32/tbeam/platformio.ini
# Remove or comment out: -D USE_AODV_ROUTING

# Build default firmware
pio run -e tbeam -t upload
```

### Key Differences You'll Notice

| Behavior | Default Router | AODV Router |
|----------|---------------|-------------|
| First message to new node | May fail or be slow | Fast after route discovery |
| Route discovery | Implicit (learns from traffic) | Explicit (RREQ/RREP messages) |
| Serial output | Less routing info | Detailed routing logs |
| Topology changes | Gradual adaptation | Quick re-routing |

## Troubleshooting

### No AODV logs appear

**Check**: Is AODV enabled?
```powershell
# Verify build flags
pio run -e tbeam -v
# Should see: -D USE_AODV_ROUTING
```

### Messages not delivered

1. **Check radio settings**: All nodes must use same frequency/channel
2. **Check encryption**: All nodes need same encryption key
3. **Check range**: Nodes must be within radio range
4. **Monitor logs**: Look for RREQ/RREP exchanges

### Route discovery fails

**Symptoms**: See RREQ but no RREP
1. Increase RREQ retries in `AODVRouter.h`:
   ```cpp
   #define AODV_RREQ_RETRIES 3  // Was 2
   ```
2. Increase network traversal time:
   ```cpp
   #define AODV_NET_TRAVERSAL_TIME 3000  // Was 2000
   ```
3. Rebuild and reflash

### Too many RREQ broadcasts

**Symptoms**: Lots of route discoveries
1. Increase route timeout in `AODVRouter.h`:
   ```cpp
   #define AODV_ACTIVE_ROUTE_TIMEOUT 5000  // Was 3000
   ```
2. This keeps routes valid longer

## Performance Tips

### For Static Networks (nodes don't move)
```cpp
// In AODVRouter.h
#define AODV_ACTIVE_ROUTE_TIMEOUT 10000  // 10 seconds
#define AODV_RREQ_RATELIMIT 20000        // 20 seconds
```

### For Mobile Networks (nodes move frequently)
```cpp
// In AODVRouter.h
#define AODV_ACTIVE_ROUTE_TIMEOUT 2000   // 2 seconds
#define AODV_RREQ_RETRIES 3              // 3 retries
```

## Advanced: Viewing Routing Table

To debug routing issues, you can add code to dump the routing table.

Add to `AODVRouter.cpp` in `runOnce()`:

```cpp
// Print routing table every 30 seconds
static uint32_t lastPrint = 0;
if (millis() - lastPrint > 30000) {
    LOG_INFO("=== AODV Routing Table ===");
    for (auto &entry : routingTable) {
        if (entry.second.routeValid) {
            LOG_INFO("Dest: 0x%08x -> Next: 0x%08x (hops: %d, seq: %u)",
                     entry.first, entry.second.nextHop, 
                     entry.second.hopCount, entry.second.destSeqNum);
        }
    }
    lastPrint = millis();
}
```

## Next Steps

1. **Test basic routing**: 2-3 nodes, direct and multi-hop
2. **Test route changes**: Move nodes, break links
3. **Monitor overhead**: Observe RREQ frequency
4. **Tune parameters**: Adjust timeouts for your use case
5. **Compare performance**: Test vs default routing

## Getting Help

- Check logs carefully - AODV provides detailed debug output
- Serial monitor is your friend
- Start simple (2 nodes) and add complexity
- Document your node positions and test results

## Success Criteria

✅ AODV router initializes on boot
✅ Can send messages between adjacent nodes
✅ Can send messages through intermediate nodes
✅ RREQ/RREP exchange visible in logs
✅ Routes stored and reused (no RREQ for second message)
✅ Routes expire and rediscover after timeout

## Example Test Session

```
# Terminal 1 - Node A
AODV Router initialized
Using AODV routing protocol
AODV: No route to 0x87654321, initiating route discovery
AODV: Sending RREQ for dest 0x87654321 with TTL 3
AODV: Route discovery complete for 0x87654321
AODV: Valid route found to 0x87654321 via 0xABCDEF (hops: 2)

# Terminal 2 - Node B (intermediate)
AODV Router initialized
Using AODV routing protocol
AODV: Received RREQ id=1 from 0x12345678 to 0x87654321 (hops=1)
AODV: Forwarding RREQ
AODV: Received RREP from 0x87654321 (hops=1, seq=3)
AODV: Forwarding RREP to 0x12345678 via 0x12345678

# Terminal 3 - Node C (destination)
AODV Router initialized
Using AODV routing protocol
AODV: Received RREQ id=1 from 0x12345678 to 0x87654321 (hops=2)
AODV: We are the destination, sending RREP
```

This confirms AODV is working correctly!
