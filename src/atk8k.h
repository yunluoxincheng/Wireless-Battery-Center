#pragma once

#include <windows.h>
#include <optional>
#include <string>
#include <vector>

#include "devices.h"

namespace atk8k {

inline constexpr unsigned short VendorId = 0x373B;
inline constexpr unsigned short ProductId = 0x101B;
inline constexpr wchar_t DeviceKey[] = L"kysona:373B:101B";

bool IsReceiverPath(const wchar_t* path);

class ActivityWatch {
public:
    bool Start(HWND window);
    void Stop(HWND window);
    bool RefreshDevices();
    bool IsActivity(HRAWINPUT input) const;
    bool Armed() const { return armed_; }

private:
    std::vector<HANDLE> devices_;
    bool armed_ = false;
};

// These run on the existing scan worker.
bool CanReadAfterActivity();
std::optional<DeviceStatus> ReadAfterActivity(ULONGLONG activityTick);
void ReceiverRemoved();

}  // namespace atk8k
