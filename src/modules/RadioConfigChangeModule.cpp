#include "RadioConfigChangeModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "main.h"
#include "graphics/Screen.h"
#include <string.h>

// External variables defined in main.cpp
extern uint32_t rebootAtMsec;

RadioConfigChangeModule *radioConfigChangeModule;

RadioConfigChangeModule::RadioConfigChangeModule()
    : SinglePortModule("RadioConfigChange", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    // Initialize module
    LOG_INFO("RadioConfigChangeModule initialized - listening for config commands");
}

ProcessMessage RadioConfigChangeModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Only process decoded text messages
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return ProcessMessage::CONTINUE;
    }

    auto &p = mp.decoded;
    if (p.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return ProcessMessage::CONTINUE;
    }

    // Get the message payload
    const char *message = (const char *)p.payload.bytes;
    size_t messageLen = p.payload.size;

    // Check if message starts with "RADIOCONFIG:"
    const char *prefix = "RADIOCONFIG:";
    size_t prefixLen = strlen(prefix);

    if (messageLen < prefixLen + 3) { // Need at least "RADIOCONFIG:X:Y"
        return ProcessMessage::CONTINUE;
    }

    if (strncmp(message, prefix, prefixLen) != 0) {
        return ProcessMessage::CONTINUE;
    }

    // Extract command (everything after "RADIOCONFIG:")
    char command[100];
    size_t cmdLen = messageLen - prefixLen;
    if (cmdLen >= sizeof(command)) {
        cmdLen = sizeof(command) - 1;
    }
    memcpy(command, message + prefixLen, cmdLen);
    command[cmdLen] = '\0';

    LOG_INFO("Received radio config command: %s from node 0x%08x", command, mp.from);

    // Execute the command
    if (executeConfigCommand(command)) {
        LOG_INFO("Successfully applied radio config change");
        applyConfigChanges();
        
        // Notify user of success
        powerFSM.trigger(EVENT_RECEIVED_MSG);
        
        // Don't let other modules process this - we've handled it
        return ProcessMessage::STOP;
    } else {
        LOG_WARN("Failed to apply radio config command: %s", command);
    }

    return ProcessMessage::CONTINUE;
}

bool RadioConfigChangeModule::wantPacket(const meshtastic_MeshPacket *p)
{
    // We want text message packets to check for config commands
    return MeshService::isTextPayload(p);
}

bool RadioConfigChangeModule::executeConfigCommand(const char *command)
{
    // Parse command format: "COMMAND:VALUE"
    char cmdType[20];
    char cmdValue[80];
    
    const char *colon = strchr(command, ':');
    if (!colon) {
        LOG_ERROR("Invalid command format - no colon separator");
        return false;
    }

    size_t typeLen = colon - command;
    if (typeLen >= sizeof(cmdType)) {
        typeLen = sizeof(cmdType) - 1;
    }
    memcpy(cmdType, command, typeLen);
    cmdType[typeLen] = '\0';

    strcpy(cmdValue, colon + 1);

    LOG_DEBUG("Command type: '%s', value: '%s'", cmdType, cmdValue);

    // Process different command types
    if (strcmp(cmdType, "PRESET") == 0) {
        uint8_t preset = atoi(cmdValue);
        if (!validatePreset(preset)) {
            LOG_ERROR("Invalid preset value: %d", preset);
            return false;
        }
        config.lora.use_preset = true;
        config.lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)preset;
        LOG_INFO("Changed modem preset to: %d", preset);
        return true;
    }
    else if (strcmp(cmdType, "POWER") == 0) {
        int8_t power = atoi(cmdValue);
        if (!validatePower(power)) {
            LOG_ERROR("Invalid power value: %d", power);
            return false;
        }
        config.lora.tx_power = power;
        LOG_INFO("Changed TX power to: %d dBm", power);
        return true;
    }
    else if (strcmp(cmdType, "CHANNEL") == 0) {
        uint8_t channel = atoi(cmdValue);
        if (!validateChannel(channel)) {
            LOG_ERROR("Invalid channel value: %d", channel);
            return false;
        }
        config.lora.channel_num = channel;
        LOG_INFO("Changed channel to: %d", channel);
        return true;
    }
    else if (strcmp(cmdType, "SF") == 0) {
        uint8_t sf = atoi(cmdValue);
        if (!validateSpreadFactor(sf)) {
            LOG_ERROR("Invalid spread factor: %d", sf);
            return false;
        }
        config.lora.use_preset = false; // Must disable presets for manual config
        config.lora.spread_factor = sf;
        LOG_INFO("Changed spread factor to: %d", sf);
        return true;
    }
    else if (strcmp(cmdType, "BW") == 0) {
        uint16_t bw = atoi(cmdValue);
        if (!validateBandwidth(bw)) {
            LOG_ERROR("Invalid bandwidth: %d", bw);
            return false;
        }
        config.lora.use_preset = false; // Must disable presets for manual config
        config.lora.bandwidth = bw;
        LOG_INFO("Changed bandwidth to: %d kHz", bw);
        return true;
    }
    else {
        LOG_ERROR("Unknown command type: %s", cmdType);
        return false;
    }
}

void RadioConfigChangeModule::applyConfigChanges()
{
    // Save configuration to flash
    LOG_INFO("Saving radio configuration changes...");
    service->reloadConfig(SEGMENT_CONFIG); // This calls saveToDisk among other things
    
    // Show message on screen if available
    if (screen) {
        screen->showSimpleBanner("Radio Config\nChanged", 3000);
    }
    
    // Trigger reboot to apply changes
    LOG_INFO("Radio configuration changed - rebooting in 5 seconds to apply...");
    
    // Schedule reboot (rebootAtMsec is a global variable defined in main.cpp)
    rebootAtMsec = millis() + (5 * 1000);
}

bool RadioConfigChangeModule::validatePreset(uint8_t preset)
{
    // Valid presets: 0 (LONG_FAST) through 8 (SHORT_TURBO)
    return preset <= meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO;
}

bool RadioConfigChangeModule::validatePower(int8_t power)
{
    // Typical range: 0-30 dBm, but let the radio interface handle final limiting
    return power >= 0 && power <= 30;
}

bool RadioConfigChangeModule::validateSpreadFactor(uint8_t sf)
{
    // Valid spreading factors: 7-12
    return sf >= 7 && sf <= 12;
}

bool RadioConfigChangeModule::validateBandwidth(uint16_t bw)
{
    // Common valid bandwidths for LoRa (in kHz)
    // Note: 31 is special and gets converted to 31.25
    switch (bw) {
        case 31:    // Will become 31.25 MHz
        case 62:    // 62.5 kHz  
        case 125:
        case 250:
        case 500:
        case 800:   // For 2.4GHz
        case 1625:  // For 2.4GHz
            return true;
        default:
            return false;
    }
}

bool RadioConfigChangeModule::validateChannel(uint8_t channel)
{
    // Channels 0-83 are typically valid (0 means auto-calculate)
    return channel <= 83;
}
