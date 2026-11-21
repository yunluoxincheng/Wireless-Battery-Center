#pragma once

#include <windows.h>
#include <string>

namespace panel {

// One UTF-8 line per connection: verb, optional tab + hex-encoded key, optional tab + value.
// The response is one newline-terminated UTF-8 JSON object. Both ends run in the same logon session.
inline constexpr size_t MaxRequest = 8192;
inline constexpr size_t MaxResponse = 4 * 1024 * 1024;

inline std::wstring PipePath() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\WirelessBatteryCenter.Panel.v1." + std::to_wstring(session);
}

}
