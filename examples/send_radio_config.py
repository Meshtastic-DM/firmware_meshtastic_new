#!/usr/bin/env python3
"""
Example script to send radio configuration change commands to a Meshtastic node.
This demonstrates how to use the RadioConfigChangeModule.

Requirements:
    pip install meshtastic

Usage:
    python3 send_radio_config.py --command "RADIOCONFIG:PRESET:1"
"""

import argparse
import meshtastic
import meshtastic.serial_interface
from pubsub import pub


def on_receive(packet, interface):
    """Callback for received packets"""
    if 'decoded' in packet and 'text' in packet['decoded']:
        print(f"Received: {packet['decoded']['text']}")


def send_config_command(command, dest="^all"):
    """
    Send a radio config command to the mesh network
    
    Args:
        command: The command string (e.g., "RADIOCONFIG:PRESET:1")
        dest: Destination node (default: "^all" for broadcast)
    """
    print(f"Connecting to Meshtastic device...")
    
    # Connect to the device (will auto-detect USB or serial port)
    interface = meshtastic.serial_interface.SerialInterface()
    
    # Subscribe to received messages
    pub.subscribe(on_receive, "meshtastic.receive")
    
    print(f"Sending command: {command}")
    print(f"Destination: {dest}")
    
    # Send the message
    interface.sendText(command, destinationId=dest)
    
    print(f"\nCommand sent! Nodes receiving this message will:")
    print(f"  1. Parse the command")
    print(f"  2. Apply the configuration change")
    print(f"  3. Save to flash")
    print(f"  4. Reboot in 5 seconds")
    print(f"\nNote: You won't receive ACKs for broadcast messages.")
    
    # Wait a moment for any responses
    import time
    time.sleep(2)
    
    interface.close()
    print("\nDone!")


def main():
    parser = argparse.ArgumentParser(
        description="Send radio configuration commands to Meshtastic nodes",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Change all nodes to LONG_SLOW preset
  python3 send_radio_config.py --command "RADIOCONFIG:PRESET:1"
  
  # Set TX power to 20 dBm
  python3 send_radio_config.py --command "RADIOCONFIG:POWER:20"
  
  # Change to channel 10
  python3 send_radio_config.py --command "RADIOCONFIG:CHANNEL:10"
  
  # Set spreading factor to 11 (manual mode)
  python3 send_radio_config.py --command "RADIOCONFIG:SF:11"
  
  # Set bandwidth to 250 kHz (manual mode)
  python3 send_radio_config.py --command "RADIOCONFIG:BW:250"
  
  # Send to specific node (replace with actual node ID)
  python3 send_radio_config.py --command "RADIOCONFIG:PRESET:3" --dest "!a1b2c3d4"

Preset Numbers:
  0 = LONG_FAST
  1 = LONG_SLOW
  2 = VERY_LONG_SLOW (deprecated)
  3 = MEDIUM_SLOW
  4 = MEDIUM_FAST
  5 = SHORT_SLOW
  6 = SHORT_FAST
  7 = LONG_MODERATE
  8 = SHORT_TURBO
        """
    )
    
    parser.add_argument(
        "--command",
        required=True,
        help="Radio config command to send (e.g., 'RADIOCONFIG:PRESET:1')"
    )
    
    parser.add_argument(
        "--dest",
        default="^all",
        help="Destination node ID (default: ^all for broadcast)"
    )
    
    args = parser.parse_args()
    
    # Validate command format
    if not args.command.startswith("RADIOCONFIG:"):
        print("ERROR: Command must start with 'RADIOCONFIG:'")
        print("Example: RADIOCONFIG:PRESET:1")
        return 1
    
    send_config_command(args.command, args.dest)
    return 0


if __name__ == "__main__":
    exit(main())
