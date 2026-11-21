#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

enum class DeviceKind { Other, Mouse, Headset, Keyboard, Gamepad, Phone };
enum class WirelessLink : std::uint8_t { Unreported, Unknown, Linked, Unlinked };
enum class BatteryCapability : std::uint8_t { Unknown, Readable, Unsupported, None };
enum class Presence : std::uint8_t { Offline, Unknown, Online };

// One battery-powered device, as seen during a single poll.
struct DeviceStatus {
    std::wstring key;          // stable identity across polls (used for the tray icon id)
    std::wstring name;         // human readable name
    std::wstring via;          // "2.4 GHz receiver", "Bluetooth", "USB cable", ...
    DeviceKind kind = DeviceKind::Other;
    int  percent    = -1;      // 0..100, or -1 when unknown right now
    std::wstring detail;       // extra breakdown for the tooltip, e.g. "L 80%, R 75%, case 40%"
    std::wstring levelText;    // set when the device only reports coarse levels ("Low", "Full"...)
    bool charging   = false;
    bool asleep     = false;   // receiver answered but the device is sleeping (keep last value)
    bool connected  = true;    // false when the driver can confirm the device is unavailable
    int  millivolts = 0;       // battery voltage when reported (0 = unknown)
    int  graceSec   = 0;       // keep showing the last value this long if the device stops answering
    unsigned short vid = 0;    // USB / Bluetooth HID vendor id when known (picks the brand color)
    unsigned short pid = 0;    // product id of the interface that supplied this status, when known
    const char* protocol = nullptr; // diagnostic driver or protocol name (static storage)
    bool cached = false;       // the source reused a previous reading this poll
    WirelessLink wirelessLink = WirelessLink::Unreported; // separate receiver telemetry, when supported
    std::uint64_t readingTimeMs = 0; // source timestamp in Unix milliseconds, when supplied
    std::wstring identity;     // verified Windows device instance or device-reported unit identity
    BatteryCapability battery = BatteryCapability::Unknown;
    bool panelOnly = false;    // discovery metadata, never a taskbar/tray battery entry
    bool systemBatterySnapshot = false; // level published without a measurement timestamp
};

// Queries every supported device. Blocking (can take a second or two); call from a worker thread.
#ifdef _WIN32
std::vector<DeviceStatus> PollAllDevices(bool manualRefresh = false, std::set<std::wstring>* endpoints = nullptr);
#else
std::vector<DeviceStatus> PollAllDevices();
#endif

// Prints every HID collection (for adding support for new devices).
std::wstring DumpHidCollections();
