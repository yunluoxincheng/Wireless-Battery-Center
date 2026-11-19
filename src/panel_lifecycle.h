#pragma once

#include <windows.h>

namespace panel {

inline constexpr wchar_t BackgroundMutex[] = L"Local\\WirelessBatteryTray";
inline constexpr wchar_t BackgroundWindowClass[] = L"WirelessBatteryWnd";
inline constexpr wchar_t PanelMutex[] = L"Local\\WirelessBatteryPanel";
inline constexpr wchar_t PanelWindowProperty[] = L"WirelessBattery.Panel.MainWindow";

inline BOOL CALLBACK FindPanelWindowCallback(HWND window, LPARAM data) {
    if (!GetPropW(window, PanelWindowProperty)) return TRUE;
    *reinterpret_cast<HWND*>(data) = window;
    return FALSE;
}

inline HWND FindPanelWindow() {
    HWND result = nullptr;
    EnumWindows(FindPanelWindowCallback, reinterpret_cast<LPARAM>(&result));
    return result;
}

inline bool ActivatePanelWindow() {
    const HWND window = FindPanelWindow();
    if (!window) return false;
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    SetForegroundWindow(window);
    return true;
}

inline HWND FindBackgroundWindow() { return FindWindowW(BackgroundWindowClass, nullptr); }

inline void RequestClose(HWND window) {
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
}

}  // namespace panel
