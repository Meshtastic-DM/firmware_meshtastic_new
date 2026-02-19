# SimRadio PKI Support

## Overview

This document describes the PKI (Public Key Infrastructure) encryption support added to the SimRadio implementation for the Portduino simulator platform. This enhancement allows the simulator to properly handle encrypted packets that cannot be decrypted locally, enabling full end-to-end PKI testing in the simulator environment.

**Commit:** `8f179b1a1d3f7c01a852b3ebdca13cd16d04d3d5`  
**Date:** February 15, 2026  
**File:** `src/platform/portduino/SimRadio.cpp`

## Problem Statement

Prior to this change, SimRadio could only handle packets that were successfully decoded locally. When a PKI-encrypted packet was received that the local node couldn't decrypt (missing private key, different recipient, etc.), the simulator would:

1. Fail to decode the packet
2. Send empty or corrupted data to the simulator UI
3. Lose the ability to display and relay encrypted packets

This made it impossible to properly test PKI encryption flows in the simulator, particularly multi-hop scenarios where intermediate nodes relay encrypted packets they cannot decrypt.

## Solution Architecture

### Protocol Design

The solution uses the `Compressed` message wrapper with a special marker to distinguish between plaintext and ciphertext:

```protobuf
message Compressed {
    PortNum portnum = 1;  // UNKNOWN_APP = ciphertext marker
    bytes data = 2;       // Payload (plaintext) or ciphertext
}
```

**Marker Convention:**
- `portnum == UNKNOWN_APP` (0) → `data` contains ciphertext (encrypted payload)
- `portnum != UNKNOWN_APP` → `data` contains plaintext (decoded payload)

All simulator packets are wrapped in `PortNum_SIMULATOR_APP` packets with `Compressed` as the payload.

## Implementation Details

### 1. Transmit Path: `startSend()`

When sending a packet from the device to the simulator UI:

```cpp
void SimRadio::startSend(meshtastic_MeshPacket *txp)
{
    // 1. Copy packet for decoding attempt
    meshtastic_MeshPacket *p = packetPool.allocCopy(*txp);
    
    // 2. Try to decode (works for PSK, already-decoded, or decryptable PKI)
    DecodeState st = perhapsDecode(p);
    
    meshtastic_Compressed c = meshtastic_Compressed_init_default;
    
    // 3. Check decode result
    if (st == DecodeState::DECODE_SUCCESS &&
        p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        
        // SUCCESS: Send plaintext payload
        c.portnum = p->decoded.portnum;
        memcpy(c.data.bytes, p->decoded.payload.bytes, p->decoded.payload.size);
        c.data.size = p->decoded.payload.size;
        
    } else {
        // FAILURE: Send ciphertext with UNKNOWN_APP marker
        c.portnum = meshtastic_PortNum_UNKNOWN_APP;
        
        if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
            memcpy(c.data.bytes, p->encrypted.bytes, p->encrypted.size);
            c.data.size = p->encrypted.size;
        } else {
            c.data.size = 0;  // Corrupted/unknown
        }
    }
    
    // 4. Wrap Compressed in SIMULATOR_APP packet and send to UI
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.payload.size = pb_encode_to_bytes(
        p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes),
        &meshtastic_Compressed_msg, &c
    );
    p->decoded.portnum = meshtastic_PortNum_SIMULATOR_APP;
    
    service->sendToPhone(p);
}
```

#### Key Changes:

**Before:**
```cpp
size_t numbytes = beginSending(txp);
meshtastic_MeshPacket *p = packetPool.allocCopy(*txp);
perhapsDecode(p);  // No error checking
c.portnum = p->decoded.portnum;  // Assumes success
```

**After:**
```cpp
beginSending(txp);
meshtastic_MeshPacket *p = packetPool.allocCopy(*txp);
DecodeState st = perhapsDecode(p);  // Check result

if (st == DecodeState::DECODE_SUCCESS && ...) {
    // Send plaintext
} else {
    // Send ciphertext with marker
}
```

### 2. Receive Path: `unpackAndReceive()`

When receiving a packet from the simulator UI:

```cpp
void SimRadio::unpackAndReceive(meshtastic_MeshPacket &p)
{
    meshtastic_Compressed scratch;
    meshtastic_Compressed *decoded = NULL;
    
    if (p.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        // 1. Decode Compressed wrapper
        size_t sz = pb_decode_from_bytes(
            p.decoded.payload.bytes, p.decoded.payload.size,
            &meshtastic_Compressed_msg, &scratch
        );
        
        if (sz) {
            decoded = &scratch;
            
            // 2. Check for ciphertext marker
            if (decoded->portnum == meshtastic_PortNum_UNKNOWN_APP) {
                
                // CIPHERTEXT: Switch to encrypted variant
                p.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
                
                memset(&p.encrypted, 0, sizeof(p.encrypted));
                memcpy(p.encrypted.bytes, decoded->data.bytes, decoded->data.size);
                p.encrypted.size = decoded->data.size;
                
            } else {
                // PLAINTEXT: Extract original payload
                memcpy(&p.decoded.payload, &decoded->data, sizeof(decoded->data));
                p.decoded.portnum = decoded->portnum;
            }
        }
    }
    
    // 3. Process packet as if received via LoRa
    startReceive(&p);
}
```

#### Key Changes:

**Before:**
```cpp
memcpy(&p.decoded.payload, &decoded->data, sizeof(decoded->data));
p.decoded.portnum = decoded->portnum;
// Always assumed plaintext
```

**After:**
```cpp
if (decoded->portnum == meshtastic_PortNum_UNKNOWN_APP) {
    // Ciphertext: switch to encrypted variant
    p.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    memcpy(p.encrypted.bytes, decoded->data.bytes, decoded->data.size);
    p.encrypted.size = decoded->data.size;
} else {
    // Plaintext: normal handling
    memcpy(&p.decoded.payload, &decoded->data, sizeof(decoded->data));
    p.decoded.portnum = decoded->portnum;
}
```

## Complete Pipeline: SimRadio ↔ Simulator UI

### Simple Flow Diagram (Visual Slide)

```
Device A          Simulator UI         Device B
────────          ────────────         ────────

   │                                      │
   │  MeshPacket (encrypted)              │
   │  PKI for Device B                    │
   │                                      │
   ├──► perhapsDecode()                   │
   │    └─ FAILED (not for us)            │
   │                                      │
   │    Compressed                        │
   │    ├─ portnum = 0 ◄────────────────── Marker: Ciphertext
   │    └─ data = <ciphertext>            │
   │         │                             │
   │         ▼                             │
   ├──► SIMULATOR_APP ─────────────────►  │
   │                                      │
   │                   Display:           │
   │                   "ENCRYPTED"        │
   │                   Forward packet ───►│
   │                                      │
   │                       Compressed     │
   │                       portnum = 0    │
   │                       data = <cipher>│
   │                             │        │
   │                             ▼        │
   │                   unpackAndReceive() │
   │                   portnum=0?         │
   │                   └─ YES             │
   │                                      │
   │                   Create MeshPacket  │
   │                   encrypted_tag ◄──── Reconstruct
   │                   encrypted.bytes    │
   │                             │        │
   │                             ▼        │
   │                   perhapsDecode()    │
   │                   └─ SUCCESS!        │
   │                                      │
   │                   "Hello World" ◄──── Decrypted
   │                                      │
```

### Quick Reference Table (Slide)

| Direction | Input | Transform | Marker | Output |
|-----------|-------|-----------|--------|--------|
| **Send (Device→UI)** | MeshPacket encrypted | perhapsDecode() FAILED | portnum=**0** | Ciphertext preserved |
| **Send (Device→UI)** | MeshPacket decoded | perhapsDecode() SUCCESS | portnum=**1+** | Plaintext forwarded |
| **Recv (UI→Device)** | Compressed portnum=0 | unpackAndReceive() | UNKNOWN_APP | encrypted_tag variant |
| **Recv (UI→Device)** | Compressed portnum≠0 | unpackAndReceive() | Original portnum | decoded_tag variant |

**Key:** `portnum=0` (UNKNOWN_APP) = ciphertext marker

### Ultra-Compact Pipeline (Tight Slide)

| Direction | Input | Transform | Marker | Output |
|-----------|-------|-----------|--------|--------|
| **Send (Device→UI)** | MeshPacket encrypted | perhapsDecode() FAILED | portnum=**0** | Ciphertext preserved |
| **Send (Device→UI)** | MeshPacket decoded | perhapsDecode() SUCCESS | portnum=**1+** | Plaintext forwarded |
| **Recv (UI→Device)** | Compressed portnum=0 | unpackAndReceive() | UNKNOWN_APP | encrypted_tag variant |
| **Recv (UI→Device)** | Compressed portnum≠0 | unpackAndReceive() | Original portnum | decoded_tag variant |

**Key:** `portnum=0` (UNKNOWN_APP) = ciphertext marker

### Ultra-Compact Pipeline (Tight Slide)

```
┌────────────────────────────────────────────────────┐
│      SimRadio PKI: Ciphertext Preservation         │
└────────────────────────────────────────────────────┘

SEND: Device → UI
─────────────────
  Decrypt? ──┬── YES → Compressed(portnum=1, data=plaintext)
             └── NO  → Compressed(portnum=0, data=ciphertext)
                                      ▲
                                      └─ UNKNOWN_APP = marker
RECV: UI → Device
─────────────────
  portnum=0? ─┬── YES → encrypted_tag variant
              └── NO  → decoded_tag variant

Key: portnum=0 preserves ciphertext through simulator
```

### Minimal Pipeline (Single Slide)

```
┌────────────────────────────────────────────────────┐
│      SimRadio PKI: Ciphertext Preservation         │
└────────────────────────────────────────────────────┘

SEND: Device → UI
─────────────────
  Decrypt? ──┬── YES → Compressed(portnum=1, data=plaintext)
             └── NO  → Compressed(portnum=0, data=ciphertext)
                                      ▲
                                      └─ UNKNOWN_APP = marker
RECV: UI → Device
─────────────────
  portnum=0? ─┬── YES → encrypted_tag variant
              └── NO  → decoded_tag variant

Key: portnum=0 preserves ciphertext through simulator
```

### Minimal Pipeline (Single Slide)

```
SimRadio PKI Support - Bidirectional Pipeline
══════════════════════════════════════════════

SEND: Device → Simulator UI
─────────────────────────────
    MeshPacket → perhapsDecode() → Compressed → SIMULATOR_APP → UI
                       │              │
                   ┌───┴───┐          │
               SUCCESS  FAILED        │
                   │       │          │
                   ▼       ▼          │
              Plaintext  Ciphertext   │
              portnum=1  portnum=0 ◄──┘ (UNKNOWN_APP = marker)


RECEIVE: Simulator UI → Device
───────────────────────────────
    UI → SIMULATOR_APP → unpackAndReceive() → Check portnum → MeshPacket
                                                     │
                                                 ┌───┴────┐
                                            portnum=0  portnum≠0
                                                 │         │
                                                 ▼         ▼
                                            encrypted  decoded
                                             variant   variant

KEY INSIGHT: portnum=0 (UNKNOWN_APP) signals ciphertext
             portnum≠0 signals plaintext with original portnum
```

### Simplified Pipeline (Slide Version)

```
SimRadio PKI Support - Bidirectional Pipeline
══════════════════════════════════════════════

SEND: Device → Simulator UI
─────────────────────────────
    MeshPacket → perhapsDecode() → Compressed → SIMULATOR_APP → UI
                       │              │
                   ┌───┴───┐          │
               SUCCESS  FAILED        │
                   │       │          │
                   ▼       ▼          │
              Plaintext  Ciphertext   │
              portnum=1  portnum=0 ◄──┘ (UNKNOWN_APP = marker)


RECEIVE: Simulator UI → Device
───────────────────────────────
    UI → SIMULATOR_APP → unpackAndReceive() → Check portnum → MeshPacket
                                                     │
                                                 ┌───┴────┐
                                            portnum=0  portnum≠0
                                                 │         │
                                                 ▼         ▼
                                            encrypted  decoded
                                             variant   variant

KEY INSIGHT: portnum=0 (UNKNOWN_APP) signals ciphertext
             portnum≠0 signals plaintext with original portnum
```

### Simplified Pipeline (Slide Version)

```
┌──────────────────────────────────────────────────────────────────────┐
│                     SimRadio PKI Pipeline                             │
└──────────────────────────────────────────────────────────────────────┘

TRANSMIT PATH (SimRadio → Simulator UI)
═════════════════════════════════════════

    MeshPacket (Original)
         │
         ├─ encrypted_tag → PKI ciphertext
         └─ decoded_tag → plaintext
         │
         ▼
    perhapsDecode()  ◄─── Try to decrypt
         │
    ┌────┴────┐
    │         │
SUCCESS    FAILED
    │         │
    ▼         ▼
┌─────────┐  ┌──────────────┐
│Plaintext│  │  Ciphertext  │
└─────────┘  └──────────────┘
    │              │
    ▼              ▼
Compressed     Compressed
portnum: 1     portnum: 0 ◄─── UNKNOWN_APP marker
data: "Hi"     data: <48B>
    │              │
    └──────┬───────┘
           │
           ▼
    SIMULATOR_APP wrapper
           │
           ▼
    Simulator UI Display
    ├─ portnum ≠ 0: "TEXT_MESSAGE: Hi"
    └─ portnum = 0: "ENCRYPTED: 48 bytes"


RECEIVE PATH (Simulator UI → SimRadio)
═══════════════════════════════════════

    Simulator UI sends packet
           │
           ▼
    SIMULATOR_APP wrapper
    (contains Compressed)
           │
           ▼
    unpackAndReceive()
    ├─ Decode Compressed
    └─ Check portnum
           │
    ┌──────┴───────┐
    │              │
portnum = 0    portnum ≠ 0
    │              │
    ▼              ▼
encrypted_tag  decoded_tag
encrypted:     decoded:
<48 bytes>    portnum: 1
              payload: "Hi"
    │              │
    └──────┬───────┘
           │
           ▼
    startReceive()
    Normal processing
```

### Detailed Pipeline Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                     SimRadio PKI Pipeline                             │
└──────────────────────────────────────────────────────────────────────┘

TRANSMIT PATH (SimRadio → Simulator UI)
═════════════════════════════════════════

    MeshPacket (Original)
         │
         ├─ encrypted_tag → PKI ciphertext
         └─ decoded_tag → plaintext
         │
         ▼
    perhapsDecode()  ◄─── Try to decrypt
         │
    ┌────┴────┐
    │         │
SUCCESS    FAILED
    │         │
    ▼         ▼
┌─────────┐  ┌──────────────┐
│Plaintext│  │  Ciphertext  │
└─────────┘  └──────────────┘
    │              │
    ▼              ▼
Compressed     Compressed
portnum: 1     portnum: 0 ◄─── UNKNOWN_APP marker
data: "Hi"     data: <48B>
    │              │
    └──────┬───────┘
           │
           ▼
    SIMULATOR_APP wrapper
           │
           ▼
    Simulator UI Display
    ├─ portnum ≠ 0: "TEXT_MESSAGE: Hi"
    └─ portnum = 0: "ENCRYPTED: 48 bytes"


RECEIVE PATH (Simulator UI → SimRadio)
═══════════════════════════════════════

    Simulator UI sends packet
           │
           ▼
    SIMULATOR_APP wrapper
    (contains Compressed)
           │
           ▼
    unpackAndReceive()
    ├─ Decode Compressed
    └─ Check portnum
           │
    ┌──────┴───────┐
    │              │
portnum = 0    portnum ≠ 0
    │              │
    ▼              ▼
encrypted_tag  decoded_tag
encrypted:     decoded:
<48 bytes>    portnum: 1
              payload: "Hi"
    │              │
    └──────┬───────┘
           │
           ▼
    startReceive()
    Normal processing
```

### Detailed Pipeline Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    SIMRADIO → SIMULATOR UI PIPELINE                      │
└─────────────────────────────────────────────────────────────────────────┘

SimRadio Device                    Transformations                  Simulator UI
─────────────────                 ─────────────────                ──────────────

MeshPacket (TX)                                                    
├─ to: 0xBBBBBBBB                                                  
├─ from: 0xAAAAAAAA                                                
├─ channel: 0                                                      
└─ which_payload_variant:                                          
   ├─ encrypted_tag ──────┐                                        
   │  └─ encrypted.bytes   │                                       
   │     encrypted.size    │                                       
   └─ decoded_tag          │                                       
      └─ decoded.portnum   │                                       
         decoded.payload   │                                       
                           │                                       
                           ▼                                       
        ┌──────────────────────────────────────┐                  
        │  startSend(txp)                      │                  
        │  ├─ beginSending(txp)                │                  
        │  └─ allocCopy(*txp) → p              │                  
        └──────────────────────────────────────┘                  
                           │                                       
                           ▼                                       
        ┌──────────────────────────────────────┐                  
        │  DecodeState st = perhapsDecode(p)   │                  
        │  ├─ Attempts to decrypt packet       │                  
        │  ├─ DECODE_SUCCESS: plaintext ready  │                  
        │  └─ DECODE_FAILED: keep ciphertext   │                  
        └──────────────────────────────────────┘                  
                           │                                       
                ┌──────────┴──────────┐                           
                │                     │                           
        DECODE_SUCCESS        DECODE_FAILED                       
                │                     │                           
                ▼                     ▼                           
    ┌───────────────────────┐  ┌──────────────────────┐          
    │ Plaintext Path        │  │ Ciphertext Path      │          
    │                       │  │                      │          
    │ Compressed c:         │  │ Compressed c:        │          
    │ ├─ portnum =          │  │ ├─ portnum =         │          
    │ │  TEXT_MESSAGE_APP   │  │ │  UNKNOWN_APP (0)   │          
    │ └─ data.bytes =       │  │ └─ data.bytes =      │          
    │    "Hello World"      │  │    <encrypted bytes> │          
    │    data.size = 11     │  │    data.size = 48    │          
    └───────────────────────┘  └──────────────────────┘          
                │                     │                           
                └──────────┬──────────┘                           
                           │                                       
                           ▼                                       
        ┌──────────────────────────────────────┐                  
        │  Wrap in MeshPacket (SIMULATOR_APP)  │                  
        │  ├─ which_payload_variant =          │                  
        │  │  meshtastic_MeshPacket_decoded_tag│                  
        │  ├─ decoded.portnum =                │                  
        │  │  SIMULATOR_APP                    │                  
        │  └─ decoded.payload =                │                  
        │     pb_encode(Compressed_msg, &c)    │                  
        └──────────────────────────────────────┘                  
                           │                                       
                           ▼                                       
        ┌──────────────────────────────────────┐                  
        │  service->sendToPhone(p)             │                  
        │  service->loop()                     │                  
        └──────────────────────────────────────┘                  
                           │                                       
                           ▼                                       
                    [Network/IPC]                                  
                           │                                       
                           ▼                                                    ┌─────────────────────┐
                                                                                │ Simulator UI        │
                                                                                │                     │
                                                                                │ Receive packet:     │
                                                                                │ ├─ portnum=         │
                                                                                │ │  SIMULATOR_APP    │
                                                                                │ └─ payload=         │
                                                                                │    Compressed proto │
                                                                                │                     │
                                                                                │ Decode Compressed:  │
                                                                                │ ├─ portnum check    │
                                                                                │ └─ data extraction  │
                                                                                │                     │
                                                                                │ ┌─ If portnum ≠ 0   │
                                                                                │ │  Display:         │
                                                                                │ │  "TEXT_MESSAGE:   │
                                                                                │ │   Hello World"    │
                                                                                │ │                   │
                                                                                │ └─ If portnum = 0   │
                                                                                │    Display:         │
                                                                                │    "ENCRYPTED:      │
                                                                                │     <48 bytes>"     │
                                                                                └─────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                    SIMULATOR UI → SIMRADIO PIPELINE                      │
└─────────────────────────────────────────────────────────────────────────┘

Simulator UI                       Transformations                  SimRadio Device
────────────────                  ─────────────────                 ─────────────────

User Action:                                                        
"Send packet to Device B"                                           
                           │                                        
                           ▼                                        
        ┌──────────────────────────────────────┐                   
        │ Simulator constructs:                │                   
        │                                      │                   
        │ Compressed wrapper:                  │                   
        │ ├─ portnum = TEXT_MESSAGE_APP        │   (plaintext)     
        │ │  OR portnum = UNKNOWN_APP          │   (ciphertext)    
        │ └─ data = payload bytes              │                   
        └──────────────────────────────────────┘                   
                           │                                        
                           ▼                                        
        ┌──────────────────────────────────────┐                   
        │ Wrap in MeshPacket:                  │                   
        │ ├─ to = 0xBBBBBBBB                   │                   
        │ ├─ from = 0xAAAAAAAA                 │                   
        │ ├─ which_payload_variant =           │                   
        │ │  decoded_tag                       │                   
        │ ├─ decoded.portnum = SIMULATOR_APP   │                   
        │ └─ decoded.payload =                 │                   
        │    pb_encode(Compressed)             │                   
        └──────────────────────────────────────┘                   
                           │                                        
                           ▼                                        
                    [Network/IPC]                                   
                           │                                        
                           ▼                                                                         ┌──────────────────────┐
                                                                                                     │ SimRadio Device      │
                                                                                                     │                      │
                                                                                                     │ unpackAndReceive(p)  │
                                                                                                     └──────────────────────┘
                                                                                                                │
                                                                                                                ▼
                                                                             ┌──────────────────────────────────────────┐
                                                                             │ Decode Compressed from payload:          │
                                                                             │ pb_decode_from_bytes(                    │
                                                                             │   p.decoded.payload.bytes,               │
                                                                             │   Compressed_msg, &scratch)              │
                                                                             └──────────────────────────────────────────┘
                                                                                                │
                                                                                    ┌───────────┴──────────┐
                                                                                    │                      │
                                                                            portnum = 0           portnum ≠ 0
                                                                          UNKNOWN_APP          (normal portnum)
                                                                                    │                      │
                                                                                    ▼                      ▼
                                                                    ┌───────────────────────┐  ┌──────────────────────┐
                                                                    │ Ciphertext Path       │  │ Plaintext Path       │
                                                                    │                       │  │                      │
                                                                    │ p.which_payload =     │  │ p.which_payload =    │
                                                                    │   encrypted_tag       │  │   decoded_tag        │
                                                                    │                       │  │                      │
                                                                    │ p.encrypted.bytes =   │  │ p.decoded.portnum =  │
                                                                    │   decoded->data.bytes │  │   decoded->portnum   │
                                                                    │ p.encrypted.size =    │  │                      │
                                                                    │   decoded->data.size  │  │ p.decoded.payload =  │
                                                                    │                       │  │   decoded->data      │
                                                                    │ Result:               │  │                      │
                                                                    │ MeshPacket with       │  │ Result:              │
                                                                    │ encrypted payload     │  │ MeshPacket with      │
                                                                    │                       │  │ decoded payload      │
                                                                    └───────────────────────┘  └──────────────────────┘
                                                                                    │                      │
                                                                                    └───────────┬──────────┘
                                                                                                │
                                                                                                ▼
                                                                             ┌──────────────────────────────────────────┐
                                                                             │ startReceive(&p)                         │
                                                                             │ ├─ Process as if received via LoRa       │
                                                                             │ ├─ Router handles routing                │
                                                                             │ └─ Modules process packet                │
                                                                             └──────────────────────────────────────────┘
                                                                                                │
                                                                                                ▼
                                                                                        [Normal packet flow]
```

### Detailed Pipeline Stages

#### Stage 1: MeshPacket Creation (Application Layer)
```cpp
// Application creates packet
meshtastic_MeshPacket *txp;
txp->to = 0xBBBBBBBB;
txp->from = 0xAAAAAAAA;
txp->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
strcpy(txp->decoded.payload.bytes, "Hello World");
txp->decoded.payload.size = 11;
// OR: txp->encrypted.bytes = <ciphertext> for PKI
```

#### Stage 2: Transmission Initiation (SimRadio::startSend)
```cpp
void SimRadio::startSend(meshtastic_MeshPacket *txp)
{
    // Step 2.1: Signal transmission start
    isReceiving = false;
    beginSending(txp);
    
    // Step 2.2: Copy packet for processing
    meshtastic_MeshPacket *p = packetPool.allocCopy(*txp);
    
    // Step 2.3: Attempt decoding
    DecodeState st = perhapsDecode(p);
    
    // Result: st = DECODE_SUCCESS or DECODE_FAILED
    //         p->which_payload_variant = decoded_tag or encrypted_tag
}
```

#### Stage 3: Compressed Message Creation
```cpp
meshtastic_Compressed c = meshtastic_Compressed_init_default;

// PATH A: Decode Success → Plaintext
if (st == DecodeState::DECODE_SUCCESS &&
    p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
    
    c.portnum = p->decoded.portnum;  // e.g., TEXT_MESSAGE_APP = 1
    memcpy(c.data.bytes, p->decoded.payload.bytes, p->decoded.payload.size);
    c.data.size = p->decoded.payload.size;
    
    // Result: Compressed { portnum: 1, data: "Hello World" (11 bytes) }
}

// PATH B: Decode Failed → Ciphertext
else {
    c.portnum = meshtastic_PortNum_UNKNOWN_APP;  // = 0 (marker)
    
    if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
        memcpy(c.data.bytes, p->encrypted.bytes, p->encrypted.size);
        c.data.size = p->encrypted.size;
    }
    
    // Result: Compressed { portnum: 0, data: <48 bytes of ciphertext> }
}
```

#### Stage 4: SIMULATOR_APP Wrapper
```cpp
// Reset packet to decoded variant for wrapper
p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
memset(&p->decoded, 0, sizeof(p->decoded));

// Encode Compressed into payload
p->decoded.payload.size = pb_encode_to_bytes(
    p->decoded.payload.bytes,
    sizeof(p->decoded.payload.bytes),
    &meshtastic_Compressed_msg,
    &c
);

// Set portnum to SIMULATOR_APP
p->decoded.portnum = meshtastic_PortNum_SIMULATOR_APP;

// Result: MeshPacket {
//   which_payload_variant: decoded_tag,
//   decoded: {
//     portnum: SIMULATOR_APP (88),
//     payload: <Compressed protobuf bytes>
//   }
// }
```

#### Stage 5: Send to Simulator UI
```cpp
service->sendQueueStatusToPhone(router->getQueueStatus(), 0, p->id);
service->sendToPhone(p);
service->loop();

// Packet transmitted over IPC/network to simulator UI
```

#### Stage 6: Simulator UI Processing
```javascript
// Pseudo-code for UI
function onPacketReceived(meshPacket) {
    if (meshPacket.decoded.portnum === SIMULATOR_APP) {
        // Decode Compressed wrapper
        let compressed = Compressed.decode(meshPacket.decoded.payload);
        
        if (compressed.portnum === UNKNOWN_APP) {
            // Ciphertext
            display(`ENCRYPTED: ${compressed.data.length} bytes`);
            displayHex(compressed.data);
        } else {
            // Plaintext
            display(`${getPortName(compressed.portnum)}: ${compressed.data}`);
        }
        
        // Store for forwarding to other devices
        storePacket(compressed);
    }
}
```

#### Stage 7: Simulator UI Forwarding
```javascript
// When user clicks "Forward to Device B"
function forwardToDevice(compressed, targetDevice) {
    let meshPacket = {
        to: targetDevice.nodeId,
        from: sourceDevice.nodeId,
        which_payload_variant: "decoded",
        decoded: {
            portnum: SIMULATOR_APP,
            payload: Compressed.encode(compressed)  // Re-encode same Compressed
        }
    };
    
    sendToDevice(targetDevice, meshPacket);
}
```

#### Stage 8: Receive at SimRadio (unpackAndReceive)
```cpp
void SimRadio::unpackAndReceive(meshtastic_MeshPacket &p)
{
    meshtastic_Compressed scratch;
    
    // Step 8.1: Decode Compressed wrapper
    size_t sz = pb_decode_from_bytes(
        p.decoded.payload.bytes,
        p.decoded.payload.size,
        &meshtastic_Compressed_msg,
        &scratch
    );
    
    if (!sz) {
        LOG_ERROR("Error decoding proto for simulator message!");
        return;
    }
    
    // Step 8.2: Check portnum marker
    if (scratch.portnum == meshtastic_PortNum_UNKNOWN_APP) {
        // CIPHERTEXT PATH
        p.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memset(&p.encrypted, 0, sizeof(p.encrypted));
        memcpy(p.encrypted.bytes, scratch.data.bytes, scratch.data.size);
        p.encrypted.size = scratch.data.size;
        
        // Result: MeshPacket { encrypted_tag, encrypted: <48 bytes> }
    } else {
        // PLAINTEXT PATH
        p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        memcpy(&p.decoded.payload, &scratch.data, sizeof(scratch.data));
        p.decoded.portnum = scratch.portnum;
        
        // Result: MeshPacket { decoded_tag, decoded: { portnum: 1, payload: "Hello" } }
    }
    
    // Step 8.3: Process as received packet
    startReceive(&p);
}
```

#### Stage 9: Normal Packet Processing
```cpp
void SimRadio::startReceive(meshtastic_MeshPacket *p)
{
    // Packet enters normal Meshtastic processing:
    // 1. Router checks routing
    // 2. perhapsDecode() attempts decryption (if encrypted)
    // 3. Modules process payload
    // 4. ACK/response generation
    // 5. Retransmission to other nodes
    
    // If encrypted and we can decrypt:
    //   - perhapsDecode() succeeds
    //   - Modules see plaintext
    
    // If encrypted and we cannot decrypt:
    //   - perhapsDecode() fails
    //   - Router relays encrypted packet
    //   - Eventually reaches intended recipient
}
```

## Message Flow Examples

### Example 1: Plaintext Packet (PSK or No Encryption)

```
Device A (Simulator)
    |
    +-- Send TEXT_MESSAGE_APP packet (PSK encrypted)
        |
        +-- perhapsDecode() → DECODE_SUCCESS (we have PSK)
        |
        +-- Create Compressed:
            - portnum = TEXT_MESSAGE_APP
            - data = "Hello World" (plaintext)
        |
        +-- Wrap in SIMULATOR_APP and send to UI
    |
    v
Simulator UI
    |
    +-- Display: "TEXT_MESSAGE_APP: Hello World"
    |
    +-- Forward to Device B (simulator)
        |
        +-- Wrap in Compressed (portnum=TEXT_MESSAGE_APP)
        |
        +-- Send as SIMULATOR_APP packet
    |
    v
Device B (Simulator)
    |
    +-- unpackAndReceive()
        |
        +-- Decode Compressed
        |
        +-- portnum != UNKNOWN_APP → plaintext path
        |
        +-- Extract: portnum=TEXT_MESSAGE_APP, payload="Hello World"
        |
        +-- startReceive() → process as decoded packet
```

### Example 2: PKI Encrypted Packet (Cannot Decrypt)

```
Device A (Simulator)
    |
    +-- Send TEXT_MESSAGE_APP packet (PKI encrypted to Device C)
        |
        +-- perhapsDecode() → DECODE_FAILED (not for us, no private key)
        |
        +-- Create Compressed:
            - portnum = UNKNOWN_APP (marker)
            - data = <encrypted bytes> (ciphertext)
        |
        +-- Wrap in SIMULATOR_APP and send to UI
    |
    v
Simulator UI
    |
    +-- Display: "UNKNOWN_APP: <encrypted data>" (shows as ciphertext)
    |
    +-- Forward to Device B (relay node in simulator)
        |
        +-- Wrap in Compressed (portnum=UNKNOWN_APP)
        |
        +-- Send as SIMULATOR_APP packet
    |
    v
Device B (Simulator - Relay)
    |
    +-- unpackAndReceive()
        |
        +-- Decode Compressed
        |
        +-- portnum == UNKNOWN_APP → ciphertext path
        |
        +-- Switch to encrypted variant:
            - p.which_payload_variant = encrypted_tag
            - p.encrypted.bytes = <ciphertext>
        |
        +-- startReceive() → process as encrypted packet
        |
        +-- Router relays encrypted packet (can't decrypt, just forwards)
    |
    v
Device C (Simulator - Destination)
    |
    +-- Receives encrypted packet
    |
    +-- perhapsDecode() → DECODE_SUCCESS (has matching private key)
    |
    +-- Create Compressed:
        - portnum = TEXT_MESSAGE_APP
        - data = "Hello World" (decrypted)
    |
    +-- Simulator UI displays plaintext message
```

### Example 3: PKI Encrypted Packet (Can Decrypt)

```
Device A (Sender)
    |
    +-- Send PKI-encrypted packet to Device A itself (testing)
        |
        +-- perhapsDecode() → DECODE_SUCCESS (we have our own private key)
        |
        +-- Create Compressed:
            - portnum = TEXT_MESSAGE_APP
            - data = "Hello World" (decrypted plaintext)
        |
        +-- Send to simulator UI as plaintext
    |
    v
Simulator UI
    |
    +-- Display: "TEXT_MESSAGE_APP: Hello World"
```

## Use Cases Enabled

### 1. Multi-Hop PKI Testing
Test PKI encryption across multiple simulator nodes:
- Node A encrypts for Node C
- Node B relays encrypted packet (cannot decrypt)
- Node C receives and decrypts

**Before:** Node B would fail to properly relay the encrypted packet.  
**After:** Node B correctly handles ciphertext and relays it.

### 2. PKI Key Exchange Testing
Test public key distribution:
- Node A has no public key for Node B
- Node A sends PKI-encrypted packet
- Node B responds with NAK (PKI_UNKNOWN_PUBKEY)
- Node B sends NodeInfo with public key
- Node A retries encryption

**Before:** Encrypted packets couldn't flow through simulator.  
**After:** Full PKI handshake works in simulator.

### 3. Mixed PSK/PKI Networks
Test networks with both encryption types:
- Some packets use PSK (channel encryption)
- Some packets use PKI (end-to-end encryption)
- Nodes relay both types correctly

### 4. Simulator UI Visualization
Simulator UI can now:
- Display plaintext for decryptable packets
- Display ciphertext indicator for encrypted packets
- Show encryption state in packet flow diagrams

## Technical Considerations

### Buffer Size Limits

Both plaintext and ciphertext must fit in `Compressed.data.bytes`:

```cpp
// Plaintext check
if (p->decoded.payload.size <= sizeof(c.data.bytes)) {
    memcpy(c.data.bytes, p->decoded.payload.bytes, p->decoded.payload.size);
    c.data.size = p->decoded.payload.size;
} else {
    LOG_WARN("Payload too large for Compressed wrapper (decoded). Sending empty.");
    c.data.size = 0;
}

// Ciphertext check
if (p->encrypted.size <= sizeof(c.data.bytes)) {
    memcpy(c.data.bytes, p->encrypted.bytes, p->encrypted.size);
    c.data.size = p->encrypted.size;
} else {
    LOG_WARN("Ciphertext too large for Compressed wrapper. Sending empty.");
    c.data.size = 0;
}
```

### Decode State Handling

The code checks both:
1. `DecodeState` result from `perhapsDecode()`
2. Final `which_payload_variant` tag

```cpp
if (st == DecodeState::DECODE_SUCCESS &&
    p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
    // Plaintext path
} else {
    // Ciphertext path
}
```

This ensures partial decode failures are caught.

### Channel Assignment

The code includes an optional placeholder for PKI channel assignment:

```cpp
// (optional) if PKI always uses channel 0 in your design:
// p.channel = 0;
```

This can be uncommented if PKI packets should always use channel 0.

## Compatibility

### Backward Compatibility

**Old Simulator UI with New SimRadio:**
- Old UI won't recognize `UNKNOWN_APP` marker
- Will display ciphertext as regular data (degraded but functional)

**New Simulator UI with Old SimRadio:**
- New UI won't receive ciphertext markers
- Can still display plaintext packets normally
- PKI packets will appear as empty/corrupted

### Forward Compatibility

The `UNKNOWN_APP=0` marker is a standard PortNum value, ensuring:
- No protobuf changes required
- Works with existing `Compressed` message definition
- Extensible for future payload types

## Testing Recommendations

### Unit Tests
1. **Plaintext send/receive:** Verify PSK-encrypted packets work as before
2. **Ciphertext send:** Verify undecryptable packets send with UNKNOWN_APP marker
3. **Ciphertext receive:** Verify UNKNOWN_APP packets reconstruct encrypted variant
4. **Buffer overflow:** Verify large payloads are handled safely

### Integration Tests
1. **Three-node PKI flow:** A→B→C with PKI encryption
2. **Mixed encryption:** PSK and PKI packets in same network
3. **Key exchange:** PKI_UNKNOWN_PUBKEY → NodeInfo → retry flow
4. **Relay node:** Node that cannot decrypt any PKI packets

### Simulator Tests
1. **UI display:** Verify UI shows plaintext vs ciphertext status
2. **Packet injection:** Inject encrypted packets from simulator UI
3. **Multi-hop:** Trace encrypted packet across multiple simulator nodes

## Logging

Relevant log messages:

```cpp
// Send path
LOG_WARN("Payload too large for Compressed wrapper (decoded). Sending empty.");
LOG_WARN("Ciphertext too large for Compressed wrapper. Sending empty.");

// Receive path
LOG_ERROR("Error decoding proto for simulator message!");
LOG_WARN("Ciphertext too large for MeshPacket.encrypted. Dropping.");
```

## Future Enhancements

### 1. Encryption Metadata
Add encryption type indicator to Compressed:

```protobuf
message Compressed {
    PortNum portnum = 1;
    bytes data = 2;
    
    enum EncryptionType {
        NONE = 0;
        PSK = 1;
        PKI = 2;
    }
    EncryptionType encryption = 3;  // NEW
}
```

### 2. Partial Decryption Info
For encrypted packets, include metadata:

```protobuf
message Compressed {
    // ... existing fields ...
    
    fixed32 intended_recipient = 4;  // PKI recipient node ID
    bytes sender_public_key = 5;     // Sender's public key
}
```

### 3. Simulator Decryption
Allow simulator UI to attempt decryption with user-provided keys:
- Import private keys into simulator
- Decrypt and display payloads in UI
- Useful for debugging and packet inspection

## Related Documentation

- **PKI Encryption Flow:** See `PSK_Encryption_Flow.md`
- **AODV Implementation:** See `AODV_Implementation.md`
- **SDN Implementation:** See `SDN_Implementation.md`
- **ACK Logic:** See `ACK_Logic.md`

## Summary

This enhancement enables full PKI encryption support in the Meshtastic simulator by:

1. ✅ Preserving ciphertext for packets that cannot be decrypted locally
2. ✅ Using `UNKNOWN_APP` marker to distinguish plaintext vs ciphertext
3. ✅ Enabling multi-hop encrypted packet flows in simulator
4. ✅ Maintaining backward compatibility with existing simulator infrastructure
5. ✅ Supporting mixed PSK/PKI network testing

The implementation is minimal, robust, and leverages existing protobuf messages without requiring protocol changes.
