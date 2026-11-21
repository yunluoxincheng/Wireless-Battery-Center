#include "../src/device_profiles.h"
#include "../src/hid.h"

#include <cassert>
#include <iostream>
#include <map>

int main() {
    HidColl first, second;
    first.vid = second.vid = 0x413C;
    first.pid = second.pid = 0x2113;
    first.container = L"{1}";
    second.container = L"{2}";
    auto otherCollection = first;
    otherCollection.path = L"another HID collection";
    assert(HidIdentity(first) == HidIdentity(otherCollection));
    assert(HidIdentity(first) != HidIdentity(second));
    otherCollection.pid++;
    assert(HidIdentity(first) != HidIdentity(otherCollection));

    DeviceProfile profile;
    DeviceStatus status;
    status.name = L"Device \"\u6d4b\u8bd5\"";
    status.via = L"2.4 GHz receiver";
    status.kind = DeviceKind::Keyboard;
    status.wirelessLink = WirelessLink::Unknown;
    status.battery = BatteryCapability::Unsupported;
    assert(ObservedPresence(status) == Presence::Unknown);
    assert(ObserveProfile(profile, status, ObservedPresence(status), L"Vendor", L"HID", 1000));
    assert(profile.lastOnline == 0 && profile.readingTime == 0 && profile.percent == -1);

    status.battery = BatteryCapability::Unknown;
    status.wirelessLink = WirelessLink::Linked;
    status.percent = 12;
    status.readingTimeMs = 2000;
    assert(ObserveProfile(profile, status, ObservedPresence(status), L"Vendor", L"Protocol", 2000));
    assert(profile.firstOnline == 2000 && profile.lastOnline == 2000);
    assert(profile.percent == 12 && profile.readingSource == L"Protocol");
    assert(!ObserveProfile(profile, status, ObservedPresence(status), L"Vendor", L"Protocol", 5000));
    assert(profile.readingTime == 2000 && profile.lastOnline == 2000);
    status.cached = true;
    status.wirelessLink = WirelessLink::Unknown;
    status.percent = 99;
    assert(!ObserveProfile(profile, status, ObservedPresence(status), L"Vendor", L"Protocol", 5000));
    assert(profile.percent == 12 && profile.lastOnline == 2000);
    status.connected = false;
    assert(ObservedPresence(status) == Presence::Offline);

    DeviceProfile published;
    status.connected = true;
    status.cached = false;
    status.systemBatterySnapshot = true;
    status.wirelessLink = WirelessLink::Linked;
    status.readingTimeMs = 0;
    assert(ObserveProfile(published, status, ObservedPresence(status), L"Vendor", L"Windows Bluetooth", 6000));
    assert(!ObserveProfile(published, status, ObservedPresence(status), L"Vendor", L"Windows Bluetooth", 7000));
    assert(published.lastOnline == 7000 && published.readingTime == 6000);

    DeviceStatus wired;
    wired.via = L"USB cable";
    wired.battery = BatteryCapability::None;
    assert(ObservedPresence(wired) == Presence::Online && wired.percent == -1);

    const auto path = L"Software\\WirelessBattery.ProfileTest." + std::to_wstring(GetCurrentProcessId());
    HKEY temp;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                           KEY_ALL_ACCESS, nullptr, &temp, nullptr) == ERROR_SUCCESS);
    HKEY records;
    assert(RegCreateKeyExW(temp, L"DeviceProfiles", 0, nullptr, REG_OPTION_VOLATILE,
                           KEY_ALL_ACCESS, nullptr, &records, nullptr) == ERROR_SUCCESS);
    const ProfileStore initial(path);
    assert(initial.Save(HidIdentity(first), profile));
    profile.percent = 54;
    assert(initial.Save(HidIdentity(second), profile));

    // A fresh store reads distinct instances and skips damage or unknown schema versions.
    constexpr wchar_t future[] = L"2 newer format";
    assert(RegSetValueExW(records, L"future", 0, REG_SZ, reinterpret_cast<const BYTE*>(future),
                         sizeof(future)) == ERROR_SUCCESS);
    constexpr wchar_t corrupt[] = L"1 truncated";
    assert(RegSetValueExW(records, L"corrupt", 0, REG_SZ, reinterpret_cast<const BYTE*>(corrupt),
                         sizeof(corrupt)) == ERROR_SUCCESS);
    const ProfileStore reopened(path);
    bool issues = false;
    const auto loaded = reopened.Load(issues);
    const std::map<std::wstring, DeviceProfile> instances(loaded.begin(), loaded.end());
    assert(issues && instances.size() == 2);
    assert(instances.at(HidIdentity(first)).percent == 12);
    assert(instances.at(HidIdentity(second)).percent == 54);
    assert(instances.at(HidIdentity(first)).name == status.name);
    assert(!reopened.Save(L"future", profile));
    assert(reopened.Remove(HidIdentity(first)));
    assert(reopened.Load(issues).size() == 1);
    assert(reopened.Save(HidIdentity(first), profile));
    assert(reopened.Load(issues).size() == 2);
    RegCloseKey(records);
    assert(RegDeleteKeyW(temp, L"DeviceProfiles") == ERROR_SUCCESS);
    RegCloseKey(temp);
    assert(RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    std::cout << "Device profile identity, observation, storage and recovery passed.\n";
}
