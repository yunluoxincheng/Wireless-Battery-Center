#include "atk8k.h"

#include <algorithm>
#include <cwctype>

namespace atk8k {
namespace {

bool MouseRegistration(RAWINPUTDEVICE& mouse) {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(mouse)) == UINT(-1)) return false;
    if (count == 0) return true;
    std::vector<RAWINPUTDEVICE> devices(count);
    count = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(mouse));
    if (count == UINT(-1)) return false;
    for (UINT i = 0; i < count; ++i) {
        const auto& device = devices[i];
        if (device.usUsagePage == 1 &&
            (device.usUsage == 2 || (device.usUsage == 0 && (device.dwFlags & RIDEV_PAGEONLY)))) {
            mouse = device;
            break;
        }
    }
    return true;
}

}  // namespace

bool IsReceiverPath(const wchar_t* path) {
    std::wstring lower(path);
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    return lower.find(L"vid_373b&pid_101b&") != std::wstring::npos ||
           lower.find(L"vid_373b&pid_101b#") != std::wstring::npos;
}

bool ActivityWatch::RefreshDevices() {
    devices_.clear();
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1) || count == 0) return false;
    std::vector<RAWINPUTDEVICELIST> list(count);
    count = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (count == UINT(-1)) return false;
    for (UINT i = 0; i < count; ++i) {
        if (list[i].dwType != RIM_TYPEMOUSE) continue;
        wchar_t path[1024]{};
        UINT length = ARRAYSIZE(path);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path, &length) != UINT(-1) && IsReceiverPath(path))
            devices_.push_back(list[i].hDevice);
    }
    return !devices_.empty();
}

bool ActivityWatch::Start(HWND window) {
    RAWINPUTDEVICE existing{};
    // Windows gives this usage to only one window per process. Do not replace another registration.
    if (!MouseRegistration(existing) || existing.usUsagePage != 0 || !RefreshDevices()) return false;
    RAWINPUTDEVICE mouse{1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
    armed_ = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
    return armed_;
}

void ActivityWatch::Stop(HWND window) {
    if (!armed_) return;
    RAWINPUTDEVICE existing{};
    if (MouseRegistration(existing) && existing.hwndTarget == window) {
        RAWINPUTDEVICE mouse{1, 2, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    }
    armed_ = false;
    devices_.clear();
}

bool ActivityWatch::IsActivity(HRAWINPUT input) const {
    if (!armed_) return false;
    RAWINPUTHEADER header{};
    UINT size = sizeof(header);
    if (GetRawInputData(input, RID_HEADER, &header, &size, sizeof(header)) != sizeof(header) ||
        header.dwType != RIM_TYPEMOUSE ||
        std::find(devices_.begin(), devices_.end(), header.hDevice) == devices_.end()) return false;
    RAWINPUT packet{};
    size = sizeof(packet);
    return GetRawInputData(input, RID_INPUT, &packet, &size, sizeof(header)) != UINT(-1) &&
           (packet.data.mouse.lLastX || packet.data.mouse.lLastY || packet.data.mouse.usButtonFlags);
}

}  // namespace atk8k
