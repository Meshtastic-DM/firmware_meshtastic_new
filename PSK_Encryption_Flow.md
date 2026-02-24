# Meshtastic PSK Encryption Flow

## Overview

Meshtastic uses **Pre-Shared Key (PSK)** encryption with AES-CTR mode to secure mesh network communications. Each channel has its own PSK, and packets are encrypted before transmission and decrypted upon reception.

---

## Channel and PSK Basics

### Channel Structure
- Meshtastic supports **multiple channels** (up to `MAX_NUM_CHANNELS`)
- Each channel has:
  - **PSK**: Pre-Shared Key (variable length: 0, 1, 16, or 32 bytes)
  - **Channel Hash**: 8-bit hash of the PSK used as channel identifier in packets
  - **Channel Index**: 0-based index for local channel management
  - **Role**: PRIMARY, SECONDARY, or DISABLED

### PSK Types

#### 1. **Disabled Encryption** (PSK length = 0)
```cpp
k.length = 0; // No encryption
```
- Packets sent in plaintext
- Used for debugging or public channels

#### 2. **Short PSK** (PSK length = 1)
```cpp
// Single byte PSK index (1-255)
pskIndex = k.bytes[0];
```
**Expansion Process**:
- If `pskIndex == 0` → No encryption
- If `pskIndex > 0`:
  ```cpp
  memcpy(k.bytes, defaultpsk, sizeof(defaultpsk)); // Start with default PSK
  k.bytes[15] += (pskIndex - 1); // Modify last byte based on index
  k.length = 16; // Result is AES-128 key
  ```
- **Purpose**: Simplified key sharing using a single byte (common preset keys)

#### 3. **AES-128** (PSK length = 16 bytes)
```cpp
k.length = 16; // Full 128-bit key
```
- Standard AES-128 encryption
- Most common mode

#### 4. **AES-256** (PSK length = 32 bytes)
```cpp
k.length = 32; // Full 256-bit key
```
- Stronger encryption for sensitive networks

#### 5. **Short Keys** (PSK length < 16 or < 32)
```cpp
// Auto-padded with zeros to 16 or 32 bytes
LOG_WARN("User provided a too short key - padding");
```

---

## Packet Encryption Flow (Sending)

### Step 1: Packet Creation
User creates a packet with:
```cpp
meshtastic_MeshPacket *p = router->allocForSending();
p->to = destination;
p->channel = channelIndex; // 0-7 (local channel index)
p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
// ... payload data
```

At this point:
- `which_payload_variant = meshtastic_MeshPacket_decoded_tag`
- Packet contains plaintext data in `p->decoded`

### Step 2: Routing Layer (`Router::send()`)

#### 2a. Check if Already Encrypted
```cpp
if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
    // Already encrypted, skip to transmission
    return iface->send(p);
}
```

#### 2b. Encode to Protobuf
```cpp
size_t numbytes = pb_encode_to_bytes(bytes, sizeof(bytes), 
                                      &meshtastic_Data_msg, &p->decoded);
```
Converts `p->decoded` (Data protobuf) to raw bytes for encryption.

#### 2c. Determine Encryption Method

##### **Option A: PKI Encryption** (Public Key Infrastructure)
Used when **ALL** conditions are met:
- Packet is from local node (`isFromUs`)
- Destination is a single node (not broadcast: `!isBroadcast`)
- Both sender and receiver have public keys (32 bytes each)
- Specific portnums (not TRACEROUTE, NODEINFO, ROUTING, POSITION)
- Not Ham radio mode (`!owner.is_licensed`)
- PKI not explicitly disabled

**If ANY condition fails → Falls back to PSK encryption**

```cpp
crypto->encryptCurve25519(p->to, getFrom(p), 
                          node->user.public_key, 
                          p->id, numbytes, bytes, p->encrypted.bytes);
p->channel = 0; // PKI uses channel 0
p->pki_encrypted = true;
```

**Result**: End-to-end encrypted with Curve25519, only recipient can decrypt.

**DM Encryption Decision Tree**:
```
Sending DM to node X:
├─ Do I have node X's public key? 
│  ├─ YES: Is it 32 bytes valid?
│  │  ├─ YES: Is my private key 32 bytes?
│  │  │  ├─ YES: Is this a text message (or allowed portnum)?
│  │  │  │  ├─ YES: Am I NOT a Ham operator?
│  │  │  │  │  ├─ YES: ✅ Use PKI encryption (channel=0)
│  │  │  │  │  └─ NO:  ⬇️ Fall through to PSK
│  │  │  │  └─ NO:  ⬇️ Fall through to PSK
│  │  │  └─ NO:  ⬇️ Fall through to PSK
│  │  └─ NO:  ⬇️ Fall through to PSK
│  └─ NO:  ⬇️ Fall through to PSK
└─ ✅ Use PSK encryption (channel=hash)
```

**Key Point**: **DMs don't require PKI** - they can use regular PSK encryption. PKI is an enhancement when available.

##### **Option B: PSK Encryption** (Standard Channel Encryption)
```cpp
// Step 1: Set active channel and get hash
hash = channels.setActiveByIndex(chIndex);

// Step 2: Update packet channel to hash
p->channel = hash; // Now contains 8-bit hash, not index!

// Step 3: Encrypt with channel PSK
crypto->encryptPacket(getFrom(p), p->id, numbytes, bytes);
memcpy(p->encrypted.bytes, bytes, numbytes);
```

**Detailed PSK Encryption Process**:

1. **`channels.setActiveByIndex(chIndex)`**:
   ```cpp
   int16_t Channels::setActiveByIndex(ChannelIndex channelIndex) {
       return setCrypto(channelIndex);
   }
   ```

2. **`channels.setCrypto(chIndex)`**:
   ```cpp
   CryptoKey k = getKey(chIndex);
   crypto->setKey(k); // Load PSK into crypto engine
   return getHash(chIndex); // Return 8-bit channel hash
   ```

3. **`crypto->encryptPacket(fromNode, packetId, numBytes, bytes)`**:
   ```cpp
   void CryptoEngine::encryptPacket(uint32_t fromNode, uint64_t packetId, 
                                     size_t numBytes, uint8_t *bytes) {
       if (key.length > 0) {
           initNonce(fromNode, packetId); // Create nonce from sender+ID
           encryptAESCtr(key, nonce, numBytes, bytes); // AES-CTR encryption
       }
   }
   ```

**Nonce Construction**:
```cpp
void CryptoEngine::initNonce(uint32_t fromNode, uint64_t packetId) {
    // Nonce = [packetId (8 bytes) | fromNode (4 bytes) | pad (4 bytes)]
    memset(nonce, 0, sizeof(nonce));
    memcpy(nonce, &packetId, sizeof(packetId));
    memcpy(nonce + sizeof(packetId), &fromNode, sizeof(fromNode));
}
```

**Why CTR Mode?**:
- CTR (Counter) mode turns block cipher into stream cipher
- Same key+nonce for encrypt and decrypt
- No padding needed
- Unique nonce per packet prevents replay attacks

### Step 3: Update Packet Variant
```cpp
p->encrypted.size = numbytes;
p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
```

Now packet contains:
- `p->encrypted.bytes`: Encrypted data
- `p->channel`: 8-bit hash (not index!)
- `p->which_payload_variant`: `encrypted_tag`

### Step 4: Transmission
```cpp
iface->send(p);
```

Packet is transmitted over LoRa with:
- **Header**: Contains `p->channel` (hash), `p->from`, `p->to`, `hop_limit`, etc.
- **Payload**: Encrypted bytes

---

## Packet Decryption Flow (Receiving)

### Step 1: Packet Reception
Radio receives encrypted packet:
```cpp
RadioInterface::deliverToReceiver(meshtastic_MeshPacket *p);
```

Packet state:
- `which_payload_variant = meshtastic_MeshPacket_encrypted_tag`
- `p->channel` contains 8-bit hash
- `p->encrypted.bytes` contains encrypted data

### Step 2: Decryption Attempt (`perhapsDecode()`)

#### 2a. Check if Already Decoded
```cpp
if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag)
    return DECODE_SUCCESS; // Already decrypted
```

#### 2b. Try PKI Decryption First
```cpp
if (p->channel == 0 && isToUs(p) && ... has public keys ...) {
    if (crypto->decryptCurve25519(...)) {
        // PKI decryption successful
        p->decoded = decodedtmp;
        p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        return DECODE_SUCCESS;
    }
}
```

#### 2c. Try PSK Decryption (All Channels)
```cpp
for (chIndex = 0; chIndex < channels.getNumChannels(); chIndex++) {
    // Try to use this hash/channel pair
    if (channels.decryptForHash(chIndex, p->channel)) {
        // Copy encrypted bytes to scratch buffer
        memcpy(bytes, p->encrypted.bytes, rawSize);
        
        // Decrypt with channel PSK
        crypto->decrypt(p->from, p->id, rawSize, bytes);
        
        // Try to decode as protobuf
        if (pb_decode_from_bytes(bytes, rawSize, &meshtastic_Data_msg, &decodedtmp)) {
            p->decoded = decodedtmp;
            p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
            decrypted = true;
            break;
        }
    }
}
```

**`channels.decryptForHash(chIndex, channelHash)`**:
```cpp
bool Channels::decryptForHash(ChannelIndex chIndex, ChannelHash channelHash) {
    if (getHash(chIndex) != channelHash) {
        return false; // Hash doesn't match, skip this channel
    }
    
    LOG_DEBUG("Use channel %d (hash 0x%x)", chIndex, channelHash);
    setCrypto(chIndex); // Load PSK into crypto engine
    return true;
}
```

**`crypto->decrypt()`**:
```cpp
void CryptoEngine::decrypt(uint32_t fromNode, uint64_t packetId, 
                           size_t numBytes, uint8_t *bytes) {
    // CTR mode: decrypt is same as encrypt!
    encryptPacket(fromNode, packetId, numBytes, bytes);
}
```

#### 2d. Validation
After decryption attempt:
```cpp
// Decode protobuf
if (!pb_decode_from_bytes(bytes, rawSize, &meshtastic_Data_msg, &decodedtmp)) {
    LOG_ERROR("Invalid protobufs (bad psk?)!");
    continue; // Try next channel
}

if (decodedtmp.portnum == meshtastic_PortNum_UNKNOWN_APP) {
    LOG_ERROR("Invalid portnum (bad psk?)!");
    continue;
}

// Success!
p->decoded = decodedtmp;
p->channel = chIndex; // Store actual channel index (not hash!)
```

### Step 3: Handle Decrypted Packet
```cpp
if (decrypted) {
    printPacket("decoded message", p);
    MeshModule::callModules(*p); // Deliver to application modules
    return DECODE_SUCCESS;
} else {
    LOG_WARN("No suitable channel found for decoding, hash was 0x%x!", p->channel);
    return DECODE_FAILURE;
}
```

---

## Channel Hash Computation

### Hash Algorithm
```cpp
ChannelHash Channels::getHash(ChannelIndex i) {
    const meshtastic_Channel &ch = getByIndex(i);
    const char *name = getName(i);
    
    if (!ch.has_settings || ch.role == meshtastic_Channel_Role_DISABLED) {
        return 0; // Disabled channel
    }
    
    // XOR hash of channel name + PSK
    CryptoKey k = getKey(i);
    ChannelHash hash = crc8(k.bytes, k.length); // 8-bit CRC
    
    // XOR with name hash
    hash ^= crc8((uint8_t *)name, strlen(name));
    
    return hash;
}
```

**Properties**:
- 8-bit hash (0-255)
- Deterministic: Same PSK+name → Same hash
- Collision possible but rare
- Allows sender/receiver to verify they're using same channel

---

## Security Considerations

### 1. **PSK Distribution**
- PSKs must be shared **out-of-band** (QR codes, manual entry, etc.)
- Short PSK (1 byte) provides convenience but limited security
- Full 16/32 byte PSKs provide strong security

### 2. **Nonce Uniqueness**
- Nonce = `fromNode` + `packetId`
- `packetId` is unique per packet (random + counter)
- Ensures same message encrypted twice produces different ciphertext

### 3. **Channel Privacy**
- Receivers try **all channels** to decrypt
- Anyone with the PSK can decrypt
- No forward secrecy (captured traffic can be decrypted if PSK leaked later)

### 4. **PKI vs PSK**
- **PKI**: End-to-end encryption, only recipient can decrypt
- **PSK**: All channel members can decrypt all traffic
- PKI preferred for DMs, PSK for group channels

### 5. **Hash Collisions**
- 8-bit hash → 256 possible values
- Collision rate ~1.5% for 8 random channels
- Receivers try all matching channels (performance impact)

---

## Example Flow

### Sending a Message

**User**: Sends "Hello" on Channel 0
```
1. Create packet:
   - to = 0xffffffff (broadcast)
   - channel = 0 (index)
   - decoded.payload = "Hello"
   - which_payload_variant = decoded_tag

2. Router::send():
   - Encode "Hello" to protobuf bytes
   - channels.setActiveByIndex(0) → hash = 0x8a
   - crypto->setKey(channel0_PSK)
   - crypto->encryptPacket(myNode, packetId, bytes)
     - nonce = [packetId | myNode | 00 00 00 00]
     - AES-CTR encrypt with channel0_PSK + nonce
   - p->channel = 0x8a (hash)
   - p->encrypted.bytes = [encrypted data]
   - which_payload_variant = encrypted_tag

3. iface->send(p):
   - Transmit over LoRa with channel=0x8a
```

### Receiving the Message

**Receiver**: Receives packet with channel=0x8a
```
1. RadioInterface::deliverToReceiver():
   - p->channel = 0x8a
   - p->encrypted.bytes = [encrypted data]

2. perhapsDecode():
   - Try channel 0: getHash(0) = 0x8a ✓ Match!
   - crypto->setKey(channel0_PSK)
   - crypto->decrypt(fromNode, packetId, bytes)
     - nonce = [packetId | fromNode | 00 00 00 00]
     - AES-CTR decrypt (same as encrypt!)
   - pb_decode_from_bytes() → Success!
   - p->decoded.payload = "Hello"
   - p->channel = 0 (now index, not hash)
   - which_payload_variant = decoded_tag

3. MeshModule::callModules():
   - Deliver "Hello" to text message handler
```

---

## Key Takeaways

1. **Channel field changes meaning**:
   - **Before encryption**: Channel index (0-7)
   - **After encryption**: Channel hash (0-255)
   - **After decryption**: Channel index (0-7)

2. **Encryption is symmetric**:
   - Same key, same nonce → encrypt/decrypt are identical (CTR mode)

3. **Receiver tries all channels**:
   - No way to know which channel without trying
   - Hash collision → try multiple PSKs

4. **Nonce prevents replay**:
   - `fromNode` + `packetId` ensures unique nonce
   - Same packet re-encrypted produces different ciphertext

5. **Two encryption paths**:
   - **PSK**: Shared secret, all channel members decrypt
   - **PKI**: Public key, only recipient decrypts

---

## Curve25519 Public Key Distribution (PKI)

### Overview
Meshtastic uses **Curve25519** elliptic curve cryptography for end-to-end encrypted direct messages. Unlike PSK which is shared among all channel members, PKI encryption ensures only the intended recipient can decrypt the message.

### Key Generation

#### Initial Setup
When a device first boots (or when keys are regenerated):

```cpp
void CryptoEngine::generateKeyPair(uint8_t *pubKey, uint8_t *privKey) {
    // 1. Initialize RNG with entropy sources
    CryptRNG.begin(APP_VERSION);
    CryptRNG.stir(device_id, 16);       // Mix in device ID
    CryptRNG.stir(random_noise, 4);     // Mix in random hardware noise
    
    // 2. Generate Curve25519 key pair
    Curve25519::dh1(public_key, private_key);
    
    // 3. Store in config
    config.security.public_key.bytes = public_key;  // 32 bytes
    config.security.private_key.bytes = private_key; // 32 bytes
}
```

**Key Storage**:
- **Private Key**: Stored in `config.security.private_key` (32 bytes), never transmitted
- **Public Key**: Stored in `config.security.public_key` (32 bytes), broadcast to mesh
- Both keys persist in flash memory across reboots

### Public Key Distribution

#### Method: NodeInfo Broadcast

Public keys are distributed automatically via **NodeInfo packets** (PortNum=4), which are **separate from regular messages**:

**Important**: NodeInfo broadcasts are NOT the same as regular broadcast messages. They are periodic system packets that share node metadata.

```cpp
// NodeInfoModule broadcasts user information periodically
void NodeInfoModule::sendOurNodeInfo(NodeNum dest, bool wantReplies) {
    meshtastic_User &u = owner;
    
    // Strip public key if user is licensed (Ham radio)
    if (u.is_licensed && u.public_key.size > 0) {
        u.public_key.bytes[0] = 0;
        u.public_key.size = 0;  // Ham operators don't use PKI
    }
    
    // NodeInfo contains:
    // - u.id (node ID)
    // - u.long_name
    // - u.short_name
    // - u.public_key (32 bytes) ← Distributed here
    // - u.role
    
    // This is sent as a broadcast to all nodes (to=0xffffffff)
    // But it's PortNum=4 (NODEINFO), not a regular text message
    service->sendToMesh(allocDataProtobuf(u));
}
```

**Broadcast Triggers**:
1. **Periodic**: Every `node_info_broadcast_secs` (default: 900s = 15 minutes)
2. **On Startup**: 30 seconds after boot
3. **Channel Change**: When radio generation changes
4. **Manual Request**: When another node requests user info

**Protobuf Structure** (`User` message):
```protobuf
message User {
    string id = 1;              // "!f0000010"
    string long_name = 2;       // "Alice's Node"
    string short_name = 3;      // "AL"
    bytes public_key = 8;       // 32-byte Curve25519 public key
    bool is_licensed = 6;       // If true, public_key is stripped
    Config.DeviceConfig.Role role = 7;
}
```

#### Reception and Storage

When a node receives a NodeInfo packet:

```cpp
bool NodeInfoModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, 
                                            meshtastic_User *pptr) {
    auto p = *pptr;
    
    // Update NodeDB with received user info (including public key)
    bool hasChanged = nodeDB->updateUser(getFrom(&mp), p, mp.channel);
    
    // NodeDB stores:
    // - meshtastic_NodeInfoLite.user.public_key (32 bytes)
    // - Indexed by node number
}
```

**Storage in NodeDB**:
```cpp
meshtastic_NodeInfoLite {
    uint32 num;                    // Node number
    meshtastic_UserLite user {
        string id;
        string long_name;
        string short_name;
        bytes public_key;          // ← Stored for PKI encryption
        // ...
    };
    // ...
}
```

### PKI Encryption Process (Using Distributed Keys)

When sending an encrypted DM:

```cpp
// 1. Check if PKI should be used
if (isFromUs(p) && 
    !isBroadcast(p->to) && 
    config.security.private_key.size == 32 &&
    node->user.public_key.size == 32) {  // ← Public key from NodeDB
    
    // 2. Perform Diffie-Hellman key exchange
    crypto->encryptCurve25519(
        p->to,                           // Recipient node number
        getFrom(p),                      // Our node number
        node->user.public_key,           // Recipient's public key (from NodeDB)
        p->id,                           // Packet ID (for nonce)
        numBytes,                        // Plaintext length
        bytes,                           // Plaintext
        p->encrypted.bytes               // Ciphertext output
    );
    
    p->channel = 0;        // PKI uses channel 0
    p->pki_encrypted = true;
}
```

**Shared Secret Derivation**:
```cpp
void CryptoEngine::encryptCurve25519(..., remotePublic, ...) {
    // 1. Compute shared secret using ECDH
    uint8_t shared_secret[32];
    Curve25519::dh2(remotePublic, private_key, shared_secret);
    
    // 2. Derive encryption key using SHA256
    SHA256 hash;
    hash.update(shared_secret, 32);
    hash.update(&packetId, 8);
    uint8_t derived_key[32];
    hash.finalize(derived_key, 32);
    
    // 3. Encrypt with derived key
    CryptoKey k = {derived_key, 32};
    encryptAESCtr(k, nonce, numBytes, bytes);
}
```

### PKI Decryption Process

When receiving a PKI-encrypted packet:

```cpp
// 1. Detect PKI packet (channel == 0 and isToUs)
if (p->channel == 0 && isToUs(p) && 
    nodeDB->getMeshNode(p->from)->user.public_key.size > 0) {
    
    // 2. Decrypt using sender's public key
    crypto->decryptCurve25519(
        p->from,                                    // Sender node number
        nodeDB->getMeshNode(p->from)->user.public_key,  // Sender's public key
        p->id,                                      // Packet ID
        rawSize,                                    // Ciphertext length
        p->encrypted.bytes,                         // Ciphertext
        bytes                                       // Plaintext output
    );
    
    // Decryption computes same shared secret using ECDH
    // (our private key + sender's public key)
}
```

### Key Characteristics

**Security Properties**:
- **Forward Secrecy**: No (same key pair used for all messages)
- **Perfect Forward Secrecy**: No (compromise of private key decrypts all past messages)
- **End-to-End Encryption**: Yes (only sender and recipient can decrypt)
- **Key Length**: 256 bits (Curve25519)

**Advantages over PSK**:
- **Privacy**: Only recipient can decrypt (not all channel members)
- **No Pre-Sharing**: Keys distributed automatically via mesh
- **Per-Node**: Each DM conversation uses different shared secret

**When PKI is NOT Used (Falls back to PSK)**:
- **Broadcast messages**: PKI only works for unicast
- **Missing public key**: Recipient hasn't broadcast NodeInfo yet
- **Ham Radio mode**: Licensed operators can't use encryption
- **System packets**: NODEINFO, ROUTING, POSITION, TRACEROUTE always use PSK
- **Incomplete keys**: Either sender or receiver missing valid 32-byte keys
- **Legacy DMs**: Can still use PSK for backward compatibility

**Common Scenarios**:

| Scenario | Encryption Used | Reason |
|----------|----------------|---------|
| DM with public key available | PKI (channel=0) | Preferred for privacy |
| DM without public key | PSK (channel=hash) | Public key not yet received |
| DM from Ham operator | PSK (channel=hash) | Ham radio restrictions |
| Broadcast message | PSK (channel=hash) | PKI doesn't support broadcast |
| NODEINFO packet | PSK (channel=hash) | System packet exclusion |
| Group chat | PSK (channel=hash) | Multiple recipients |

**Key Point**: **DMs work with or without PKI** - PSK is always available as fallback.

**Limitations**:
- **Not for Broadcasts**: PKI only works for unicast (DMs)
- **Requires NodeInfo**: Must receive public key before sending encrypted DM
- **Ham Radio Excluded**: Licensed operators don't broadcast public keys (regulatory)
- **Storage**: Requires storing 32 bytes per node in NodeDB
- **Performance**: ECDH computation slower than PSK

### Example: Full PKI Flow

#### Scenario: Alice (0x10) sends encrypted DM to Bob (0x20)

**Step 1: Bob Broadcasts Public Key**
```
Bob's NodeInfoModule:
- Generates key pair: (Bob_private, Bob_public)
- Broadcasts NodeInfo with Bob_public every 15 minutes
- Alice receives and stores Bob_public in NodeDB
```

**Step 2: Alice Sends Encrypted DM**
```
1. Alice creates message: "Hello Bob"
2. Router::send() detects:
   - to = 0x20 (Bob, unicast)
   - Bob's public_key exists in NodeDB
3. Compute shared secret:
   - Alice_private + Bob_public → shared_secret_AB
4. Derive key:
   - SHA256(shared_secret_AB || packetId) → derived_key
5. Encrypt:
   - AES-CTR(derived_key, nonce, "Hello Bob") → ciphertext
6. Transmit:
   - channel = 0 (indicates PKI)
   - pki_encrypted = true
```

**Step 3: Bob Receives and Decrypts**
```
1. Bob receives packet:
   - channel = 0 (PKI indicator)
   - from = 0x10 (Alice)
2. Look up Alice's public key in NodeDB
3. Compute shared secret:
   - Bob_private + Alice_public → shared_secret_AB
   - (Same secret as Alice computed!)
4. Derive key:
   - SHA256(shared_secret_AB || packetId) → derived_key
5. Decrypt:
   - AES-CTR(derived_key, nonce, ciphertext) → "Hello Bob"
```

### Public Key Lifecycle

**Key Generation**:
- Boot time (if no keys exist)
- Admin command: `SET_OWNER` with new keys
- User request via menu/app

**Key Rotation**:
- Not automatic
- Manual regeneration via admin module
- Requires new NodeInfo broadcast

**Key Revocation**:
- No formal revocation mechanism
- User can clear public key (sets size=0)
- Other nodes retain old key until new NodeInfo received

**Key Persistence**:
- Stored in `config.security` (flash)
- Survives reboots and firmware updates
- Only cleared by factory reset or explicit command

### Security Considerations

**Weak Point Detection**:
```cpp
// Check for low-entropy keys (security risk)
bool checkLowEntropyPublicKey(bytes public_key) {
    // Detect keys with too many zeros or predictable patterns
    // Triggered by poor RNG or compromised key generation
}
```

**Ham Radio Compliance**:
- Licensed operators (`is_licensed = true`) must NOT use PKI
- Public keys stripped from NodeInfo broadcasts
- Ensures regulatory compliance (no encryption for ham bands)

**Trust Model**:
- **No Authentication**: Public keys not signed/verified
- **Trust on First Use (TOFU)**: First received public key is trusted
- **No PKI Infrastructure**: No certificate authority or web of trust
- **Spoofing Risk**: Attacker could broadcast fake public key for a node

### Key Takeaways

1. **Public keys distributed via NodeInfo broadcasts** every 15 minutes
2. **Automatic**: No manual key exchange needed
3. **Stored in NodeDB**: Each node maintains table of other nodes' public keys
4. **ECDH shared secret**: Computed independently by sender and receiver
5. **No forward secrecy**: Same key pair used for all messages
6. **Ham radio excluded**: Licensed operators don't use PKI (regulatory)
7. **Trust on first use**: No authentication of public keys
