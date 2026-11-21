#include "settings.h"

#include <cwchar>

namespace {

constexpr wchar_t PendingRemoval[] = L"PendingDeviceRemoval";
constexpr wchar_t LegacyImportVersion[] = L"LegacyImportVersion";
constexpr DWORD CurrentImportVersion = 1;

bool ImportMissingValue(HKEY source, HKEY destination, const wchar_t* name) {
    const auto existing = RegQueryValueExW(destination, name, nullptr, nullptr, nullptr, nullptr);
    if (existing == ERROR_SUCCESS) return true;
    if (existing != ERROR_FILE_NOT_FOUND) return false;

    DWORD type = 0, size = 0;
    const auto result = RegQueryValueExW(source, name, nullptr, &type, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) return false;
    std::vector<BYTE> data(size);
    return RegQueryValueExW(source, name, nullptr, &type, data.data(), &size) == ERROR_SUCCESS &&
        RegSetValueExW(destination, name, 0, type, data.data(), size) == ERROR_SUCCESS;
}

bool CopySubkey(HKEY source, HKEY destination, const wchar_t* name, const wchar_t* value = nullptr) {
    HKEY oldKey;
    const auto result = RegOpenKeyExW(source, name, 0, KEY_READ, &oldKey);
    if (result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) return false;
    HKEY newKey;
    const bool opened = RegCreateKeyExW(destination, name, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &newKey, nullptr)
        == ERROR_SUCCESS;
    bool copied = opened;
    if (opened) {
        if (value) {
            copied = ImportMissingValue(oldKey, newKey, value);
        } else {
            // Profiles are flat values. Copy the data without inheriting the old key's ACL.
            for (DWORD index = 0;; ++index) {
                wchar_t key[1024];
                DWORD length = 1024;
                const auto enumerated = RegEnumValueW(oldKey, index, key, &length, nullptr, nullptr, nullptr, nullptr);
                if (enumerated == ERROR_NO_MORE_ITEMS) break;
                if (enumerated != ERROR_SUCCESS) {
                    copied = false;
                    break;
                }
                copied = ImportMissingValue(oldKey, newKey, key) && copied;
            }
        }
        RegCloseKey(newKey);
    }
    RegCloseKey(oldKey);
    return copied;
}

bool ErasePreference(HKEY root, const wchar_t* path, const wchar_t* name, size_t stride,
                     const std::wstring& identity, const std::wstring& legacy) {
    std::vector<std::wstring> values, kept;
    if (!ReadMultiSetting(root, path, name, values) || values.size() % stride != 0) return false;
    for (size_t i = 0; i < values.size(); i += stride) {
        if (values[i] == identity || values[i] == legacy) continue;
        kept.insert(kept.end(), values.begin() + i, values.begin() + i + stride);
    }
    return values == kept || WriteMultiSetting(root, path, name, kept);
}

} // namespace

bool ReadMultiSetting(HKEY root, const wchar_t* path, const wchar_t* name, std::vector<std::wstring>& values) {
    values.clear();
    DWORD size = 0;
    const auto result = RegGetValueW(root, path, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS || size % sizeof(wchar_t) != 0) return false;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2);
    DWORD capacity = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(root, path, name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &capacity) != ERROR_SUCCESS)
        return false;
    for (const wchar_t* p = buffer.data(); *p; p += wcslen(p) + 1) values.emplace_back(p);
    return true;
}

bool WriteMultiSetting(HKEY root, const wchar_t* path, const wchar_t* name, const std::vector<std::wstring>& values) {
    std::wstring multi;
    for (const auto& value : values) { multi += value; multi.push_back(L'\0'); }
    if (multi.empty()) multi.push_back(L'\0');
    multi.push_back(L'\0');
    return RegSetKeyValueW(root, path, name, REG_MULTI_SZ, multi.data(),
                           static_cast<DWORD>(multi.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool ImportLegacySettings(HKEY root, const wchar_t* legacy, const wchar_t* destination, bool panel) {
    HKEY target;
    if (RegCreateKeyExW(root, destination, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &target, nullptr)
        != ERROR_SUCCESS) return false;
    DWORD version = 0, size = sizeof(version);
    const auto marker = RegGetValueW(target, nullptr, LegacyImportVersion, RRF_RT_REG_DWORD, nullptr, &version, &size);
    if (marker == ERROR_SUCCESS && version >= CurrentImportVersion) {
        RegCloseKey(target);
        return true;
    }

    bool ok = marker == ERROR_SUCCESS || marker == ERROR_FILE_NOT_FOUND;
    if (ok) {
        HKEY source;
        const auto opened = RegOpenKeyExW(root, legacy, 0, KEY_READ, &source);
        if (opened == ERROR_SUCCESS) {
            if (panel) {
                ok = CopySubkey(source, target, L"Appearance", L"themeMode");
            } else {
                for (const auto* name : {L"HiddenDevices", L"PanelHiddenDevices", L"DeviceOrder", L"DeviceAliases",
                    L"PanelLegacyIdentities", L"LowBatteryPercent", L"LowBatteryNotifications", L"TaskbarWidget",
                    L"WidgetMonitor", L"OtherTaskbars", L"WidgetBackground", L"StackedLayout", L"BrandColors",
                    L"HideLastKnownDevices", L"TaskbarOffset", L"IgnoreDevices", L"Language"}) {
                    ok = ImportMissingValue(source, target, name) && ok;
                }
                ok = CopySubkey(source, target, L"DeviceProfiles") && ok;
            }
            RegCloseKey(source);
        } else if (opened != ERROR_FILE_NOT_FOUND) {
            ok = false;
        }
    }
    if (ok) {
        ok = RegSetValueExW(target, LegacyImportVersion, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&CurrentImportVersion), sizeof(CurrentImportVersion)) == ERROR_SUCCESS;
    }
    RegCloseKey(target);
    return ok;
}

bool RecoverDeviceRemoval(const wchar_t* path, std::wstring& pending) {
    std::vector<std::wstring> record;
    if (!ReadMultiSetting(HKEY_CURRENT_USER, path, PendingRemoval, record)) return false;
    if (record.empty()) {
        pending.clear();
        return true;
    }
    if (record.size() != 2) return false;
    const auto& key = record[0];
    const auto& legacy = record[1];
    pending = key;
    for (const auto* name : {L"HiddenDevices", L"PanelHiddenDevices", L"DeviceOrder"}) {
        if (!ErasePreference(HKEY_CURRENT_USER, path, name, 1, key, legacy)) return false;
    }
    for (const auto* name : {L"DeviceAliases", L"PanelLegacyIdentities"}) {
        if (!ErasePreference(HKEY_CURRENT_USER, path, name, 2, key, legacy)) return false;
    }
    const auto profilePath = std::wstring(path) + L"\\DeviceProfiles";
    const auto removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, profilePath.c_str(), key.c_str());
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) return false;
    if (RegDeleteKeyValueW(HKEY_CURRENT_USER, path, PendingRemoval) != ERROR_SUCCESS) return false;
    pending.clear();
    return true;
}

bool ForgetDeviceProfile(const wchar_t* path, const std::wstring& key, const std::wstring& preference,
                         std::wstring& pending) {
    std::vector<std::wstring> record;
    if (!ReadMultiSetting(HKEY_CURRENT_USER, path, PendingRemoval, record)) return false;
    if (!record.empty() && (record.size() != 2 || record.front() != key)) return false;
    if (record.empty() && !WriteMultiSetting(HKEY_CURRENT_USER, path, PendingRemoval, {key, preference}))
        return false;
    return RecoverDeviceRemoval(path, pending);
}
