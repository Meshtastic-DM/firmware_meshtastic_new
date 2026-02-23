# AODV Route Table Query

This implementation allows you to query the AODV routing table from Meshtastic nodes via TCP (simulator) or serial interface.

## Changes Made

### 1. Protobuf Definitions (protobufs/meshtastic/aodv.proto)
Added new message types:
- `RouteTableRequest` - Request to get a node's routing table
- `RouteEntry` - Single route entry with destination, next hop, hop count, sequence number, and expiry
- `RouteTableResponse` - Response containing node ID, sequence number, and up to 20 routes

### 2. Firmware Implementation

**AODVModule.cpp:**
- Added `handleRouteTableRequest()` - Processes incoming route table queries
- Added `handleRouteTableResponse()` - Logs received route table data
- Added `sendRouteTableResponse()` - Sends routing table to requester

**AODVRouteTable.h:**
- Added `getAllRoutes()` - Returns const reference to all routes for querying

### 3. Python Scripts

**print_aodv_routes.py** - Basic script that sends route table request
**print_aodv_routes_with_parser.py** - Advanced script with response parsing and formatted output

## Setup Instructions

### Step 1: Generate Python Protobuf Files

```bash
./generate_python_protos.sh
```

Or manually:
```bash
cd protobufs
python3 -m grpc_tools.protoc -I. --python_out=. meshtastic/aodv.proto
```

If you don't have grpcio-tools:
```bash
pip install grpcio-tools
```

### Step 2: Install Python Dependencies

```bash
pip install meshtastic pypubsub
```

### Step 3: Build and Run Firmware

```bash
# Build the firmware
pio run -e native

# Run the simulator (in separate terminals)
.pio/build/native/program -p 4403  # Node 1
.pio/build/native/program -p 4404  # Node 2
.pio/build/native/program -p 4405  # Node 3
```

### Step 4: Query Route Tables

Query single node:
```bash
python3 print_aodv_routes_with_parser.py -p 4403
```

Query all nodes:
```bash
python3 print_aodv_routes_with_parser.py -a -n 5
```

## Usage Examples

### Query Node on Port 4403
```bash
./print_aodv_routes_with_parser.py -p 4403
```

Expected output:
```
======================================================================
ROUTING TABLE FOR NODE 0x12345678
======================================================================
Node Sequence Number: 42
Number of Routes: 3
----------------------------------------------------------------------
Destination        Next Hop        Hops   Seq Num    Expiry (s)
----------------------------------------------------------------------
0x87654321         0x34           2      15         285
0x11223344         0x56           1      8          142
0xaabbccdd         0x78           3      23         198
======================================================================
```

### Query All Simulator Nodes
```bash
./print_aodv_routes_with_parser.py -a -n 5
```

## Protocol Flow

1. **Client → Node**: Send `RouteTableRequest` via AODV_ROUTING_APP port (75)
2. **Node**: Receives request, gathers routing table entries
3. **Node → Client**: Send `RouteTableResponse` with up to 20 route entries
4. **Client**: Parse and display routing table

## Message Format

### RouteTableRequest
```protobuf
message RouteTableRequest {
  uint32 reserved = 1;  // For future use
}
```

### RouteTableResponse
```protobuf
message RouteTableResponse {
  uint32 node_id = 1;              // Responding node ID
  repeated RouteEntry routes = 2;  // Up to 20 routes
  uint32 node_seq_num = 3;         // Node's sequence number
}
```

### RouteEntry
```protobuf
message RouteEntry {
  uint32 destination = 1;      // Destination node
  uint32 next_hop = 2;         // Next hop (8-bit)
  uint32 hop_count = 3;        // Number of hops
  uint32 dest_seq_num = 4;     // Destination sequence number
  uint32 expiry_seconds = 5;   // Seconds until route expires
}
```

## Serial Port Usage

To use with a real device via serial:

```python
from meshtastic.serial_interface import SerialInterface

iface = SerialInterface(devPath="/dev/ttyUSB0")  # Adjust port

# Send route table request
msg = aodv_pb2.AODV()
msg.rt_req.CopyFrom(aodv_pb2.RouteTableRequest())
payload = msg.SerializeToString()

iface.sendData(
    data=payload,
    destinationId=iface.myInfo.my_node_num,
    portNum=75,
    wantAck=False
)
```

## Troubleshooting

### "aodv_pb2 not found"
Run `./generate_python_protos.sh` to generate Python protobuf files.

### "No response received"
- Ensure the AODV module is enabled in firmware
- Check that the node has active routes (send some messages first)
- Increase timeout: `-t 10`

### "Connection refused"
- Make sure the simulator is running on the specified port
- Check port number with `-p` option

### Routes are empty
- AODV routes are created on-demand
- Send messages between nodes to create routes first
- Routes expire after 5 minutes of inactivity

## Notes

- Maximum 20 routes per response (due to protobuf size constraints)
- Routes expire after 5 minutes (AODV_ACTIVE_ROUTE_TIMEOUT)
- Only valid, non-expired routes are included in responses
- Route table queries work over both TCP (simulator) and Serial (real devices)
