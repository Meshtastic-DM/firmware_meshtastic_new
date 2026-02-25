# SDN (Software Defined Networking) Implementation

## Overview

The SDN module provides centralized network management for Meshtastic mesh networks. It enables a designated controller node to collect routing information from the mesh and potentially influence routing decisions.

## Architecture

### Components

1. **SDN Controller**: A designated node that periodically broadcasts authenticated announcements
2. **SDN Clients**: Regular mesh nodes that:
   - Listen for and authenticate controller announcements
   - Report learned routes to the authenticated controller
   - Maintain bidirectional routes with the controller

### Protocol Buffer Definitions

The SDN protocol defines the following message types in `sdn.proto`:

#### SDNAnnouncement
Broadcast periodically by the SDN controller to establish its authority.

```protobuf
message SDNAnnouncement {
    bytes hmac_hash = 1;         // HMAC-SHA256 authentication tag (16 bytes)
    bytes public_key = 2;        // Curve25519 public key of controller (32 bytes)
    uint32 sequence_num = 3;     // Monotonically increasing sequence number
    fixed32 timestamp = 4;       // Unix timestamp when announcement was created
}
```

**Purpose:**
- Authenticate the controller to mesh nodes
- Provide anti-replay protection via sequence numbers
- Distribute controller's public key for future encryption
- Establish reverse routes to the controller

#### SDNRouteUpdate
Sent by mesh nodes to report newly learned AODV routes to the authenticated controller.

```protobuf
message SDNRouteUpdate {
    fixed32 destination = 1;     // Route destination node (32-bit full node number)
    uint32 next_hop = 2;         // Next hop node (8-bit last byte)
    uint32 hop_count = 3;        // Number of hops to destination
    uint32 dest_seq_num = 4;     // AODV destination sequence number
    fixed32 timestamp = 5;       // Unix timestamp when route was learned
                                 // Reporter node ID available in MeshPacket.from
}
```

**Purpose:**
- Provide controller with network topology visibility
- Enable centralized routing decisions (future feature)
- Support network analytics and troubleshooting

#### SDN
Top-level message wrapper that contains either announcement or route update.

```protobuf
message SDN {
    oneof payload_variant {
        SDNAnnouncement announcement = 1;    // Controller announcement message
        SDNRouteUpdate route_update = 2;     // Route update to controller
    }
}
```

## Security

### Authentication Mechanism

SDN announcements use **HMAC-SHA256** for authentication:

1. **HMAC Input**: `LE(controller_id || seq || timestamp || public_key)`
   - All multi-byte values encoded as little-endian
   - Total: 44 bytes (4 + 4 + 4 + 32)

2. **HMAC Key**: Shared secret configured on all nodes
   - Currently: `"meshtastic-sdn-secret"` (test mode)
   - Future: Configurable via module settings

3. **HMAC Output**: SHA256 → truncated to 16 bytes

### Anti-Replay Protection

- Each announcement includes a monotonically increasing `sequence_num`
- Nodes track `g_lastAcceptedControllerSeq` and reject seq ≤ last accepted
- Assumes single controller (multi-controller support requires per-controller tracking)

### Timestamp Validation

Announcements are rejected if:
- Timestamp is > 60 seconds in the future
- Timestamp is > 600 seconds in the past

This prevents replay attacks using old announcements captured from the network.

## Implementation Details

### SDN Controller Behavior

**Initialization:**
- Test mode: Node with ID `0x00000010` acts as controller
- Announcement interval: 60 seconds (test mode)

**Announcement Broadcasting:**
```cpp
void SDNModule::sendAnnouncement()
```

1. Retrieve public key from NodeDB or config
2. Increment sequence number (from AODV route table)
3. Generate current timestamp
4. Compute HMAC-SHA256 over message fields
5. Broadcast announcement with hop_limit=3

**Route Update Reception:**
```cpp
void SDNModule::handleSDNRouteUpdate(...)
```

- Currently logs received route updates
- Future: Store in topology database for centralized routing

### SDN Client Behavior

**Announcement Validation:**
```cpp
void SDNModule::handleSDNAnnouncement(...)
```

1. Verify public key size (must be 32 bytes)
2. Verify HMAC hash size (must be 16 bytes)
3. Compute expected HMAC using shared secret
4. Compare HMAC (constant-time via `memcmp`)
5. Check sequence number (anti-replay)
6. Validate timestamp window
7. Update `sdnAuthenticated` flag
8. Store controller node ID and public key
9. Install reverse route to controller via AODV

**Route Update Sending:**
```cpp
void SDNModule::sendRouteUpdate(...)
```

Called by AODV module when new routes are learned:

1. Check if controller is known and authenticated
2. Create SDNRouteUpdate message with route details
3. Send unicast to `sdnControllerNode` with ACK requested

### Integration with AODV

The SDN module is tightly integrated with AODV routing:

1. **Sequence Numbers**: SDN announcements use AODV sequence numbers for consistency
2. **Route Installation**: Validated announcements create AODV routes to controller
3. **Route Reporting**: AODV calls `sdnModule->sendRouteUpdate()` for new routes

## Configuration

### Current Test Mode Settings

```cpp
static constexpr uint32_t kTestControllerNode = 0x00000010;
static constexpr uint32_t kTestAnnouncementIntervalSec = 60;
static constexpr const char *kTestSecret = "meshtastic-sdn-secret";
```

### Future Configuration (TODO)

The SDN module needs to be added to module configuration protobufs:

```protobuf
message ModuleConfig {
  message SDNConfig {
    bool enabled = 1;
    bool is_controller = 2;
    uint32 announcement_interval_secs = 3;
    bytes hmac_secret = 4;  // Shared secret for authentication
    uint32 controller_node_id = 5;  // For clients: known controller
  }
  
  oneof payload_variant {
    // ... existing modules
    SDNConfig sdn = N;
  }
}
```

## Message Flow

### Controller Announcement Flow

```
SDN Controller (0x00000010)
    |
    | [Every 60s]
    |
    +-- Broadcast SDNAnnouncement (hop_limit=3)
        - hmac_hash: HMAC-SHA256 tag
        - public_key: Controller's Curve25519 key
        - sequence_num: Incrementing counter
        - timestamp: Current Unix time
    |
    v
Mesh Nodes (0xXXXXXXXX)
    |
    +-- Verify HMAC
    +-- Check sequence number (anti-replay)
    +-- Validate timestamp
    +-- Set sdnAuthenticated = true
    +-- Store controller ID and public key
    +-- Install reverse route to controller
```

### Route Update Flow

```
Mesh Node A (0xAAAAAAAA)
    |
    | [New AODV route learned]
    |
    +-- AODVModule detects new route to 0xDDDDDDDD
    |
    +-- Call sdnModule->sendRouteUpdate()
        |
        +-- Check: sdnAuthenticated?
        +-- Create SDNRouteUpdate
            - destination: 0xDDDDDDDD
            - next_hop: 0xNN
            - hop_count: H
            - dest_seq_num: S
            - timestamp: Current Unix time
            (reporter_node from MeshPacket.from)
        |
        +-- Send unicast to sdnControllerNode (with ACK)
    |
    v
SDN Controller (0x00000010)
    |
    +-- Receive route update
    +-- Log/store topology information
    +-- [Future] Make routing decisions
```

## Security Considerations

### Current Limitations

1. **Route Updates Not Authenticated**: Only announcements use HMAC
   - Risk: Nodes can send fake route updates to controller
   - Mitigation (TODO): Add HMAC or signature to route updates

2. **Shared Secret**: All nodes know the HMAC key
   - Risk: Compromised node can impersonate controller
   - Mitigation: Consider public-key signatures for announcements

3. **Single Controller**: Anti-replay assumes one controller
   - Risk: Cannot support multiple concurrent controllers
   - Mitigation (TODO): Per-controller sequence tracking

### Threat Model

**Protected Against:**
- ✅ Announcement replay attacks (sequence numbers)
- ✅ Announcement spoofing (HMAC authentication)
- ✅ Timestamp manipulation (window validation)
- ✅ Public key swapping (key bound into HMAC)

**Not Protected Against:**
- ❌ Route update spoofing (no authentication)
- ❌ Controller impersonation by compromised node with shared secret
- ❌ Denial of service (announcement flooding)

## Future Enhancements

### 1. Authenticated Route Updates
Add per-message authentication to route updates:
- Option A: HMAC using shared secret
- Option B: Digital signatures using controller's private key verification

### 2. Centralized Routing Decisions
Enable controller to:
- Compute optimal routes based on collected topology
- Send route installation commands to mesh nodes
- Override AODV decisions for policy-based routing

### 3. Multiple Controller Support
- Per-controller sequence tracking
- Controller election protocol
- Failover to backup controller

### 4. Performance Metrics
Nodes report to controller:
- Link quality metrics (SNR, RSSI)
- Packet delivery ratios
- Route stability information

### 5. Access Control
Controller can:
- Authorize/deauthorize nodes
- Enforce network segmentation
- Implement traffic policies

## Dependencies

- **AODVModule**: Route discovery and maintenance
- **NodeDB**: Node information storage and public key management
- **Router**: Packet forwarding
- **SHA256 Library**: HMAC computation
- **NanoPB**: Protocol buffer encoding/decoding
- **PKI**: Public key infrastructure (when not excluded)

## Module State

- **isSDNController**: Boolean flag indicating controller role
- **sdnControllerNode**: Node ID of authenticated controller (0 if none)
- **sdnAuthenticated**: True if valid announcement received
- **sdnPublicKey**: Stored controller public key (32 bytes)
- **hmacSecret**: Shared secret for HMAC (test mode: hardcoded)
- **lastAnnouncementTime**: Timestamp of last sent announcement
- **announcementInterval**: Seconds between announcements (60s test mode)

## Debugging

Enable SDN debug logging with:
```cpp
LOG_DEBUG("SDN: ...");  // Verbose debugging
LOG_INFO("SDN: ...");   // Normal operations
LOG_WARN("SDN: ...");   // Authentication failures, errors
```

Key log messages:
- `"SDN: Test mode controller enabled"` - Controller initialization
- `"SDN: HMAC verification failed"` - Invalid announcement received
- `"SDN: Route to controller installed"` - Reverse route established
- `"SDN: Sending route update"` - Reporting route to controller

## Protocol Buffer Files

Complete message definitions are shown in the [Protocol Buffer Definitions](#protocol-buffer-definitions) section above.

Source file: `protobufs/meshtastic/sdn.proto`

## References

- AODV Implementation: See `AODV_Implementation.md`
- PSK Encryption: See `PSK_Encryption_Flow.md`
- HMAC-SHA256: RFC 2104
- Protocol Buffers: https://protobuf.dev/
