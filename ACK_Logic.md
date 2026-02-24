# ACK (Acknowledgment) Logic

## Overview

The Meshtastic firmware implements a comprehensive acknowledgment system for reliable message delivery across the mesh network. The ACK logic spans multiple router layers (ReliableRouter, NextHopRouter, FloodingRouter) and coordinates with the RoutingModule to ensure packets reach their destination and senders receive confirmation.

## ACK Message Types

### ACK (Acknowledgment)
Positive confirmation that a packet was successfully received and processed.

```protobuf
message Routing {
    Error error_reason = 1;      // NONE for ACK
    // sent as a message with decoded.request_id set to the original packet ID
}
```

### NAK (Negative Acknowledgment)
Notification that a packet could not be delivered or processed.

```protobuf
message Routing {
    enum Error {
        NONE = 0;                      // Not an error (this is an ACK)
        NO_ROUTE = 1;                  // No route to destination
        GOT_NAK = 2;                   // Recipient sent a NAK
        TIMEOUT = 3;                   // Timeout waiting for ACK
        NO_INTERFACE = 4;              // No interface available
        MAX_RETRANSMIT = 5;            // Exceeded max retransmissions
        NO_CHANNEL = 6;                // Could not decrypt (no matching channel)
        TOO_LARGE = 7;                 // Packet too large
        NO_RESPONSE = 8;               // Request but no service responded
        DUTY_CYCLE_LIMIT = 9;          // Duty cycle limit reached
        BAD_REQUEST = 10;              // Malformed request
        NOT_AUTHORIZED = 11;           // Not authorized
        PKI_FAILED = 12;               // PKI encryption/decryption failed
        PKI_UNKNOWN_PUBKEY = 13;       // PKI public key not known
        ADMIN_BAD_SESSION_KEY = 32;    // Admin message session key mismatch
        ADMIN_PUBLIC_KEY_UNAUTHORIZED = 33;  // Admin sender not authorized
    }
    Error error_reason = 1;
}
```

## ACK Sending Logic (ReliableRouter)

### Standard ACK Conditions

ACKs are sent when a packet with `want_ack=true` is received and addressed to us:

```cpp
void ReliableRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    if (isToUs(p)) {
        if (p->want_ack) {
            if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
                // Determine appropriate hop limit for ACK
                uint8_t ackHopLimit = routingModule->getHopLimitForResponse(p->hop_start, p->hop_limit);
                sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, ackHopLimit);
            }
        }
    }
}
```

### ACK Sending Rules

#### 1. Reliable ACK (ACK-with-want-ack)
For critical packets (e.g., direct text messages), the ACK itself requests acknowledgment:

**Conditions:**
- Original packet has `want_ack=true`
- Packet is a direct unicast to us (`isToUs(p)`)
- Packet is a text message (TEXT_MESSAGE_APP or TEXT_MESSAGE_COMPRESSED_APP)
- Packet originated from another node (not from us)

```cpp
bool ReliableRouter::shouldSuccessAckWithWantAck(const meshtastic_MeshPacket *p)
{
    if (!p->want_ack || !isToUs(p) || isFromUs(p))
        return false;
    
    bool isTextMessage = 
        (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
        IS_ONE_OF(p->decoded.portnum, meshtastic_PortNum_TEXT_MESSAGE_APP, 
                  meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP);
    
    return isTextMessage;
}
```

**Logging:**
```
ACK SEND: to=0xXXXXXXXX, for_id=0xYYYYYYYY, want_ack=1, hop_limit=N (reliable ACK)
```

#### 2. Standard ACK
For most packets requesting acknowledgment:

**Conditions:**
- Packet has `want_ack=true`
- Packet is addressed to us
- Not an ACK/reply packet itself (no `request_id` or `reply_id`)

```cpp
if (!p->decoded.request_id && !p->decoded.reply_id) {
    uint8_t ackHopLimit = routingModule->getHopLimitForResponse(p->hop_start, p->hop_limit);
    LOG_INFO("ACK SEND: to=0x%x, for_id=0x%x, want_ack=0, hop_limit=%d (standard ACK)",
             getFrom(p), p->id, ackHopLimit);
    sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, ackHopLimit);
}
```

#### 3. ACK-of-ACK (Routing Protocol ACK)
When receiving ACK/NAK packets (which themselves have `request_id` set):

**Conditions:**
- Received packet is a ROUTING_APP message with `request_id` or `reply_id`
- Packet addressed to us

```cpp
if (p->decoded.request_id || p->decoded.reply_id) {
    uint8_t ackHopLimit = routingModule->getHopLimitForResponse(p->hop_start, p->hop_limit);
    LOG_INFO("ACK SEND: to=0x%x, for_id=0x%x, hop_limit=%d (ACK-of-ACK for ROUTING_APP)",
             getFrom(p), p->id, ackHopLimit);
    sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, ackHopLimit);
}
```

#### 4. Direct/Next-Hop ACK (hop_limit=0)
For packets received directly or via next-hop routing:

**Conditions:**
- Packet received directly from sender (`hop_start == hop_limit`)
- OR packet received via next-hop routing (`next_hop` matches our node)

```cpp
if ((p->hop_start > 0 && p->hop_start == p->hop_limit) || 
    p->next_hop != NO_NEXT_HOP_PREFERENCE) {
    LOG_INFO("ACK SEND: to=0x%x, for_id=0x%x, hop_limit=0 (direct/next-hop ACK)", 
             getFrom(p), p->id);
    sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0);
}
```

**Purpose:** Stop retransmissions at the immediate sender since they won't overhear implicit ACKs.

#### 5. Next-Hop ACK for want_ack=0 packets
Even packets without `want_ack` may need ACK if we were the explicit next hop:

```cpp
if ((p->want_ack || isToUs(p)) && 
    p->next_hop == nodeDB->getLastByteOfNodeNum(getNodeNum()) && 
    p->hop_limit > 0) {
    LOG_INFO("ACK SEND: to=0x%x, for_id=0x%x, hop_limit=0 (next-hop, want_ack=0)", 
             getFrom(p), p->id);
    sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0);
}
```

## NAK Sending Logic

### 1. Decryption Failure (NO_CHANNEL)
Sent when a `want_ack` packet cannot be decrypted:

```cpp
if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
    uint8_t nakHopLimit = routingModule->getHopLimitForResponse(p->hop_start, p->hop_limit);
    LOG_INFO("NAK SEND: to=0x%x, for_id=0x%x, err=NO_CHANNEL, hop_limit=%d",
             getFrom(p), p->id, nakHopLimit);
    sendAckNak(meshtastic_Routing_Error_NO_CHANNEL, getFrom(p), p->id, 
               channels.getPrimaryIndex(), nakHopLimit);
}
```

### 2. PKI Public Key Unknown
Sent when PKI encryption is required but sender's public key is not known:

```cpp
if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag && 
    p->channel == 0 &&
    (nodeDB->getMeshNode(p->from) == nullptr || 
     nodeDB->getMeshNode(p->from)->user.public_key.size == 0)) {
    uint8_t nakHopLimit = routingModule->getHopLimitForResponse(p->hop_start, p->hop_limit);
    LOG_INFO("NAK SEND: to=0x%x, for_id=0x%x, err=PKI_UNKNOWN_PUBKEY, hop_limit=%d",
             getFrom(p), p->id, nakHopLimit);
    sendAckNak(meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY, getFrom(p), p->id, 
               channels.getPrimaryIndex(), nakHopLimit);
}
```

**Special behavior:** Triggers sender to send NodeInfo with their public key:
```cpp
if (owner.public_key.size == 32) {
    LOG_INFO("PKI decrypt failure, send a NodeInfo");
    nodeInfoModule->sendOurNodeInfo(p->from, false, p->channel, true);
}
```

### 3. Max Retransmissions Exceeded
Sent when retransmission attempts are exhausted:

```cpp
if (p.numRetransmissions == 0) {
    if (isFromUs(p.packet)) {
        LOG_INFO("NAK SEND: to=0x%x, for_id=0x%x, err=MAX_RETRANSMIT (reliable send failed fr=0x%x)", 
                 getFrom(p.packet), p.packet->id, p.packet->from);
        sendAckNak(meshtastic_Routing_Error_MAX_RETRANSMIT, getFrom(p.packet), 
                   p.packet->id, p.packet->channel);
        
        // Notify AODV of link failure for route repair
        if (aodvModule && !isBroadcast(p.packet->to)) {
            LOG_INFO("AODV: Notifying link failure for 0x%x", p.packet->to);
            aodvModule->handleLinkFailure(p.packet->to);
        }
    }
}
```

## ACK Reception Logic

### ACK Detection
An ACK is identified by:
- Routing packet with `error_reason == NONE` AND `request_id != 0`
- OR non-routing packet with `request_id != 0`

```cpp
// ACK: !routing packet with request_id OR routing packet with error_reason == NONE
PacketId ackId = ((c && c->error_reason == meshtastic_Routing_Error_NONE) || !c) 
                 ? p->decoded.request_id : 0;

if (ackId) {
    LOG_INFO("ACK RECV: from=0x%x, for_id=0x%x, stopping retransmissions", p->from, ackId);
    stopRetransmission(p->to, ackId);
}
```

### NAK Detection
A NAK is identified by:
- Routing packet with `error_reason != NONE` AND `request_id != 0`

```cpp
// NAK: routing packet with error_reason != NONE
PacketId nakId = (c && c->error_reason != meshtastic_Routing_Error_NONE) 
                 ? p->decoded.request_id : 0;

if (nakId) {
    LOG_INFO("NAK RECV: from=0x%x, for_id=0x%x, err=%d, stopping retransmissions",
             p->from, nakId, c ? c->error_reason : 0);
    stopRetransmission(p->to, nakId);
}
```

### Stopping Retransmissions
When an ACK/NAK is received:

1. **Find pending packet:**
   ```cpp
   auto key = GlobalPacketId(from, id);
   auto old = findPendingPacket(key);
   ```

2. **Cancel from TX queue:**
   ```cpp
   if (old->numRetransmissions < NUM_RELIABLE_RETX - 1) {
       if (isFromUs(p) || roleAllowsCancelingFromTxQueue(p)) {
           cancelSending(getFrom(p), p->id);
       }
   }
   ```

3. **Remove from pending list:**
   ```cpp
   pending.erase(key);
   packetPool.release(p);
   ```

## Implicit ACKs

### Broadcast Implicit ACK
When a node overhears its own broadcast being rebroadcast by another node:

```cpp
if (p->from == getNodeNum() && (isBroadcast(p->to) || !p->want_ack)) {
    auto key = GlobalPacketId(getFrom(p), p->id);
    auto old = findPendingPacket(key);
    if (old) {
        LOG_INFO("ACK IMPLICIT: for_id=0x%x, heard rebroadcast from=0x%x (broadcast/unicast want_ack=0)",
                 p->id, p->from);
        sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, old->packet->channel);
        
        // Only stop retransmissions if rebroadcast came via LoRa
        if (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA) {
            stopRetransmission(key);
        }
    }
}
```

**Purpose:** Saves airtime by stopping retransmissions when we confirm the packet was heard and is being propagated.

## Hop Limit Calculation for ACKs

The hop limit for ACK responses is calculated intelligently:

```cpp
uint8_t RoutingModule::getHopLimitForResponse(uint8_t hopStart, uint8_t hopLimit)
{
    if (hopStart != 0) {
        // Calculate hops used by the request
        uint8_t hopsUsed = hopStart < hopLimit ? config.lora.hop_limit : hopStart - hopLimit;
        
        if (hopsUsed > config.lora.hop_limit) {
            return hopsUsed;  // Request used more hops than our limit
        } else if ((uint8_t)(hopsUsed + 2) < config.lora.hop_limit) {
            return hopsUsed + 2;  // Add margin for different return path
        }
    }
    return Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
}
```

**Logic:**
1. If request used more hops than configured limit → use same number of hops
2. If request used fewer hops → use `hopsUsed + 2` (allows different path)
3. If `hopStart` not set → use default configured hop limit

## Retransmission System

### Starting Retransmissions
When sending a packet with `want_ack=true`:

```cpp
ErrorCode ReliableRouter::send(meshtastic_MeshPacket *p)
{
    if (p->want_ack) {
        if (p->hop_limit == 0) {
            p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
        }
        auto copy = packetPool.allocCopy(*p);
        startRetransmission(copy, NUM_RELIABLE_RETX);  // Typically 3 retransmissions
    }
    // ...
}
```

### Retransmission Intervals
Retransmission timing accounts for:
- **Interface-specific delays** (LoRa airtime, etc.)
- **Airtime of other packets** (delays timer when sending/receiving other packets)

```cpp
void NextHopRouter::setNextTx(PendingPacket *pending)
{
    auto d = iface->getRetransmissionMsec(pending->packet);
    pending->nextTxMsec = millis() + d;
    LOG_DEBUG("Setting next retransmission in %u msecs", d);
}
```

### Retransmission Execution
Periodic check for packets due for retransmission:

```cpp
int32_t NextHopRouter::doRetransmissions()
{
    uint32_t now = millis();
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        auto &p = it->second;
        if (p.nextTxMsec <= now) {
            if (p.numRetransmissions == 0) {
                // Send MAX_RETRANSMIT NAK
                sendAckNak(meshtastic_Routing_Error_MAX_RETRANSMIT, ...);
                stopRetransmission(it->first);
            } else {
                // Retransmit
                NextHopRouter::send(packetPool.allocCopy(*p.packet));
                --p.numRetransmissions;
                setNextTx(&p);
            }
        }
    }
}
```

## Message Flow Examples

### Example 1: Successful Direct Message with ACK

```
Node A (0xAAAAAAAA) → Node B (0xBBBBBBBB)

1. [A] Send packet: id=0x1234, to=0xBBBBBBBB, want_ack=1
   LOG: "DATA SEND: port=1, dest=0xBBBBBBBB, next_hop=0xNN, id=0x1234"
   - Start retransmission timer

2. [B] Receive packet: id=0x1234, from=0xAAAAAAAA
   LOG: "ACK SEND: to=0xAAAAAAAA, for_id=0x1234, want_ack=1, hop_limit=N (reliable ACK)"
   - Send ACK packet: id=0x5678, request_id=0x1234

3. [A] Receive ACK: from=0xBBBBBBBB, request_id=0x1234
   LOG: "ACK RECV: from=0xBBBBBBBB, for_id=0x1234, stopping retransmissions"
   - Cancel retransmissions for 0x1234
   
4. [A] Receive ACK packet from step 2 (want_ack=1)
   LOG: "ACK SEND: to=0xBBBBBBBB, for_id=0x5678, hop_limit=0 (ACK-of-ACK)"
   - Send ACK-of-ACK for reliable ACK delivery

5. [B] Receive ACK-of-ACK: request_id=0x5678
   LOG: "ACK RECV: from=0xAAAAAAAA, for_id=0x5678, stopping retransmissions"
   - Cancel retransmissions for ACK packet
```

### Example 2: Broadcast with Implicit ACK

```
Node A (0xAAAAAAAA) → BROADCAST

1. [A] Send broadcast: id=0x1234, to=0xFFFFFFFF
   LOG: "DATA BCAST: port=1, id=0x1234, hop_limit=3"
   - Start retransmission timer

2. [B] Receive broadcast: from=0xAAAAAAAA, id=0x1234
   - Rebroadcast packet

3. [A] Overhear rebroadcast: from=0xAAAAAAAA (relayed by B)
   LOG: "ACK IMPLICIT: for_id=0x1234, heard rebroadcast from=0xAAAAAAAA"
   - Cancel retransmissions for 0x1234
   - Generate internal ACK to sending layer
```

### Example 3: Failed Delivery (Max Retransmissions)

```
Node A (0xAAAAAAAA) → Node B (0xBBBBBBBB) [B is unreachable]

1. [A] Send packet: id=0x1234, to=0xBBBBBBBB, want_ack=1
   LOG: "DATA SEND: port=1, dest=0xBBBBBBBB, next_hop=0xNN, id=0x1234"

2. [A] Wait for ACK... timeout
   LOG: "Sending retransmission fr=0xAAAAAAAA, to=0xBBBBBBBB, id=0x1234, tries left=2"

3. [A] Retransmit (attempt 2)

4. [A] Wait for ACK... timeout
   LOG: "Sending retransmission fr=0xAAAAAAAA, to=0xBBBBBBBB, id=0x1234, tries left=1"

5. [A] Retransmit (attempt 3)

6. [A] Wait for ACK... timeout, retries exhausted
   LOG: "NAK SEND: to=0xAAAAAAAA, for_id=0x1234, err=MAX_RETRANSMIT"
   LOG: "AODV: Notifying link failure for 0x%x"
   - Send NAK to self
   - Notify AODV for route repair
   - Stop retransmissions
```

### Example 4: Decryption Failure

```
Node A (0xAAAAAAAA) → Node B (0xBBBBBBBB) [B can't decrypt]

1. [A] Send encrypted packet: id=0x1234, to=0xBBBBBBBB, want_ack=1

2. [B] Receive encrypted packet: cannot decrypt
   LOG: "NAK SEND: to=0xAAAAAAAA, for_id=0x1234, err=NO_CHANNEL, hop_limit=N"
   - Send NAK with NO_CHANNEL error

3. [A] Receive NAK: from=0xBBBBBBBB, request_id=0x1234, err=NO_CHANNEL
   LOG: "NAK RECV: from=0xBBBBBBBB, for_id=0x1234, err=6, stopping retransmissions"
   - Stop retransmissions
   - Application layer notified of delivery failure
```

## Integration with AODV Routing

The ACK system integrates with AODV for route maintenance:

### Link Failure Detection
When max retransmissions are reached, AODV is notified:

```cpp
if (aodvModule && !isBroadcast(p.packet->to)) {
    LOG_INFO("AODV: Notifying link failure for 0x%x", p.packet->to);
    aodvModule->handleLinkFailure(p.packet->to);
}
```

### Route Repair
AODV responds to link failures by:
1. Invalidating the failed route
2. Sending RERR (Route Error) upstream
3. Initiating new route discovery if needed

## Configuration

### Retransmission Count
Default: `NUM_RELIABLE_RETX = 3`

### Hop Limit
- Default: `config.lora.hop_limit` (typically 3-7)
- Can be overridden per-packet
- ACKs use intelligent hop limit calculation

## Logging Reference

All ACK-related operations are logged with specific prefixes:

- `ACK SEND:` - Sending an ACK packet
- `ACK RECV:` - Received an ACK packet
- `ACK IMPLICIT:` - Implicit ACK detected (overhearing rebroadcast)
- `NAK SEND:` - Sending a NAK packet
- `NAK RECV:` - Received a NAK packet

Log format typically includes:
- `to=0xXXXXXXXX` - Destination node
- `from=0xXXXXXXXX` - Source node
- `for_id=0xXXXXXXXX` - Packet ID being acknowledged
- `hop_limit=N` - Hop limit used
- `want_ack=N` - Whether ACK requests acknowledgment
- `err=N` - Error code (for NAKs)

## Summary

The Meshtastic ACK system provides:

1. **Reliability**: Automatic retransmissions until ACK received or max attempts
2. **Efficiency**: Implicit ACKs save airtime on broadcasts
3. **Intelligent routing**: Hop-limit calculation optimizes ACK delivery
4. **Error reporting**: NAKs inform senders of specific failure reasons
5. **AODV integration**: Failed deliveries trigger route repair
6. **Layered approach**: ACKs work at multiple router layers (Reliable, NextHop, Flooding)

This ensures robust message delivery across unreliable wireless mesh networks while minimizing unnecessary transmissions.
