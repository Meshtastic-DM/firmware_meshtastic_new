# Error Fix Summary: Route Request Error

## The Error

When you ran your original Python script, you encountered this error:

```python
msg = aodv_pb2.AODV()
msg.route_request.CopyFrom(aodv_pb2.RouteRequest())  # ❌ WRONG
```

**Problems:**
1. **Missing `aodv_pb2.py` file** - The Python protobuf file didn't exist, causing import errors
2. **Wrong message type** - Used `RouteRequest` (for route discovery) instead of `RouteTableRequest` (for querying routing table)
3. **Wrong field name** - Used `route_request` instead of `rt_req`

## The Fix

### 1. Generated Python Protobuf Files
```bash
./generate_python_protos.sh
```
This creates `protobufs/meshtastic/aodv_pb2.py` from the `.proto` definition.

### 2. Updated Protobuf Definition
Added new messages to `aodv.proto`:
- `RouteTableRequest` - To request routing table
- `RouteTableResponse` - To send back routing table
- `RouteEntry` - Individual route information

### 3. Corrected Python Code
```python
# ✅ CORRECT
msg = aodv_pb2.AODV()
msg.rt_req.CopyFrom(aodv_pb2.RouteTableRequest())  # Use rt_req field
```

### 4. Added Firmware Support
The firmware now handles `RouteTableRequest` and responds with `RouteTableResponse` containing:
- Node ID
- Node sequence number
- List of active routes (up to 20)

## Quick Start

1. **Generate Python files:**
   ```bash
   ./generate_python_protos.sh
   ```

2. **Build firmware:**
   ```bash
   pio run -e native
   ```

3. **Run simulator nodes:**
   ```bash
   .pio/build/native/program -p 4403 &
   .pio/build/native/program -p 4404 &
   ```

4. **Query routing table:**
   ```bash
   python3 print_aodv_routes_with_parser.py -p 4403
   ```

## What Each Script Does

**print_aodv_routes.py**
- Basic script that sends `RouteTableRequest`
- Shows how to send the request
- Firmware logs the response

**print_aodv_routes_with_parser.py**
- Advanced script with response parsing
- Captures and displays the `RouteTableResponse`
- Formats route table in a readable table
- Use this for automated route table querying

## Message Flow

```
Python Client                    Firmware Node
     |                                |
     |------ RouteTableRequest ------>|
     |      (AODV_ROUTING_APP)        |
     |                                |
     |                        [Collect routes]
     |                                |
     |<----- RouteTableResponse ------|
     |      (node_id, seq_num,        |
     |       routes[])                |
     |                                |
   [Parse and display]
```

## Testing

After making some test transmissions to create routes:

```bash
# Query single node
python3 print_aodv_routes_with_parser.py -p 4403

# Query all nodes in simulator
python3 print_aodv_routes_with_parser.py -a -n 5
```

You should see output like:
```
======================================================================
ROUTING TABLE FOR NODE 0x12345678
======================================================================
Node Sequence Number: 42
Number of Routes: 2
----------------------------------------------------------------------
Destination        Next Hop        Hops   Seq Num    Expiry (s)
----------------------------------------------------------------------
0x87654321         0x34           2      15         285
0x11223344         0x56           1      8          142
======================================================================
```
