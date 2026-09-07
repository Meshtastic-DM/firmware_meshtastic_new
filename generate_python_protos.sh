#!/bin/bash
# Script to generate Python protobuf files from .proto definitions

set -e

echo "Generating Python protobuf files for AODV..."

cd protobufs

# Check if we have protoc available
if command -v protoc &> /dev/null; then
    echo "Using system protoc..."
    protoc --python_out=. meshtastic/aodv.proto
elif command -v python3 &> /dev/null && python3 -c "import grpc_tools.protoc" 2>/dev/null; then
    echo "Using grpc_tools.protoc..."
    python3 -m grpc_tools.protoc -I. --python_out=. meshtastic/aodv.proto
else
    echo "Error: Neither 'protoc' nor 'grpc_tools.protoc' found."
    echo ""
    echo "Please install one of the following:"
    echo "  Option 1: Install protobuf compiler:"
    echo "    sudo apt install protobuf-compiler"
    echo ""
    echo "  Option 2: Install Python grpcio-tools:"
    echo "    pip install grpcio-tools"
    exit 1
fi

# Check if file was created
if [ -f "meshtastic/aodv_pb2.py" ]; then
    echo "✓ Successfully generated meshtastic/aodv_pb2.py"
    
    # Make the module importable
    touch meshtastic/__init__.py 2>/dev/null || true
    
    echo ""
    echo "You can now run the Python scripts:"
    echo "  python3 ../print_aodv_routes.py -p 4403"
    echo "  python3 ../print_aodv_routes_with_parser.py -p 4403"
else
    echo "✗ Failed to generate aodv_pb2.py"
    exit 1
fi
