#pragma once
#include "SinglePortModule.h"

/**
 * RadioConfigChangeModule - Module to change radio configuration based on received messages
 * 
 * This module listens for specially formatted text messages and updates the radio configuration
 * in real-time. Useful for coordinating configuration changes across a mesh network.
 * 
 * Message format: "RADIOCONFIG:<command>"
 * 
 * Supported commands:
 * - PRESET:<preset_number>  (e.g., "RADIOCONFIG:PRESET:1" for LONG_SLOW)
 * - POWER:<power_dbm>       (e.g., "RADIOCONFIG:POWER:20")
 * - CHANNEL:<channel_num>   (e.g., "RADIOCONFIG:CHANNEL:10")
 * - SF:<spread_factor>      (e.g., "RADIOCONFIG:SF:11" for spread factor 11)
 * - BW:<bandwidth>          (e.g., "RADIOCONFIG:BW:250" for 250kHz)
 * 
 * Example: Send a text message "RADIOCONFIG:PRESET:3" to switch all receiving nodes to MEDIUM_SLOW preset
 */
class RadioConfigChangeModule : public SinglePortModule
{
  public:
    /** Constructor */
    RadioConfigChangeModule();

  protected:
    /** Called to handle a particular incoming message
     * @return ProcessMessage::STOP if handled, ProcessMessage::CONTINUE to allow other modules
     */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    /** Indicate we want text message packets */
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;

  private:
    /**
     * Parse and execute radio config change command
     * @param command The command string (e.g., "PRESET:1", "POWER:20")
     * @return true if command was valid and executed
     */
    bool executeConfigCommand(const char *command);

    /**
     * Apply the configuration changes and reboot if necessary
     */
    void applyConfigChanges();

    /**
     * Validate that values are within acceptable ranges
     */
    bool validatePreset(uint8_t preset);
    bool validatePower(int8_t power);
    bool validateSpreadFactor(uint8_t sf);
    bool validateBandwidth(uint16_t bw);
    bool validateChannel(uint8_t channel);
};

extern RadioConfigChangeModule *radioConfigChangeModule;
