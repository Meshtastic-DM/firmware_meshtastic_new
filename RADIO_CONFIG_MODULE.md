# Radio Configuration Change Module

## Overview

This module allows Meshtastic nodes to change their radio configuration dynamically by receiving specially formatted text messages over the mesh network. This is useful for coordinating configuration changes across multiple nodes or switching communication parameters in response to network conditions.

## Features

- Change radio settings via text messages
- Supports modem presets, TX power, channel, spreading factor, and bandwidth
- Automatic validation of parameters
- Configuration saved to flash and applied on reboot
- Works with existing text message infrastructure

## Installation

The module has been integrated into the firmware and will be built automatically.

### Files Added
- `src/modules/RadioConfigChangeModule.h` - Module header
- `src/modules/RadioConfigChangeModule.cpp` - Module implementation
- `src/modules/Modules.cpp` - Updated to register the module

### Build Instructions

1. Build the firmware as normal using PlatformIO:
   ```bash
   pio run -e <your_environment>
   ```

2. Flash to your device:
   ```bash
   pio run -e <your_environment> -t upload
   ```

## Usage

### Message Format

Send a text message with the following format:
```
RADIOCONFIG:<COMMAND>:<VALUE>
```

### Supported Commands

#### 1. Change Modem Preset
```
RADIOCONFIG:PRESET:<preset_number>
```

**Valid preset numbers:**
- `0` - LONG_FAST (default)
- `1` - LONG_SLOW
- `2` - VERY_LONG_SLOW (deprecated, requires TCXO)
- `3` - MEDIUM_SLOW
- `4` - MEDIUM_FAST
- `5` - SHORT_SLOW
- `6` - SHORT_FAST
- `7` - LONG_MODERATE
- `8` - SHORT_TURBO (500kHz bandwidth)

**Example:**
```
RADIOCONFIG:PRESET:3
```
This switches all receiving nodes to MEDIUM_SLOW preset.

#### 2. Change TX Power
```
RADIOCONFIG:POWER:<power_dbm>
```

**Range:** 0-30 dBm (actual maximum depends on region and hardware)

**Example:**
```
RADIOCONFIG:POWER:20
```
Sets transmission power to 20 dBm.

#### 3. Change Channel Number
```
RADIOCONFIG:CHANNEL:<channel_num>
```

**Range:** 0-83 (0 = auto-calculate based on channel name)

**Example:**
```
RADIOCONFIG:CHANNEL:10
```
Switches to channel 10.

#### 4. Change Spreading Factor (Manual Mode)
```
RADIOCONFIG:SF:<spread_factor>
```

**Range:** 7-12  
**Note:** This disables preset mode and enables manual configuration.

**Example:**
```
RADIOCONFIG:SF:11
```
Sets spreading factor to 11.

#### 5. Change Bandwidth (Manual Mode)
```
RADIOCONFIG:BW:<bandwidth_khz>
```

**Valid values:**
- `31` - 31.25 kHz
- `62` - 62.5 kHz
- `125` - 125 kHz
- `250` - 250 kHz
- `500` - 500 kHz
- `800` - 800 kHz (2.4 GHz only)
- `1625` - 1625 kHz (2.4 GHz only)

**Note:** This disables preset mode and enables manual configuration.

**Example:**
```
RADIOCONFIG:BW:250
```
Sets bandwidth to 250 kHz.

## How It Works

1. **Message Reception**: The module listens for text messages on the TEXT_MESSAGE_APP port
2. **Command Parsing**: When a message starts with "RADIOCONFIG:", it extracts and validates the command
3. **Configuration Update**: If valid, the radio configuration in memory is updated
4. **Persistence**: Changes are saved to flash storage
5. **Reboot**: The node automatically reboots after 5 seconds to apply the new configuration

## Examples

### Example 1: Switch Network to Long-Slow Preset
Send this message to all nodes:
```
RADIOCONFIG:PRESET:1
```

All nodes will:
- Switch to LONG_SLOW preset
- Save the configuration
- Reboot in 5 seconds
- Rejoin the mesh with the new settings

### Example 2: Reduce Power for Battery Saving
```
RADIOCONFIG:POWER:10
```

### Example 3: Custom Configuration (SF + BW)
First set spreading factor:
```
RADIOCONFIG:SF:9
```

Then set bandwidth (this will also set SF=9 again):
```
RADIOCONFIG:BW:125
```

**Note:** For custom configurations, you may want to set both SF and BW. The module will disable preset mode automatically.

### Example 4: Emergency Channel Switch
If the current channel is congested:
```
RADIOCONFIG:CHANNEL:42
```

## Security Considerations

⚠️ **Important:** This module responds to ANY text message with the correct format. Consider these security implications:

1. **Anyone on your mesh can change radio settings**
2. **Malicious actors could disrupt your network** by sending invalid configurations
3. **Network fragmentation** could occur if only some nodes receive the command

### Recommended Security Measures

1. **Use Admin Channel**: Modify the module to only accept commands on the admin channel
2. **Add Authentication**: Require a passcode in the message (e.g., `RADIOCONFIG:SECRETCODE:PRESET:1`)
3. **Limit Senders**: Check the sender's node ID against a whitelist
4. **Disable When Not Needed**: Comment out the module registration in Modules.cpp when not in use

### Adding Authentication (Example Modification)

Edit `RadioConfigChangeModule.cpp` and add a passcode check:

```cpp
const char *PASSCODE = "MySecret123";  // Change this!

ProcessMessage RadioConfigChangeModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // ... existing code ...
    
    // Check for passcode before "RADIOCONFIG:"
    if (strncmp(message, PASSCODE, strlen(PASSCODE)) != 0) {
        return ProcessMessage::CONTINUE;
    }
    
    // Skip passcode in message
    message += strlen(PASSCODE);
    if (*message != ':') return ProcessMessage::CONTINUE;
    message++;
    
    // Now check for "RADIOCONFIG:"
    // ... rest of code ...
}
```

Then send messages like: `MySecret123:RADIOCONFIG:PRESET:1`

## Troubleshooting

### Command Not Working

1. **Check Serial/Debug Output**: Connect via USB and check logs for:
   ```
   RadioConfigChangeModule initialized - listening for config commands
   Received radio config command: <command> from node 0x<id>
   ```

2. **Verify Message Format**: Ensure no extra spaces or typos
   - Correct: `RADIOCONFIG:PRESET:1`
   - Wrong: `RADIOCONFIG: PRESET:1` (space after colon)
   - Wrong: `radioconfig:preset:1` (lowercase)

3. **Check Validation Errors**: Invalid values will be logged:
   ```
   Invalid preset value: 99
   Failed to apply radio config command
   ```

### Node Doesn't Reboot

- Check that the configuration actually changed
- If using SimRadio (Portduino), reboot may be skipped
- Manually reboot the node if needed

### Changes Don't Persist

- Ensure flash storage is not corrupted
- Check that nodeDB save operation succeeded in logs

## Advanced Customization

### Adding New Commands

Edit `RadioConfigChangeModule.cpp` in the `executeConfigCommand()` function:

```cpp
else if (strcmp(cmdType, "MYCOMMAND") == 0) {
    // Your custom logic here
    int myValue = atoi(cmdValue);
    config.lora.some_setting = myValue;
    LOG_INFO("Changed my setting to: %d", myValue);
    return true;
}
```

### Changing the Command Prefix

To use a different prefix than "RADIOCONFIG:", edit the prefix variable in `handleReceived()`:

```cpp
const char *prefix = "MESHCONFIG:";  // Or whatever you prefer
```

### Disabling Auto-Reboot

If you want to manually control when to reboot, comment out this line in `applyConfigChanges()`:

```cpp
// rebootAtMsec = millis() + (5 * 1000);
```

## Module API

### Public Methods

- `RadioConfigChangeModule()` - Constructor
- `ProcessMessage handleReceived(const meshtastic_MeshPacket &mp)` - Message handler
- `bool wantPacket(const meshtastic_MeshPacket *p)` - Packet filter

### Private Methods

- `bool executeConfigCommand(const char *command)` - Parse and execute commands
- `void applyConfigChanges()` - Save to flash and trigger reboot
- `bool validate*()` - Validation methods for each parameter type

## Integration with Other Modules

This module is designed to coexist with:
- **TextMessageModule**: Continues normal text message handling
- **AdminModule**: Can be used alongside admin commands
- **All other modules**: No conflicts

## Performance Impact

- **Memory**: ~2KB additional flash, minimal RAM
- **CPU**: Only processes text messages (negligible overhead)
- **Network**: No additional traffic (uses existing text messages)

## Future Enhancements

Potential improvements (not yet implemented):

1. **Batch Commands**: Support multiple changes in one message
2. **Scheduled Changes**: Apply changes at a specific time
3. **ACK Messages**: Send confirmation back to sender
4. **Rollback**: Revert to previous config if new one doesn't work
5. **Region Changes**: Support changing LoRa region
6. **Encrypted Commands**: Use PKI encryption for commands

## License

This module is part of the Meshtastic firmware and follows the same GPL-3.0 license.

## Support

For issues or questions:
- Check debug logs for error messages
- Verify your message format matches exactly
- Ensure values are within valid ranges
- Test with simple commands first (e.g., PRESET change)
