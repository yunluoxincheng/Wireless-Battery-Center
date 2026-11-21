#include "device_profiles.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace {

constexpr auto MaxValueChars = 8192;

std::wstring Encode(const DeviceProfile& p) {
    std::wostringstream out;
    out << 1 << L' ' << static_cast<int>(p.kind) << L' ' << static_cast<int>(p.battery)
        << L' ' << p.vid << L' ' << p.pid << L' ' << p.panelOnly << L' ' << p.percent
        << L' ' << p.firstOnline << L' ' << p.lastOnline << L' ' << p.readingTime
        << L' ' << std::quoted(p.name) << L' ' << std::quoted(p.via) << L' ' << std::quoted(p.brand)
        << L' ' << std::quoted(p.protocol) << L' ' << std::quoted(p.level) << L' ' << std::quoted(p.readingSource);
    return out.str();
}

bool Decode(const std::wstring& value, DeviceProfile& p) {
    std::wistringstream in(value);
    int version, kind, capability;
    if (!(in >> version) || version != 1) return false;
    if (!(in >> kind >> capability >> p.vid >> p.pid >> p.panelOnly >> p.percent
             >> p.firstOnline >> p.lastOnline >> p.readingTime >> std::quoted(p.name)
             >> std::quoted(p.via) >> std::quoted(p.brand) >> std::quoted(p.protocol)
             >> std::quoted(p.level) >> std::quoted(p.readingSource))) return false;
    if (!(in >> std::ws).eof() || kind < 0 || kind > static_cast<int>(DeviceKind::Phone) ||
        capability < 0 || capability > static_cast<int>(BatteryCapability::None) || p.name.empty() ||
        p.percent < -1 || p.percent > 100 || (p.percent >= 0) != (p.readingTime != 0) ||
        p.firstOnline > p.lastOnline || (p.firstOnline == 0) != (p.lastOnline == 0)) return false;
    p.kind = static_cast<DeviceKind>(kind);
    p.battery = static_cast<BatteryCapability>(capability);
    return true;
}

} // namespace

Presence ObservedPresence(const DeviceStatus& s) {
    if (!s.connected) return Presence::Offline;
    if (s.via == L"USB cable" || s.wirelessLink == WirelessLink::Linked) return Presence::Online;
    if (s.wirelessLink == WirelessLink::Unknown || s.wirelessLink == WirelessLink::Unlinked)
        return Presence::Unknown;
    if (!s.cached && !s.asleep && s.percent >= 0 && s.percent <= 100) return Presence::Online;
    return Presence::Unknown;
}

bool ObserveProfile(DeviceProfile& p, const DeviceStatus& s, Presence presence,
                    const std::wstring& brand, const std::wstring& protocol, std::uint64_t now) {
    const auto battery = s.battery != BatteryCapability::Unknown ? s.battery :
        (s.percent >= 0 && s.percent <= 100 ? BatteryCapability::Readable : p.battery);
    bool changed = p.name != s.name || p.via != s.via || p.kind != s.kind || p.battery != battery ||
        p.brand != brand || p.vid != s.vid || p.pid != s.pid || p.protocol != protocol || p.panelOnly != s.panelOnly;
    p.name = s.name;
    p.via = s.via;
    p.kind = s.kind;
    p.battery = battery;
    p.brand = brand;
    p.vid = s.vid;
    p.pid = s.pid;
    p.protocol = protocol;
    p.panelOnly = s.panelOnly;
    if (presence != Presence::Online) return changed;
    if (!p.firstOnline) {
        p.firstOnline = now;
        changed = true;
    }
    // A reused source timestamp is not evidence of a new observation.
    const auto observed = s.readingTimeMs ? std::min(now, s.readingTimeMs) : now;
    p.lastOnline = std::max(p.lastOnline, std::max(p.firstOnline, observed));
    const bool readingChanged = p.percent != s.percent || p.level != s.levelText || p.readingSource != protocol;
    if (!s.cached && !s.asleep && s.percent >= 0 && s.percent <= 100 && observed > p.readingTime &&
        (!s.systemBatterySnapshot || readingChanged)) {
        changed |= readingChanged;
        p.percent = s.percent;
        p.level = s.levelText;
        p.readingSource = protocol;
        p.readingTime = observed;
    }
    return changed;
}

std::vector<std::pair<std::wstring, DeviceProfile>> ProfileStore::Load(bool& issues) const {
    std::vector<std::pair<std::wstring, DeviceProfile>> profiles;
    HKEY key;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, KEY_READ, &key);
    if (opened == ERROR_FILE_NOT_FOUND) return profiles;
    if (opened != ERROR_SUCCESS) {
        issues = true;
        return profiles;
    }
    wchar_t name[1024], value[MaxValueChars];
    for (DWORD i = 0;; ++i) {
        DWORD nameSize = std::size(name), valueSize = sizeof(value), type = 0;
        const auto result = RegEnumValueW(key, i, name, &nameSize, nullptr, &type,
                                          reinterpret_cast<BYTE*>(value), &valueSize);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
            issues = true;
            break;
        }
        DeviceProfile profile;
        const auto chars = valueSize / sizeof(wchar_t);
        if (result == ERROR_SUCCESS && type == REG_SZ && nameSize && chars && valueSize % sizeof(wchar_t) == 0 &&
            value[chars - 1] == 0 && Decode(std::wstring(value, chars - 1), profile)) {
            profiles.emplace_back(std::wstring(name, nameSize), std::move(profile));
        } else {
            // Leave unreadable and future-version values intact.
            issues = true;
        }
    }
    RegCloseKey(key);
    return profiles;
}

bool ProfileStore::Save(const std::wstring& key, const DeviceProfile& profile) const {
    DWORD size = 0;
    const auto previous = RegGetValueW(HKEY_CURRENT_USER, path_.c_str(), key.c_str(), RRF_RT_REG_SZ,
                                       nullptr, nullptr, &size);
    if (previous == ERROR_SUCCESS) {
        if (size < 3 * sizeof(wchar_t) || size > MaxValueChars * sizeof(wchar_t)) return false;
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, path_.c_str(), key.c_str(), RRF_RT_REG_SZ, nullptr,
                         value.data(), &size) != ERROR_SUCCESS || value.compare(0, 2, L"1 ") != 0) return false;
    } else if (previous != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    const auto value = Encode(profile);
    return RegSetKeyValueW(HKEY_CURRENT_USER, path_.c_str(), key.c_str(), REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool ProfileStore::Remove(const std::wstring& key) const {
    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, path_.c_str(), key.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}
