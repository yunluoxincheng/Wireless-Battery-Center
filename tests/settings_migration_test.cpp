#include "../src/device_profiles.h"
#include "../src/settings.h"

#include <cassert>
#include <iostream>

int main() {
    const auto fixture = L"Software\\WirelessBatteryCenter.Test." + std::to_wstring(GetCurrentProcessId());
    const auto legacy = fixture + L"\\Legacy";
    const auto center = fixture + L"\\Center";
    const auto identity = L"physical-1";
    const auto bound = L"legacy-1";
    const auto other = L"physical-2";
    const auto legacyPanel = legacy + L"\\Panel";
    const auto centerPanel = center + L"\\Panel";
    const auto write = [&](const std::wstring& path, const wchar_t* name, const std::vector<std::wstring>& values) {
        assert(WriteMultiSetting(HKEY_CURRENT_USER, path.c_str(), name, values));
    };
    const auto read = [&](const std::wstring& path, const wchar_t* name) {
        std::vector<std::wstring> values;
        assert(ReadMultiSetting(HKEY_CURRENT_USER, path.c_str(), name, values));
        return values;
    };

    DeviceProfile profile;
    profile.name = L"Test device";
    ProfileStore original(legacy), replacement(center);
    assert(original.Save(identity, profile) && original.Save(other, profile));
    const std::vector<std::wstring> aliases{bound, L"Old alias", identity, L"Direct alias", other, L"Keep"};
    const std::vector<std::wstring> all{bound, identity, other};
    write(legacy, L"DeviceAliases", aliases);
    write(legacy, L"PanelLegacyIdentities", {identity, bound});
    for (const auto* name : {L"HiddenDevices", L"PanelHiddenDevices", L"DeviceOrder"}) write(legacy, name, all);
    write(legacy, L"WirelessBattery", {L"Must not import"});
    write(legacyPanel + L"\\Appearance", L"themeMode", {L"dark"});
    write(legacyPanel + L"\\Drivers", L"path", {L"Must reselect"});

    // An unreadable profile name interrupts the pass after the root preferences are copied.
    const auto legacyArchives = legacy + L"\\DeviceProfiles";
    const std::wstring unreadable(1100, L'x');
    const wchar_t fixtureValue[] = L"fixture";
    assert(RegSetKeyValueW(HKEY_CURRENT_USER, legacyArchives.c_str(), unreadable.c_str(), REG_SZ,
                           fixtureValue, sizeof(fixtureValue)) == ERROR_SUCCESS);
    assert(!ImportLegacySettings(HKEY_CURRENT_USER, legacy.c_str(), center.c_str(), false));
    DWORD version = 0, size = sizeof(version);
    assert(RegGetValueW(HKEY_CURRENT_USER, center.c_str(), L"LegacyImportVersion", RRF_RT_REG_DWORD,
                        nullptr, &version, &size) == ERROR_FILE_NOT_FOUND);
    assert(read(center, L"DeviceAliases") == aliases);
    auto changedAliases = aliases;
    changedAliases[3] = L"Edited after interruption";
    write(center, L"DeviceAliases", changedAliases);
    DeviceProfile edited = profile;
    edited.name = L"Updated Center record";
    assert(replacement.Save(identity, edited));
    assert(RegDeleteKeyValueW(HKEY_CURRENT_USER, legacyArchives.c_str(), unreadable.c_str()) == ERROR_SUCCESS);

    assert(ImportLegacySettings(HKEY_CURRENT_USER, legacy.c_str(), center.c_str(), false));
    assert(RegGetValueW(HKEY_CURRENT_USER, center.c_str(), L"LegacyImportVersion", RRF_RT_REG_DWORD,
                        nullptr, &version, &size) == ERROR_SUCCESS && version == 1);
    assert(ImportLegacySettings(HKEY_CURRENT_USER, legacyPanel.c_str(), centerPanel.c_str(), true));
    assert(read(centerPanel + L"\\Appearance", L"themeMode") == std::vector<std::wstring>{L"dark"});
    write(centerPanel + L"\\Appearance", L"themeMode", {L"light"});
    assert(ImportLegacySettings(HKEY_CURRENT_USER, legacyPanel.c_str(), centerPanel.c_str(), true));
    assert(read(centerPanel + L"\\Appearance", L"themeMode") == std::vector<std::wstring>{L"light"});
    assert(read(centerPanel + L"\\Drivers", L"path").empty());
    assert(read(center, L"DeviceAliases") == changedAliases && read(center, L"WirelessBattery").empty());
    bool issues = false;
    const auto resumed = replacement.Load(issues);
    assert(resumed.size() == 2 && !issues);
    for (const auto& [key, record] : resumed)
        assert(record.name == (key == identity ? edited.name : profile.name));

    // Interrupt cleanup with a wrong registry type. The archive remains until its preferences are cleared.
    const DWORD invalid = 1;
    assert(RegSetKeyValueW(HKEY_CURRENT_USER, center.c_str(), L"DeviceOrder", REG_DWORD, &invalid,
                           sizeof(invalid)) == ERROR_SUCCESS);
    std::wstring pending;
    assert(!ForgetDeviceProfile(center.c_str(), identity, bound, pending) && pending == identity);
    assert(replacement.Load(issues).size() == 2);
    assert(!ForgetDeviceProfile(center.c_str(), other, other, pending));
    write(center, L"DeviceOrder", all);
    pending.clear();
    assert(RecoverDeviceRemoval(center.c_str(), pending) && pending.empty());
    assert(read(center, L"DeviceAliases") == (std::vector<std::wstring>{other, L"Keep"}));
    assert(read(center, L"PanelLegacyIdentities").empty());
    for (const auto* name : {L"HiddenDevices", L"PanelHiddenDevices", L"DeviceOrder"})
        assert(read(center, name) == std::vector<std::wstring>{other});

    assert(ImportLegacySettings(HKEY_CURRENT_USER, legacy.c_str(), center.c_str(), false));
    assert(replacement.Load(issues).size() == 1 && replacement.Load(issues)[0].first == other);
    assert(replacement.Save(identity, profile)); // receiving it again creates a clean profile
    assert(read(center, L"DeviceAliases") == (std::vector<std::wstring>{other, L"Keep"}));
    const auto source = original.Load(issues);
    assert(read(legacy, L"DeviceAliases") == aliases && source.size() == 2 && !issues);
    for (const auto& [key, record] : source) assert(record.name == profile.name);

    for (const auto& key : {centerPanel + L"\\Appearance", centerPanel, center + L"\\DeviceProfiles", center,
                            legacyPanel + L"\\Drivers", legacyPanel + L"\\Appearance", legacyPanel,
                            legacy + L"\\DeviceProfiles", legacy, fixture}) {
        assert(RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS);
    }
    std::cout << "Legacy import, interrupted removal recovery, preference reset and re-registration passed.\n";
}
