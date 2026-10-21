#pragma once

#include <string>

/**
 * @brief Owns a persistent device ID and an editable display name.
 *
 * Loads or creates the ID and reads devicename.txt at construction. Both files
 * are relative to the working directory; a missing name file means an empty name.
 */
class DeviceIdentity {
public:
    /**
     * @param deviceId Explicit ID, or empty to load/create deviceid.txt relative
     * to the working directory. Explicit IDs are not written to disk.
     * @throws std::runtime_error If saving a newly generated UUID or reading an
     * existing name file fails.
     */
    explicit DeviceIdentity(std::string deviceId = {});

    const std::string& getDeviceId() const { return deviceId_; }
    const std::string& getDeviceName() const { return deviceName_; }
    // Saves the complete name (including spaces) before updating the cached value.
    // Throws std::runtime_error on save failure. An empty name clears the name.
    void setDeviceName(std::string name);
    // Refresh a name saved by the service without writing the identity files again.
    void reloadDeviceName() { deviceName_ = loadDeviceName(); }

private:
    static std::string getOrCreateDeviceId();
    static std::string loadDeviceName();

    const std::string deviceId_;
    std::string deviceName_;
};
