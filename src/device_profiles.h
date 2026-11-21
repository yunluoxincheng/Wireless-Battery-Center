#pragma once

#include "devices.h"
#include <windows.h>

struct DeviceProfile {
    std::wstring name, via, brand, protocol, level, readingSource;
    DeviceKind kind = DeviceKind::Other;
    BatteryCapability battery = BatteryCapability::Unknown;
    unsigned short vid = 0, pid = 0;
    bool panelOnly = false;
    int percent = -1;
    std::uint64_t firstOnline = 0, lastOnline = 0, readingTime = 0;
};

Presence ObservedPresence(const DeviceStatus& status);
bool ObserveProfile(DeviceProfile& profile, const DeviceStatus& status, Presence presence,
                    const std::wstring& brand, const std::wstring& protocol, std::uint64_t now);

// Each device is saved as a single versioned registry value. Observations never rewrite
// aliases, visibility, order, or other settings under the parent key.
class ProfileStore {
public:
    explicit ProfileStore(std::wstring settingsKey) : path_(settingsKey + L"\\DeviceProfiles") {}
    std::vector<std::pair<std::wstring, DeviceProfile>> Load(bool& issues) const;
    bool Save(const std::wstring& key, const DeviceProfile& profile) const;
    bool Remove(const std::wstring& key) const;

private:
    std::wstring path_;
};
