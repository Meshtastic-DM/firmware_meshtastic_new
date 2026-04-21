# Virtual Node (Digital Twin) Bridge – Implementation Notes

## Goal

Implement one **virtual simulator node** as a **digital twin** of one **physical serial-connected node**, so simulated and physical meshes can exchange packets bidirectionally.

- Physical -> Simulated nodes
- Simulated mirror node -> Physical node
- Supports direct and broadcast traffic
- Prevents packet loops

---

## What was implemented

## 1) Simulator-side (Python)

### Files
- `interactive.py`
- `interactiveSim.py`

### Added CLI options
- `--serial /dev/ttyUSB0`
- `--mirror-node <id>`
- (already present) `--region`, `--modem-preset`

### Bridge logic
- Open serial interface with `serial_interface.SerialInterface`.
- Bind one virtual node (`mirror-node`) as the twin of the physical node.
- Subscribe to packet events and forward in both directions:
  - **Serial RX -> simulator air injection**
  - **Mirror node TX -> serial ToRadio forwarding**

### Loop protection
- Packet-ID cache with TTL:
  - `_seen_from_serial`
  - `_seen_to_serial`
- Prevents re-forwarding same packet back to origin.

### Payload handling improvements
- Handle both packet forms:
  - `decoded` payloads
  - `encrypted` payloads (ciphertext path)
- Do not assume `packet["decoded"]` always exists.

### Minor fixes
- `interactiveSim.py`: fixed scripted call typo (`showNodes()`).
- Added mirror-node argument validation range checks.
- Close serial interface cleanly in shutdown path.

---

## 2) Firmware-side (C++)

### Files
- `src/mesh/MeshService.cpp`
- `src/mesh/PhoneAPI.cpp`

### `MeshService.cpp`
In `MeshService::handleToRadio(meshtastic_MeshPacket &p)` for Portduino:
- Accept both:
  - `decoded.portnum == SIMULATOR_APP`
  - `which_payload_variant == encrypted`
- Route both through:
  - `SimRadio::instance->unpackAndReceive(p);`
- Return after injection to avoid wrong TX path.

### `PhoneAPI.cpp`
- Guard `decoded.portnum` usage with payload variant checks.
- Only access decoded-specific fields when:
  - `p.which_payload_variant == meshtastic_MeshPacket_decoded_tag`
- Avoid decoded-only assumptions for encrypted injected packets.

---

## Protobuf changes

**No protobuf schema changes are required** for this feature.

If `protobufs` submodule shows `-dirty`, that only indicates local edits; this bridge feature does not need new `.proto` fields.

---

## Run command

```bash
python3 interactiveSim.py 5 \
  -p /home/raveen/firmware_meshtastic_new \
  --serial /dev/ttyUSB0 \
  --mirror-node 0 \
  --region IN \
  --modem-preset SHORT_TURBO \
  -c
```

---

## Expected behavior

- A packet from physical node appears in simulator as if sent by mirror node.
- Packets from mirror virtual node are sent out through the physical serial node.
- No infinite bounce loop due to packet-id suppression.

---

## Quick verification

```bash
# Firmware build
pio run -e tbeam

# Confirm key hooks exist
git grep -n "SIMULATOR_APP" src/mesh
git grep -n "meshtastic_MeshPacket_encrypted_tag" src/mesh
git grep -n "which_payload_variant" src/mesh/PhoneAPI.cpp
```

---

## Known issue source (if Python cannot decode packets)

Most common reason: **meshtastic Python package version mismatch** vs firmware/protobuf revision.

Also possible:
- expecting `decoded` when packet is actually `encrypted`
- channel/PSK mismatch causing undecryptable payload
- simulator path not handling ciphertext branch