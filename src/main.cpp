// Wireless Battery - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "devices.h"
#include "device_profiles.h"
#include "i18n.h"
#include "labels.h"
#include "atk8k.h"
#include "panel_ipc.h"
#include "panel_lifecycle.h"
#include "product.h"
#include "settings.h"
#include "report.h"
#include "widget.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_PANEL_PIPE = WM_APP + 4;
constexpr UINT WM_ATK_RESULT = WM_APP + 5;
constexpr UINT_PTR TIMER_RESUME_REFRESH = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT_PTR TIMER_DEVICE_SETTLE = 3;
constexpr UINT_PTR TIMER_ATK_WAKE = 4;
constexpr UINT_PTR TIMER_ATK_LISTEN = 5;
constexpr UINT DEVICE_SETTLE_MS = 2500;
constexpr DWORD POLL_INTERVAL_MS = 60 * 1000;
constexpr UINT ATK_WAKE_DELAY_MS = 50;
constexpr UINT ATK_LISTEN_MS = 30 * 1000;
constexpr UINT ATK_RETRY_COOLDOWN_MS = 30 * 1000;
constexpr std::uint64_t READING_FRESH_MS = 180 * 1000;
constexpr ULONGLONG PROFILE_SAVE_INTERVAL_MS = 5 * 60 * 1000;
constexpr int DEFAULT_LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_EXPIRED_OR_ASLEEP, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_REPORT,
    ID_BRAND_COLORS, ID_STACKED, ID_PANEL,
    ID_LANG_AUTO = 400, ID_LANG_ADD, ID_LANG_FIRST,  // "Language": ID_LANG_FIRST + 0..63
    ID_MONITOR_ALL = 300, ID_MONITOR_MAIN, ID_MONITOR_FIRST,  // "Show the widget on": ID_MONITOR_FIRST + 0..31
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT { DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_STRIDE = 4 };

const wchar_t* kAppName = product::Name;
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = product::RunValue;
const wchar_t* kSettingsKey = product::SettingsKey;

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_atkWakeEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideExpiredOrAsleep = false;
std::wstring g_widgetMonitor = L"all";  // which taskbars show the widget: "all", "main" or one monitor id
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)
bool g_stacked = false;      // icon above the percentage (menu option)
bool g_brandColors = false;  // off by default; known brands get their color (Razer green, Logitech blue, ...)
bool g_demo = false;
bool g_deviceSettling = false;
int g_lowBattery = DEFAULT_LOW_BATTERY;
bool g_lowBatteryNotifications = true;
std::uint64_t g_pollRevision = 0;
std::atomic<std::uint64_t> g_requestedRefresh{0};
std::uint64_t g_completedRefresh = 0;

struct PollResult {
    std::uint64_t refresh;
    std::uint64_t atkEpoch;
    std::vector<DeviceStatus> devices;
    std::set<std::wstring> endpoints;
    bool atkActivityAvailable = false;
};

struct AtkResult {
    ULONGLONG activityTick;
    std::uint64_t epoch;
    std::optional<DeviceStatus> reading;
};

// The window owns Raw Input registration and timers. The worker owns the HID connection.
atk8k::ActivityWatch g_atkWatch;
std::atomic<ULONGLONG> g_atkWakeRequest{0};
std::atomic<std::uint64_t> g_atkEpoch{0};
std::atomic<bool> g_atkReceiverRemoved{false};
ULONGLONG g_atkActivityTick = 0;
ULONGLONG g_atkCooldownUntil = 0;
bool g_atkAwaitingPoll = true;
bool g_atkListenedThisPoll = false;
bool g_atkSuspended = false;
std::wstring g_atkEntryKey;

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    DeviceProfile profile;
    Presence presence = Presence::Offline;
    Accent brandAccent = Accent::None;
    ULONGLONG profileSaved = 0;
    bool profileDirty = false;
    bool profileWriteFailed = false;
    bool profileLive = false;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    std::wstring lastLevel;
    ULONGLONG lastSeen = 0;
    std::uint64_t lastReadingMs = 0;
    bool readingExpired = false;
    bool present = false;    // the original surfaces only display the current poll and its grace period
    bool added = false;
    bool lowWarned = false;
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
std::optional<ProfileStore> g_profiles;
bool g_profileLoadIssues = false;
std::wstring g_pendingRemoval;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Per-device choices are keyed by the identity supplied by the existing drivers.
std::set<std::wstring> g_hidden;
std::set<std::wstring> g_panelHidden;
std::map<std::wstring, std::wstring> g_aliases;
std::vector<std::wstring> g_order;
// The old preference key is retained only by the verified instance that adopted it.
std::map<std::wstring, std::wstring> g_preferences;

std::vector<std::wstring> ReadMultiSetting(const wchar_t* name) {
    std::vector<std::wstring> values;
    if (!::ReadMultiSetting(HKEY_CURRENT_USER, kSettingsKey, name, values)) g_profileLoadIssues = true;
    return values;
}

template<class Strings>
bool WriteMultiSetting(const wchar_t* name, const Strings& values) {
    return ::WriteMultiSetting(HKEY_CURRENT_USER, kSettingsKey, name, {values.begin(), values.end()});
}

const std::wstring& PreferenceKey(const std::wstring& identity) {
    const auto found = g_preferences.find(identity);
    return found == g_preferences.end() ? identity : found->second;
}

bool LegacyPreferenceBound(const std::wstring& legacy) {
    return std::any_of(g_preferences.begin(), g_preferences.end(), [&](const auto& pair) { return pair.second == legacy; });
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(PreferenceKey(key)) != 0; }

void BindLegacyPreference(const std::wstring& identity, const std::wstring& legacy) {
    if (identity == legacy || g_preferences.count(identity)) return;
    if (!g_aliases.count(legacy) && !g_hidden.count(legacy) && !g_panelHidden.count(legacy) &&
        std::find(g_order.begin(), g_order.end(), legacy) == g_order.end()) return;
    if (LegacyPreferenceBound(legacy)) return;
    auto updated = g_preferences;
    updated.emplace(identity, legacy);
    std::vector<std::wstring> values;
    for (const auto& [key, preference] : updated) {
        values.push_back(key);
        values.push_back(preference);
    }
    if (WriteMultiSetting(L"PanelLegacyIdentities", values)) g_preferences.swap(updated);
    else g_profileLoadIssues = true;
}

void LoadDevicePreferences() {
    auto hidden = ReadMultiSetting(L"HiddenDevices");
    g_hidden.clear();
    g_hidden.insert(hidden.begin(), hidden.end());
    auto panelHidden = ReadMultiSetting(L"PanelHiddenDevices");
    g_panelHidden.clear();
    g_panelHidden.insert(panelHidden.begin(), panelHidden.end());
    g_order = ReadMultiSetting(L"DeviceOrder");
    auto aliases = ReadMultiSetting(L"DeviceAliases");
    g_aliases.clear();
    for (size_t i = 1; i < aliases.size(); i += 2) g_aliases[aliases[i - 1]] = aliases[i];
    auto bindings = ReadMultiSetting(L"PanelLegacyIdentities");
    g_preferences.clear();
    for (size_t i = 1; i < bindings.size(); i += 2) g_preferences.emplace(bindings[i - 1], bindings[i]);
}

void LoadDeviceSettings() {
    LoadDevicePreferences();
    DWORD threshold = DEFAULT_LOW_BATTERY, size = sizeof(threshold);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"LowBatteryPercent", RRF_RT_REG_DWORD, nullptr, &threshold, &size)
        == ERROR_SUCCESS && threshold >= 1 && threshold <= 100)
        g_lowBattery = static_cast<int>(threshold);
}

const std::wstring& DisplayName(const DeviceStatus& s) {
    const auto it = g_aliases.find(PreferenceKey(s.key));
    return it == g_aliases.end() ? s.name : it->second;
}

int ReadingPercent(const TrayEntry& e) { return e.st.percent >= 0 ? e.st.percent : e.lastPct; }

const std::wstring& ReadingLevel(const TrayEntry& e) { return e.st.percent >= 0 ? e.st.levelText : e.lastLevel; }

bool IsReadingCached(const TrayEntry& e) {
    const auto& s = e.st;
    return ReadingPercent(e) >= 0 && (!s.connected || s.asleep || s.cached || s.percent < 0);
}

bool IsCharging(const TrayEntry& e) {
    return e.st.connected && !e.st.asleep && !e.st.cached && e.st.charging;
}

bool IsReadingDimmed(const TrayEntry& e) { return e.st.asleep || e.readingExpired || ReadingPercent(e) < 0; }

bool IsEntryHidden(const TrayEntry& e) {
    return IsHidden(e.st.key) || (g_hideExpiredOrAsleep && (e.st.asleep || e.readingExpired));
}

bool IsShown(const TrayEntry& e) { return e.present && !e.st.panelOnly && e.st.connected && !IsEntryHidden(e); }

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

bool SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
bool LoadHideExpiredOrAsleep() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideExpiredOrAsleep(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }
std::wstring LoadWidgetMonitor() {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
        return buf;
    return LoadBoolSetting(L"OtherTaskbars", true) ? L"all" : L"main";  // the earlier on/off toggle
}
void SaveWidgetMonitor(const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void SetWidgetMonitor(const std::wstring& v) {
    g_widgetMonitor = v;
    SaveWidgetMonitor(v);
    widget::SetMonitorChoice(v);
}
bool LoadStacked() { return LoadBoolSetting(L"StackedLayout", false); }
void SaveStacked(bool on) { SaveBoolSetting(L"StackedLayout", on); }
bool LoadBrandColors() { return LoadBoolSetting(L"BrandColors", false); }
void SaveBrandColors(bool on) { SaveBoolSetting(L"BrandColors", on); }

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

Accent AccentFor(const TrayEntry& e) { return g_brandColors ? e.brandAccent : Accent::None; }

HICON IconFor(int pct, bool charging, bool dim, Accent accent) {
    Rgb fg = Foreground();
    BrandColor bc = AccentColor(accent, g_lightTaskbar);
    Rgb bar = dim             ? fg
            : charging        ? Rgb{ 0, 170, 255 }
            : pct <= g_lowBattery ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
            : accent != Accent::None ? Rgb{ bc.r, bc.g, bc.b }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!dim && !charging && pct >= 0 && pct <= g_lowBattery) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return T(L"off / not connected");
    std::wstring t;
    const int percent = ReadingPercent(e);
    const auto& level = ReadingLevel(e);
    if (percent >= 0 && !level.empty()) t = T(level);
    else if (percent >= 0) t = std::to_wstring(percent) + L"%";
    else t = T(L"unknown");
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (IsCharging(e)) t += std::wstring(L", ") + T(L"charging");
    if (s.asleep) t += std::wstring(L", ") + T(L"asleep");
    if (s.wirelessLink == WirelessLink::Unlinked || s.wirelessLink == WirelessLink::Unknown)
        t += std::wstring(L", ") + T(L"link unconfirmed");
    if (IsReadingCached(e) || e.readingExpired) t += std::wstring(L", ") + T(L"last known");
    if (e.readingExpired) t += std::wstring(L", ") + T(L"reading expired");
    return t;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, std::wstring(kAppName) + L"\n" + T(L"No devices with battery info found"));
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    const bool visible = !g_useWidget && IsShown(e);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    const int shown = ReadingPercent(e);
    const bool dim = IsReadingDimmed(e);
    const bool charging = IsCharging(e);
    Accent accent = AccentFor(e);
    std::wstring sig = std::to_wstring(shown) + (charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k") +
                       L"a" + std::to_wstring((int)accent);
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = IconFor(shown, charging, dim, accent);
        e.iconSig = sig;
    }
    std::wstring tip = DisplayName(s) + L"\n" + StatusText(e);

    const wchar_t* infoTitle = nullptr;
    std::wstring info;
    const bool live = !dim && !IsReadingCached(e);
    if (charging || (live && shown > g_lowBattery + 10)) e.lowWarned = false;
    if (g_lowBatteryNotifications && live && !charging && shown <= g_lowBattery && !e.lowWarned) {
        e.lowWarned = true;
        infoTitle = T(L"Low battery");
        info = TF(L"{name} is at {percent}%", { { L"name", DisplayName(s) }, { L"percent", std::to_wstring(shown) } });
    }
    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip, infoTitle, info);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly, bool includeOffline = false) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (shownOnly ? IsShown(e) : (includeOffline || (e.present && !e.st.panelOnly))) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), PreferenceKey(e->st.key));
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        if (ka != kb) return ka < kb;
        return a->st.name != b->st.name ? a->st.name < b->st.name : a->st.key < b->st.key;  // same name: keep a fixed order
    });
    return list;
}

void UpdateWidget() {
    if (!g_useWidget) return;
    std::vector<TrayEntry*> list = OrderedEntries(true);
    // Two devices of one kind share an icon: give each a tiny caption so they can be told apart.
    std::vector<LabelInput> labelIn;
    for (auto* e : list) labelIn.push_back({ e->st.kind, DisplayName(e->st) });
    std::vector<std::wstring> labels = ShortLabels(labelIn, 3);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        it.percent = ReadingPercent(*e);
        it.text = T(ReadingLevel(*e));
        it.charging = !e->readingExpired && IsCharging(*e);
        it.dim = IsReadingDimmed(*e);
        it.low = !it.dim && !it.charging && it.percent <= g_lowBattery;
        it.bluetooth = e->st.via == L"Bluetooth";
        it.label = labels[items.size()];
        it.accent = it.dim ? Accent::None : AccentFor(*e);
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += DisplayName(e->st) + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = std::wstring(kAppName) + L": " + T(L"No devices with battery info found");
    widget::SetItems(items, tip);
}

std::uint64_t UnixTimeMs() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER now{};
    now.LowPart = fileTime.dwLowDateTime;
    now.HighPart = fileTime.dwHighDateTime;
    constexpr ULONGLONG unixEpoch = 116444736000000000ull;
    return (now.QuadPart - unixEpoch) / 10000;
}

bool UpdateReadingExpiry(TrayEntry& e, std::uint64_t nowMs) {
    const bool expired = e.lastReadingMs && ReadingPercent(e) >= 0 && nowMs > e.lastReadingMs
                      && nowMs - e.lastReadingMs > READING_FRESH_MS;
    const bool changed = e.readingExpired != expired;
    e.readingExpired = expired;
    return changed;
}

void RefreshReadingExpiry() {
    // The existing UI timer advances freshness even when the hardware scan is still pending.
    const auto nowMs = UnixTimeMs();
    bool changed = false;
    for (auto& [key, e] : g_entries) {
        if (UpdateReadingExpiry(e, nowMs)) {
            ApplyEntry(e);
            changed = true;
        }
    }
    if (changed) {
        UpdatePlaceholder();
        UpdateWidget();
    }
}

void UpdateEntry(TrayEntry& e, const DeviceStatus& s, ULONGLONG tick, std::uint64_t nowMs) {
    e.present = true;
    e.st = s;
    if (s.percent >= 0) {
        e.lastPct = s.percent;
        e.lastLevel = s.levelText;
        if (s.readingTimeMs) {
            // A BLE advertisement or system snapshot may be returned by several polls.
            if (e.lastReadingMs && s.readingTimeMs <= e.lastReadingMs + 32) e.st.cached = true;
            else e.lastReadingMs = s.readingTimeMs;
        }
        else if (s.connected && !s.asleep && !s.cached) e.lastReadingMs = nowMs;
    }
    e.lastSeen = tick;
    UpdateReadingExpiry(e, nowMs);
    ApplyEntry(e);
}

std::wstring Utf16(std::string_view value);

void SaveProfile(TrayEntry& e, ULONGLONG tick, bool immediate) {
    if (!e.profileDirty || (!immediate && tick - e.profileSaved < PROFILE_SAVE_INTERVAL_MS)) return;
    e.profileSaved = tick;
    e.profileWriteFailed = !g_profiles->Save(e.st.key, e.profile);
    e.profileDirty = e.profileWriteFailed;
}

void ObserveEntry(TrayEntry& e, DeviceStatus s, const std::wstring& key, ULONGLONG tick, std::uint64_t now) {
    e.brandAccent = BrandOf(s);
    std::wstring brand;
    if (e.brandAccent != Accent::None) brand = StyleOf(e.brandAccent).name;
    else if (s.name == L"VGN V98Pro V2") brand = L"VGN";
    else if (s.vid == 0x373B || s.name == L"ATK V75X") brand = L"ATK";
    else if (s.vid == 0x35BB) brand = L"HECATE";
    else if (s.vid == 0x413C) brand = L"Dell";
    s.key = key;
    UpdateEntry(e, s, tick, now);
    const auto previousPresence = e.presence;
    const auto online = e.profile.lastOnline;
    const auto reading = e.profile.readingTime;
    e.presence = ObservedPresence(e.st);
    const bool changed = ObserveProfile(e.profile, e.st, e.presence, brand,
                                        s.protocol ? Utf16(s.protocol) : L"", now);
    const bool newReading = e.profile.readingTime != reading;
    e.profileLive = newReading && !s.systemBatterySnapshot;
    if (s.identity.empty()) return;
    e.profileDirty |= changed || e.profile.lastOnline != online || newReading;
    SaveProfile(e, tick, changed || previousPresence != e.presence);
}

void LoadProfiles() {
    g_profiles.emplace(kSettingsKey);
    for (auto& [key, profile] : g_profiles->Load(g_profileLoadIssues)) {
        auto& e = g_entries[key];
        e.uid = StableId(key);
        e.st.key = e.st.identity = key;
        e.st.name = profile.name;
        e.st.via = profile.via;
        e.st.kind = profile.kind;
        e.st.vid = profile.vid;
        e.st.pid = profile.pid;
        e.st.connected = false;
        e.st.panelOnly = profile.panelOnly;
        e.profile = std::move(profile);
    }
}

void OnResults(const std::vector<DeviceStatus>& results, const std::set<std::wstring>& endpoints) {
    const ULONGLONG tick = GetTickCount64();
    const auto nowMs = UnixTimeMs();
    std::map<std::wstring, size_t> counts;
    for (const auto& s : results) ++counts[s.key];
    std::set<std::wstring> seen;
    g_atkEntryKey.clear();
    for (const auto& s : results) {
        std::wstring key = s.identity.empty() ? s.key : s.identity;
        if (s.identity.empty() && LegacyPreferenceBound(key)) key = L"session:" + key;
        if (key == g_pendingRemoval) continue;
        if (s.identity.empty() && (g_entries.count(key) && !g_entries.at(key).st.identity.empty())) continue;
        if (!seen.insert(key).second) continue;
        if (ObservedPresence(s) == Presence::Offline && !g_entries.count(key)) continue;
        if (!s.identity.empty() && counts[s.key] == 1 && ObservedPresence(s) == Presence::Online)
            BindLegacyPreference(key, s.key);
        const auto [entry, inserted] = g_entries.try_emplace(key);
        auto& e = entry->second;
        if (inserted) e.uid = StableId(key);
        ObserveEntry(e, s, key, tick, nowMs);
        if (s.key == atk8k::DeviceKey) g_atkEntryKey = key;
    }
    for (auto& [key, e] : g_entries) {
        if (seen.count(key)) continue;
        e.profileLive = false;
        const auto previousPresence = e.presence;
        e.presence = endpoints.count(key) ? Presence::Unknown : Presence::Offline;
        SaveProfile(e, tick, previousPresence != e.presence);
        if (!e.present) continue;
        UpdateReadingExpiry(e, nowMs);
        // A missed reading can also happen during hotplug or a timeout. Keep the cached level without inferring sleep.
        const bool inGrace = e.st.graceSec > 0 && e.st.connected &&
                             tick - e.lastSeen < (ULONGLONG)e.st.graceSec * 1000;
        if (inGrace) {
            e.st.percent = -1;
            e.st.charging = false;
            ApplyEntry(e);
        } else {
            e.present = false;
            e.st.connected = false;
            e.st.percent = -1;
            ApplyEntry(e);
            if (e.icon) DestroyIcon(e.icon);
            e.icon = nullptr;
            e.lowWarned = false;
        }
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (!it->second.present && it->second.st.identity.empty()) it = g_entries.erase(it);
        else ++it;
    }
    ++g_pollRevision;
    UpdatePlaceholder();
    UpdateWidget();
}

void StopAtkListener() {
    KillTimer(g_hwnd, TIMER_ATK_LISTEN);
    g_atkWatch.Stop(g_hwnd);
}

void CancelAtkActivity() {
    StopAtkListener();
    KillTimer(g_hwnd, TIMER_ATK_WAKE);
    g_atkActivityTick = 0;
    g_atkWakeRequest = 0;
}

void InvalidateAtkActivity(bool removed) {
    if (removed) g_atkReceiverRemoved = true;
    ++g_atkEpoch;
    CancelAtkActivity();
    g_atkAwaitingPoll = true;
    g_atkCooldownUntil = 0;
}

void MaybeListenForAtkActivity() {
    if (!g_atkWakeEvt || g_demo || g_atkSuspended || g_atkAwaitingPoll || g_atkActivityTick || g_atkWatch.Armed() ||
        g_atkListenedThisPoll || GetTickCount64() < g_atkCooldownUntil) return;
    auto it = g_entries.find(g_atkEntryKey);
    if (it == g_entries.end() || !it->second.present || !it->second.st.connected || it->second.st.asleep) return;

    const auto& e = it->second;
    if (e.st.wirelessLink == WirelessLink::Linked && e.st.percent >= 0 && !e.st.cached &&
        UnixTimeMs() < e.lastReadingMs + POLL_INTERVAL_MS) return;

    g_atkListenedThisPoll = true;
    if (g_atkWatch.Start(g_hwnd) && !SetTimer(g_hwnd, TIMER_ATK_LISTEN, ATK_LISTEN_MS, nullptr))
        StopAtkListener();
}

void OnAtkActivity(HRAWINPUT input) {
    if (!g_atkWatch.IsActivity(input)) return;
    g_atkActivityTick = GetTickCount64();
    StopAtkListener();
    if (!SetTimer(g_hwnd, TIMER_ATK_WAKE, ATK_WAKE_DELAY_MS, nullptr))
        g_atkActivityTick = 0;
}

void OnAtkResult(const AtkResult& result) {
    if (result.epoch != g_atkEpoch.load() || result.activityTick != g_atkActivityTick) return;
    g_atkActivityTick = 0;
    if (!result.reading) {
        g_atkCooldownUntil = GetTickCount64() + ATK_RETRY_COOLDOWN_MS;
        g_atkListenedThisPoll = false;
        return;
    }
    auto it = g_entries.find(g_atkEntryKey);
    if (it == g_entries.end() || !it->second.present || !it->second.st.connected) return;
    ObserveEntry(it->second, *result.reading, it->first, GetTickCount64(), UnixTimeMs());
    UpdatePlaceholder();
    UpdateWidget();
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

bool SetMode(bool useWidget) {
    if (!SaveBoolSetting(L"TaskbarWidget", useWidget)) return false;
    g_wantWidget = useWidget;
    Reconcile();
    return true;
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetHideExpiredOrAsleep(bool on) {
    g_hideExpiredOrAsleep = on;
    SaveHideExpiredOrAsleep(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

// Switches the UI language ("" = follow Windows) and redraws everything that shows text.
void SetLanguage(const std::wstring& code);

void RedrawAll() {
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void SetLanguage(const std::wstring& code) {
    i18n::SetChoice(code);
    RedrawAll();  // tray tooltips and the placeholder text
    UpdateWidget();
}

void SetStacked(bool on) {
    g_stacked = on;
    SaveStacked(on);
    widget::SetStacked(on);
}

void SetBrandColors(bool on) {
    g_brandColors = on;
    SaveBrandColors(on);
    RedrawAll();
    UpdateWidget();
}

bool SetHidden(const std::wstring& key, bool hidden, bool panelOnly) {
    auto& target = panelOnly ? g_panelHidden : g_hidden;
    auto updated = target;
    if (hidden) updated.insert(PreferenceKey(key));
    else updated.erase(PreferenceKey(key));
    if (!WriteMultiSetting(panelOnly ? L"PanelHiddenDevices" : L"HiddenDevices", updated)) return false;
    target.swap(updated);
    if (panelOnly) return true;
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
    return true;
}

// Moves within the initiating surface, leaving filtered devices in their original slots.
bool MoveEntry(const std::wstring& key, int dir, bool panelOrder = false, bool includeHidden = false,
               const std::wstring& target = {}) {
    auto all = OrderedEntries(false, true);
    std::vector<TrayEntry**> slots;
    for (auto*& entry : all) {
        if (panelOrder ? (includeHidden || !g_panelHidden.count(PreferenceKey(entry->st.key))) : IsShown(*entry))
            slots.push_back(&entry);
    }
    const auto indexOf = [&](const std::wstring& deviceKey) {
        const auto found = std::find_if(slots.begin(), slots.end(), [&](TrayEntry** slot) {
            return (*slot)->st.key == deviceKey;
        });
        return found - slots.begin();
    };
    auto from = indexOf(key);
    const auto count = static_cast<ptrdiff_t>(slots.size());
    if (from == count) return false;
    const auto to = target.empty() ? from + dir : indexOf(target);
    if (to < 0 || to >= count) return false;
    if (from == to) return true;
    const int step = from < to ? 1 : -1;
    auto* moving = *slots[from];
    for (; from != to; from += step) *slots[from] = *slots[from + step];
    *slots[to] = moving;
    std::vector<std::wstring> order;
    for (auto* e : all) order.push_back(PreferenceKey(e->st.key));
    const size_t limit = std::max<size_t>(64, order.size());
    for (const auto& k : g_order) {
        if (order.size() >= limit) break;
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    }
    if (!WriteMultiSetting(L"DeviceOrder", order)) return false;
    g_order.swap(order);
    UpdateWidget();
    return true;
}

bool SetAlias(const std::wstring& key, const std::wstring& name) {
    auto updated = g_aliases;
    if (name.empty()) updated.erase(PreferenceKey(key));
    else updated[PreferenceKey(key)] = name;
    std::vector<std::wstring> values;
    for (const auto& [deviceKey, alias] : updated) {
        values.push_back(deviceKey);
        values.push_back(alias);
    }
    if (!WriteMultiSetting(L"DeviceAliases", values)) return false;
    g_aliases.swap(updated);
    ApplyEntry(g_entries.at(key));
    UpdateWidget();
    return true;
}

bool SetLowBattery(int threshold) {
    if (threshold == g_lowBattery) return true;
    const DWORD value = static_cast<DWORD>(threshold);
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"LowBatteryPercent", REG_DWORD,
                        &value, sizeof(value)) != ERROR_SUCCESS) return false;
    g_lowBattery = threshold;
    for (auto& [key, entry] : g_entries) {
        if (entry.st.percent > threshold) entry.lowWarned = false;
    }
    RedrawAll();
    UpdateWidget();
    return true;
}

bool SetLowBatteryNotifications(bool enabled) {
    if (!SaveBoolSetting(L"LowBatteryNotifications", enabled)) return false;
    g_lowBatteryNotifications = enabled;
    return true;
}

// ---- device panel -----------------------------------------------------------------------------

std::wstring Utf16(std::string_view value) {
    const int inputSize = static_cast<int>(value.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputSize, nullptr, 0);
    if (!size) return {};
    std::wstring decoded(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputSize, decoded.data(), size);
    return decoded;
}

std::string JsonString(const std::wstring& value) {
    const int inputSize = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), inputSize, nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(), inputSize, utf8.data(), size, nullptr, nullptr);
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char ch : utf8) {
        if (ch < 32) {
            result += "\\u00";
            result += digits[ch >> 4];
            result += digits[ch & 0xF];
        } else {
            if (ch == '"' || ch == '\\') result += '\\';
            result += static_cast<char>(ch);
        }
    }
    return result + "\"";
}

std::wstring DecodePanelKey(std::string_view hex) {
    if (hex.size() % 2 != 0) return {};
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    constexpr std::string_view digits = "0123456789abcdef";
    for (size_t i = 0; i < hex.size(); i += 2) {
        const auto high = digits.find(hex[i]), low = digits.find(hex[i + 1]);
        if (high == digits.npos || low == digits.npos) return {};
        bytes += static_cast<char>(high * 16 + low);
    }
    return Utf16(bytes);
}

const char* JsonBool(bool value) { return value ? "true" : "false"; }

std::string PanelSnapshot(bool ok) {
    const auto now = UnixTimeMs();
    std::string json = "{\"ok\":";
    json += JsonBool(ok);
    json += ",\"poll\":" + std::to_string(g_pollRevision);
    json += ",\"refreshRequested\":" + std::to_string(g_requestedRefresh.load());
    json += ",\"refreshCompleted\":" + std::to_string(g_completedRefresh);
    json += ",\"threshold\":" + std::to_string(g_lowBattery);
    json += ",\"lowBatteryNotifications\":" + std::string(JsonBool(g_lowBatteryNotifications));
    json += ",\"demo\":" + std::string(JsonBool(g_demo));
    json += ",\"widget\":" + std::string(JsonBool(g_wantWidget));
    json += ",\"widgetAvailable\":" + std::string(JsonBool(g_useWidget));
    json += ",\"profileError\":" + std::string(JsonBool(g_profileLoadIssues || std::any_of(
        g_entries.begin(), g_entries.end(), [](const auto& pair) { return pair.second.profileWriteFailed; })));
    json += ",\"devices\":[";
    bool first = true;
    for (const auto* e : OrderedEntries(false, true)) {
        const auto& s = e->st;
        if (!first) json += ',';
        first = false;
        const auto& p = e->profile;
        const bool offline = e->presence == Presence::Offline;
        const bool online = e->presence == Presence::Online;
        const bool expired = p.readingTime && now > p.readingTime && now - p.readingTime > READING_FRESH_MS;
        json += "{\"key\":" + JsonString(s.key);
        json += ",\"name\":" + JsonString(s.name);
        json += ",\"displayName\":" + JsonString(DisplayName(s));
        const auto alias = g_aliases.find(PreferenceKey(s.key));
        json += ",\"alias\":" + JsonString(alias == g_aliases.end() ? L"" : alias->second);
        json += ",\"kind\":" + std::to_string(static_cast<int>(s.kind));
        json += ",\"via\":" + JsonString(s.via);
        json += ",\"percent\":" + std::to_string(p.percent);
        json += ",\"level\":" + JsonString(p.level);
        json += ",\"brand\":" + JsonString(p.brand);
        json += ",\"batteryCapability\":" + std::to_string(static_cast<int>(p.battery));
        json += ",\"offline\":" + std::string(JsonBool(offline));
        json += ",\"presence\":\"" + std::string(online ? "online" : offline ? "offline" : "unknown") + '"';
        json += ",\"removable\":" + std::string(JsonBool(g_pollRevision && offline && !s.identity.empty() && !e->profileWriteFailed));
        if (s.wirelessLink != WirelessLink::Unreported) {
            const char* link = "unknown";
            if (!offline && s.wirelessLink == WirelessLink::Linked) link = "linked";
            if (!offline && s.wirelessLink == WirelessLink::Unlinked) link = "unlinked";
            json += ",\"receiverPresent\":" + std::string(JsonBool(e->present && !offline && s.via == L"2.4 GHz receiver"));
            json += ",\"wirelessLink\":\"" + std::string(link) + '"';
        }
        json += ",\"asleep\":" + std::string(JsonBool(e->present && !offline && s.asleep));
        json += ",\"cached\":" + std::string(JsonBool(p.percent >= 0 && (!online || !e->profileLive)));
        json += ",\"expired\":" + std::string(JsonBool(expired));
        json += ",\"charging\":" + std::string(JsonBool(online && e->profileLive && !expired && IsCharging(*e)));
        json += ",\"updated\":" + std::to_string(p.readingTime);
        json += ",\"readingSource\":" + JsonString(p.readingSource);
        json += ",\"firstOnline\":" + std::to_string(p.firstOnline);
        json += ",\"lastOnline\":" + std::to_string(p.lastOnline);
        json += ",\"hidden\":" + std::string(JsonBool(g_panelHidden.count(PreferenceKey(s.key)) != 0));
        json += ",\"surfaceSupported\":" + std::string(JsonBool(!s.panelOnly));
        json += ",\"surface\":" + std::string(JsonBool(!IsHidden(s.key)));
        json += ",\"vid\":" + std::to_string(s.vid);
        json += ",\"pid\":" + std::to_string(s.pid);
        json += ",\"protocol\":" + JsonString(p.protocol);
        json += '}';
    }
    return json + "]}";
}

bool ApplyPanelCommand(std::string_view request) {
    if (request == "snapshot") return true;
    if (request == "refresh") {
        ++g_requestedRefresh;
        return SetEvent(g_refreshEvt) != FALSE;
    }
    const auto first = request.find('\t');
    if (first == request.npos) return false;
    const auto second = request.find('\t', first + 1);
    if (second == request.npos) return false;
    const auto verb = request.substr(0, first);
    const auto hexKey = request.substr(first + 1, second - first - 1);
    const auto value = request.substr(second + 1);
    if (hexKey.empty() && verb == "threshold") {
        if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != value.npos) return false;
        int threshold = 0;
        for (char digit : value) threshold = threshold * 10 + digit - '0';
        return threshold >= 1 && threshold <= 100 && SetLowBattery(threshold);
    }
    if (hexKey.empty() && verb == "widget") return (value == "0" || value == "1") && SetMode(value == "1");
    if (hexKey.empty() && verb == "lowBatteryNotifications")
        return (value == "0" || value == "1") && SetLowBatteryNotifications(value == "1");
    const auto key = DecodePanelKey(hexKey);
    if (key.empty() || !g_entries.count(key)) return false;
    if (verb == "forget") {
        auto it = g_entries.find(key);
        auto& e = it->second;
        if (!value.empty() || !g_pollRevision || e.presence != Presence::Offline || e.st.identity.empty() || e.profileWriteFailed)
            return false;
        const bool removed = ForgetDeviceProfile(kSettingsKey, key, PreferenceKey(key), g_pendingRemoval);
        LoadDevicePreferences();
        if (!removed) return false;
        if (e.added) TrayDelete(e.uid);
        if (e.icon) DestroyIcon(e.icon);
        g_entries.erase(it);
        UpdatePlaceholder();
        UpdateWidget();
        return true;
    }
    if (verb == "hidden" || verb == "surface") {
        if (value != "0" && value != "1") return false;
        if (verb == "surface" && g_entries.at(key).st.panelOnly) return false;
        return SetHidden(key, verb == "hidden" ? value == "1" : value == "0", verb == "hidden");
    }
    if (verb == "move") {
        if (value == "up" || value == "down") return MoveEntry(key, value == "up" ? -1 : 1, true);
        if (value == "upAll" || value == "downAll") return MoveEntry(key, value == "upAll" ? -1 : 1, true, true);
    }
    if (verb == "moveTo" || verb == "moveToAll") {
        const auto target = DecodePanelKey(value);
        return !target.empty() && MoveEntry(key, 0, true, verb == "moveToAll", target);
    }
    if (verb == "name") {
        auto name = Utf16(value);
        if ((!value.empty() && name.empty()) || name.size() > 80 ||
            std::any_of(name.begin(), name.end(), [](wchar_t c) { return iswcntrl(c) != 0; })) return false;
        const auto start = name.find_first_not_of(L" ");
        if (start == name.npos) return SetAlias(key, L"");
        return SetAlias(key, name.substr(start, name.find_last_not_of(L" ") - start + 1));
    }
    return false;
}

std::string HandlePanelRequest(std::string_view request) { return PanelSnapshot(ApplyPanelCommand(request)); }

void OpenPanel() {
    if (panel::ActivatePanelWindow()) return;
    const auto exe = ExePath();
    const auto dir = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    const auto path = dir + product::PanelExe;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(path.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return;
    }
    MessageBoxW(g_hwnd, T(L"Install WirelessBatteryCenterPanel.exe and its Qt runtime next to WirelessBatteryCenter.exe."),
                T(L"Open device manager"), MB_OK | MB_ICONINFORMATION);
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_PANEL, T(L"Open device manager"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"No devices with battery info found"));
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"Point at a device to show, hide or move it"));
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? T(L"Show on the taskbar") : T(L"Show in the tray"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? T(L"Move left") : T(L"Move up"));
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? T(L"Move right") : T(L"Move down"));
        std::wstring line = DisplayName(e->st) + L":  " + StatusText(*e) + L"   [" + T(e->st.via) + L"]";
        for (size_t amp = line.find(L'&'); amp != line.npos; amp = line.find(L'&', amp + 2))
            line.insert(amp, 1, L'&');
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_STRING | (g_hideExpiredOrAsleep ? MF_CHECKED : 0), ID_HIDE_EXPIRED_OR_ASLEEP,
                T(L"Hide devices that are asleep or have expired readings"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wantWidget ? MF_CHECKED : 0), ID_MODE_WIDGET, T(L"Show on the taskbar (icon + percentage)"));
    AppendMenuW(m, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, T(L"Show as tray icons"));
    CheckMenuRadioItem(m, ID_MODE_WIDGET, ID_MODE_TRAY, g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);
    UINT bgFlags = MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(m, bgFlags, ID_WIDGET_BACKGROUND, T(L"Background behind taskbar text (for transparent taskbars)"));
    AppendMenuW(m, MF_STRING | (g_stacked ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_STACKED, T(L"Icons above percentages (narrower)"));
    // With more than one monitor: choose which taskbars show the widget (all, main only, or one monitor).
    auto monitors = widget::OtherMonitors();
    if (monitors.size() > 31) monitors.resize(31);
    if (!monitors.empty()) {
        HMENU sub = CreatePopupMenu();
        UINT f = MF_STRING | (g_wantWidget ? 0 : MF_GRAYED);
        AppendMenuW(sub, f, ID_MONITOR_ALL, T(L"All monitors"));
        AppendMenuW(sub, f, ID_MONITOR_MAIN, T(L"Main monitor only"));
        UINT checked = g_widgetMonitor == L"main" ? ID_MONITOR_MAIN : ID_MONITOR_ALL;
        bool found = g_widgetMonitor == L"all" || g_widgetMonitor == L"main";
        for (UINT i = 0; i < monitors.size(); i++) {
            AppendMenuW(sub, f, ID_MONITOR_FIRST + i, TF(L"{monitor} only", { { L"monitor", monitors[i].label } }).c_str());
            if (monitors[i].id == g_widgetMonitor) { checked = ID_MONITOR_FIRST + i; found = true; }
        }
        if (!found) {  // the chosen monitor is unplugged: the main taskbar shows the widget meanwhile
            AppendMenuW(sub, MF_STRING | MF_GRAYED, ID_MONITOR_FIRST + 31, T(L"Chosen monitor (not connected, shown on main)"));
            checked = ID_MONITOR_FIRST + 31;
        }
        CheckMenuRadioItem(sub, ID_MONITOR_ALL, ID_MONITOR_FIRST + 31, checked, MF_BYCOMMAND);
        AppendMenuW(m, MF_POPUP | (g_wantWidget ? 0 : MF_GRAYED), (UINT_PTR)sub, T(L"Show the widget on"));
    }
    AppendMenuW(m, MF_STRING | (g_brandColors ? MF_CHECKED : 0), ID_BRAND_COLORS, T(L"Brand colors (Razer green, Logitech blue, ...)"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Language: automatic, every built-in or user-added language (in its own name), and a way to add one.
    i18n::Init();  // picks up language files added since the last time
    auto languages = i18n::Available();
    if (languages.size() > 64) languages.resize(64);
    {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, ID_LANG_AUTO, T(L"Automatic (Windows language)"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        UINT checked = ID_LANG_AUTO;
        for (UINT i = 0; i < languages.size(); i++) {
            AppendMenuW(sub, MF_STRING, ID_LANG_FIRST + i, languages[i].name.c_str());
            if (!i18n::Choice().empty() && _wcsicmp(languages[i].code.c_str(), i18n::Choice().c_str()) == 0) checked = ID_LANG_FIRST + i;
        }
        CheckMenuRadioItem(sub, ID_LANG_AUTO, ID_LANG_FIRST + 63, checked, MF_BYCOMMAND);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, ID_LANG_ADD, T(L"Add your own language..."));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Language"));
    }
    AppendMenuW(m, MF_STRING, ID_REFRESH, T(L"Refresh now"));
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, T(L"Start with Windows"));
    AppendMenuW(m, MF_STRING, ID_REPORT, T(L"Request support for a device..."));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, T(L"Exit"));
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | (i18n::Rtl() ? TPM_LAYOUTRTL : 0), pt.x, pt.y, 0,
                              g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_PANEL: OpenPanel(); break;
        case ID_REFRESH: ApplyPanelCommand("refresh"); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_EXPIRED_OR_ASLEEP: SetHideExpiredOrAsleep(!g_hideExpiredOrAsleep); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MONITOR_ALL: SetWidgetMonitor(L"all"); break;
        case ID_LANG_AUTO: SetLanguage(L""); break;
        case ID_LANG_ADD: i18n::OpenUserFolder(); break;
        case ID_MONITOR_MAIN: SetWidgetMonitor(L"main"); break;
        case ID_BRAND_COLORS: SetBrandColors(!g_brandColors); break;
        case ID_STACKED: SetStacked(!g_stacked); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_REPORT: report::Show(g_inst, g_hwnd); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_LANG_FIRST && cmd < ID_LANG_FIRST + languages.size()) {
                SetLanguage(languages[cmd - ID_LANG_FIRST].code);
            } else if (cmd >= ID_MONITOR_FIRST && cmd < ID_MONITOR_FIRST + monitors.size()) {
                SetWidgetMonitor(monitors[cmd - ID_MONITOR_FIRST].id);
            } else if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: SetHidden(key, !IsHidden(key), false); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                }
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

// `--demo`: one sample device of every brand instead of real ones, for screenshots and for checking the
// brand colors without owning the hardware. Keys, vendor ids and names are mixed on purpose, so every way
// of recognising a brand gets used.
std::vector<DeviceStatus> DemoDevices() {
    auto make = [](const wchar_t* key, const wchar_t* name, DeviceKind kind, int pct, unsigned short vid, bool charging) {
        DeviceStatus s;
        s.key = key;
        s.name = name;
        s.kind = kind;
        s.percent = pct;
        s.vid = vid;
        s.charging = charging;
        s.via = L"demo";
        return s;
    };
    std::vector<DeviceStatus> devices = {
        make(L"razer:demo", L"Razer DeathAdder V3 Pro", DeviceKind::Mouse, 78, 0x1532, false),
        make(L"demo:logi", L"Logitech G PRO X 2 LIGHTSPEED", DeviceKind::Headset, 64, 0x046D, false),
        make(L"steelseries:demo", L"SteelSeries Arctis Nova 7", DeviceKind::Headset, 55, 0, false),
        make(L"hsc:1b1c:0a6b", L"Corsair HS80 RGB Wireless", DeviceKind::Headset, 81, 0x1B1C, false),
        make(L"hyperx:demo", L"HyperX Cloud III Wireless", DeviceKind::Headset, 47, 0, false),
        make(L"rog:demo", L"ROG Harpe Ace", DeviceKind::Mouse, 90, 0, false),
        make(L"zowie:demo", L"ZOWIE EC2-CW", DeviceKind::Mouse, 66, 0x04A5, false),
        make(L"bt:demo-ps", L"Wireless Controller", DeviceKind::Gamepad, 38, 0x054C, false),
        make(L"xbox:demo", L"Xbox Wireless Controller", DeviceKind::Gamepad, 72, 0x045E, false),
        make(L"bt:demo-buds", L"Galaxy Buds3 Pro", DeviceKind::Headset, 59, 0, false),
        make(L"airpods:demo", L"AirPods Pro", DeviceKind::Headset, 85, 0, false),
        make(L"jabra:demo", L"Jabra Evolve2 65", DeviceKind::Headset, 23, 0x0B0E, false),
        make(L"jbl:demo", L"JBL Quantum 910", DeviceKind::Headset, 70, 0x0ECB, false),
        make(L"coolermaster:demo", L"Cooler Master MM712", DeviceKind::Mouse, 12, 0x2516, false),
        make(L"steam:demo", L"Steam Controller", DeviceKind::Gamepad, 50, 0x28DE, true),
        make(L"bt:demo-plain", L"Bluetooth Headphones", DeviceKind::Headset, 100, 0, false),
    };
    devices.back().percent = -1;
    devices.back().wirelessLink = WirelessLink::Linked;
    devices[6].connected = false;
    devices[6].percent = -1;
    devices[4].asleep = true;
    auto wired = make(L"demo:kb216", L"Dell KB216", DeviceKind::Keyboard, -1, 0x413C, false);
    wired.battery = BatteryCapability::None;
    wired.wirelessLink = WirelessLink::Linked;
    wired.panelOnly = true;
    devices.push_back(std::move(wired));
    auto headset = make(L"demo:hecate", L"HECATE G5 Wireless", DeviceKind::Headset, -1, 0x35BB, false);
    headset.pid = 0xA008;
    headset.battery = BatteryCapability::Unsupported;
    headset.wirelessLink = WirelessLink::Unknown;
    headset.panelOnly = true;
    devices.push_back(std::move(headset));
    for (auto& s : devices) s.identity = s.key;
    return devices;
}

DWORD WINAPI Worker(LPVOID) {
    // At sign-in a USB receiver may enumerate before its wireless link is ready.
    constexpr DWORD startupPollDelays[] = { 3000, 7000, 15000 };
    size_t startupPoll = 0;
    HANDLE events[] = { g_stopEvt, g_refreshEvt, g_atkWakeEvt };
    const DWORD eventCount = g_atkWakeEvt ? ARRAYSIZE(events) : 2;
    ULONGLONG nextPoll = 0;
    ULONGLONG lastAtkPollReading = 0;
    std::uint64_t lastManualRefresh = 0;
    for (;;) {
        const auto now = GetTickCount64();
        const DWORD wait = WaitForMultipleObjects(eventCount, events, FALSE,
            nextPoll > now ? static_cast<DWORD>(nextPoll - now) : 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) return 0;
        const auto epoch = g_atkEpoch.load();
        if (g_atkReceiverRemoved.exchange(false)) atk8k::ReceiverRemoved();

        if (wait == WAIT_OBJECT_0 + 1 || GetTickCount64() >= nextPoll) {
            const auto refresh = g_requestedRefresh.load();
            auto* v = new PollResult{refresh, epoch, {}};
            v->devices = g_demo ? DemoDevices() : PollAllDevices(refresh != lastManualRefresh, &v->endpoints);
            lastManualRefresh = refresh;
            v->atkActivityAvailable = !g_demo && atk8k::CanReadAfterActivity();
            const auto reading = std::find_if(v->devices.begin(), v->devices.end(), [](const auto& device) {
                return device.key == atk8k::DeviceKey && device.percent >= 0 && !device.cached;
            });
            if (reading != v->devices.end()) {
                lastAtkPollReading = GetTickCount64();
                g_atkWakeRequest = 0;
            }
            if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
            const DWORD delay = startupPoll < ARRAYSIZE(startupPollDelays)
                ? startupPollDelays[startupPoll++] : POLL_INTERVAL_MS;
            nextPoll = GetTickCount64() + delay;
            // A wake event can coincide with the periodic deadline. Keep it only if this scan missed.
            if (wait == WAIT_OBJECT_0 + 2 && g_atkWakeRequest.load()) SetEvent(g_atkWakeEvt);
        } else if (wait == WAIT_OBJECT_0 + 2) {
            const auto activity = g_atkWakeRequest.exchange(0);
            if (!activity || activity <= lastAtkPollReading) continue;
            auto* result = new AtkResult{activity, epoch, atk8k::ReadAfterActivity(activity)};
            if (!PostMessageW(g_hwnd, WM_ATK_RESULT, 0, (LPARAM)result)) delete result;
        }
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_PANEL_PIPE:
            panel::Complete();
            return 0;
        case WM_RESULTS: {
            auto* v = reinterpret_cast<PollResult*>(lp);
            g_completedRefresh = v->refresh;
            OnResults(v->devices, v->endpoints);
            if (!g_atkSuspended && v->atkEpoch == g_atkEpoch.load()) {
                g_atkAwaitingPoll = !v->atkActivityAvailable;
                const auto it = g_entries.find(g_atkEntryKey);
                if (!v->atkActivityAvailable || it == g_entries.end() || !it->second.present || !it->second.st.connected ||
                    (it->second.st.percent >= 0 && !it->second.st.cached)) {
                    CancelAtkActivity();
                    g_atkCooldownUntil = 0;
                }
                if (g_atkWatch.Armed() && !SetTimer(h, TIMER_ATK_LISTEN, ATK_LISTEN_MS, nullptr))
                    StopAtkListener();
                g_atkListenedThisPoll = g_atkWatch.Armed();
                MaybeListenForAtkActivity();
            }
            delete v;
            return 0;
        }
        case WM_ATK_RESULT: {
            auto* result = reinterpret_cast<AtkResult*>(lp);
            OnAtkResult(*result);
            delete result;
            return 0;
        }
        case WM_INPUT:
            OnAtkActivity(reinterpret_cast<HRAWINPUT>(lp));
            return DefWindowProcW(h, msg, wp, lp);
        case WM_INPUT_DEVICE_CHANGE:
            if (g_atkWatch.Armed() && !g_atkWatch.RefreshDevices()) StopAtkListener();
            return 0;
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (lp && (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) {
                const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
                if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
                    atk8k::IsReceiverPath(reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lp)->dbcc_name)) {
                    InvalidateAtkActivity(wp == DBT_DEVICEREMOVECOMPLETE);
                }
            }
            if (!g_deviceSettling && (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE
                                      || wp == DBT_DEVNODES_CHANGED)) {
                // A single device can publish several interfaces. Scan now and once more after they settle.
                g_deviceSettling = SetTimer(h, TIMER_DEVICE_SETTLE, DEVICE_SETTLE_MS, nullptr) != 0;
                SetEvent(g_refreshEvt);
            }
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) {
                g_atkSuspended = true;
                InvalidateAtkActivity(false);
            }
            if (wp == PBT_APMRESUMEAUTOMATIC) {
                InvalidateAtkActivity(false);
                g_atkSuspended = false;
                SetTimer(h, TIMER_RESUME_REFRESH, 5000, nullptr);
            }
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEVICE_SETTLE || wp == TIMER_RESUME_REFRESH) {
                KillTimer(h, wp);
                if (wp == TIMER_DEVICE_SETTLE) g_deviceSettling = false;
                SetEvent(g_refreshEvt);
            }
            if (wp == TIMER_WIDGET) {
                Reconcile();
                RefreshReadingExpiry();
                MaybeListenForAtkActivity();
                panel::ExpireClient();
            }
            if (wp == TIMER_ATK_LISTEN) StopAtkListener();
            if (wp == TIMER_ATK_WAKE) {
                KillTimer(h, TIMER_ATK_WAKE);
                if (g_atkActivityTick) {
                    g_atkWakeRequest = g_atkActivityTick;
                    SetEvent(g_atkWakeEvt);
                }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); RedrawAll(); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            CancelAtkActivity();
            panel::Stop();
            panel::RequestClose(panel::FindPanelWindow());
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                SaveProfile(e, GetTickCount64(), true);
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `--list` prints what the background finds. `--dump` prints every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        // Also saved to a file: PowerShell does not wait for this windowed exe, so console output can end up
        // after the prompt or get lost, and a double-click has no console at all.
        std::wstring text = DumpHidCollections();
        wchar_t path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"WirelessBatteryCenter-dump.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::wstring crlf;
            for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
            int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), nullptr, 0, nullptr, nullptr);
            std::string u = "\xEF\xBB\xBF" + std::string(n, 0);  // UTF-8 byte order mark, so Notepad shows it right
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), u.data() + 3, n, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
            CloseHandle(f);
        }
        if (out && out != INVALID_HANDLE_VALUE) {
            write(text);
            write(std::wstring(L"\nAlso saved to ") + path + L"\n");
        } else {
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);  // no console: show the file
        }
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        Accent accent = BrandOf(s);
        std::wstring line = s.name + L"  [" + s.via + L"]  " +
                            (accent != Accent::None ? L"{" + std::wstring(StyleOf(accent).name) + L"}  " : L"");
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.charging) line += L", charging";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--report")) {  // the "Request support for a device" window on its own
        i18n::Init();
        report::Show(inst, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (report::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        const HWND background = panel::FindBackgroundWindow();
        const HWND window = panel::FindPanelWindow();
        panel::RequestClose(window);
        panel::RequestClose(background);
        for (int i = 0; i < 60 && (IsWindow(background) || IsWindow(window)); ++i) Sleep(100);
        return 0;
    }
    g_inst = inst;
    g_demo = cmdLine && wcsstr(cmdLine, L"--demo");
    if (g_demo) kSettingsKey = product::DemoSettingsKey;

    HANDLE mutex = CreateMutexW(nullptr, FALSE, panel::BackgroundMutex);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    if (!g_demo) g_profileLoadIssues = !ImportLegacySettings(HKEY_CURRENT_USER, product::LegacySettingsKey,
                                                          product::SettingsKey, false);
    if (!RecoverDeviceRemoval(kSettingsKey, g_pendingRemoval)) g_profileLoadIssues = true;

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = panel::BackgroundWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        CloseHandle(mutex);
        return 1;
    }

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_atkWakeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_wantWidget = LoadWidgetMode();
    g_lowBatteryNotifications = LoadBoolSetting(L"LowBatteryNotifications", true);
    g_hideExpiredOrAsleep = LoadHideExpiredOrAsleep();
    g_widgetBackground = LoadWidgetBackground();
    g_brandColors = LoadBrandColors();
    g_stacked = LoadStacked();
    widget::SetStacked(g_stacked);
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    i18n::Init();
    g_widgetMonitor = LoadWidgetMonitor();
    widget::SetMonitorChoice(g_widgetMonitor);
    LoadDeviceSettings();
    LoadProfiles();
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    panel::Start(g_hwnd, WM_PANEL_PIPE, HandlePanelRequest);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (report::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<PollResult*>(m.lParam);
    while (PeekMessageW(&m, nullptr, WM_ATK_RESULT, WM_ATK_RESULT, PM_REMOVE)) delete reinterpret_cast<AtkResult*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (g_worker) CloseHandle(g_worker);
    if (g_atkWakeEvt) CloseHandle(g_atkWakeEvt);
    CloseHandle(mutex);
    return 0;
}
