// Fallbacks that work for any vendor:
//  * HID devices exposing a standard battery usage (Generic Device Controls "Battery Strength",
//    Battery System "Absolute State Of Charge") in a feature or input report.
//  * Bluetooth devices: Windows keeps the level it reads over HFP (classic) or the GATT Battery
//    Service (LE) in a device property, the same value Settings > Bluetooth & devices shows.
#include "drivers.h"

#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace {

constexpr bool MergeConnectedState(bool previous, bool current) {
    return previous || current;
}

static_assert(!MergeConnectedState(false, false));
static_assert(MergeConnectedState(false, true));
static_assert(MergeConnectedState(true, false));
static_assert(MergeConnectedState(true, true));

// Enumeration order must not change the result.
static_assert(MergeConnectedState(MergeConnectedState(false, true), false));
static_assert(MergeConnectedState(MergeConnectedState(false, false), true));

bool IsBatteryUsage(USHORT page, USAGE usage) {
    return (page == 0x06 && usage == 0x20) || (page == 0x85 && usage == 0x65);
}

const DEVPROPKEY kBtBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const DEVPROPKEY kIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };
const DEVPROPKEY kClassOfDevice = { { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 10 };

constexpr DeviceKind KindFromClassOfDevice(UINT32 cod) {
    UINT32 major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
    if (major == 0x02) return DeviceKind::Phone;
    if (major == 0x04) return DeviceKind::Headset;
    if (major == 0x05) {
        if ((minor & 0x0F) == 0x01 || (minor & 0x0F) == 0x02) return DeviceKind::Gamepad;
        if ((minor >> 4) == 1) return DeviceKind::Keyboard;
        if ((minor >> 4) == 2) return DeviceKind::Mouse;
    }
    return DeviceKind::Other;
}

static_assert(KindFromClassOfDevice(0x0200) == DeviceKind::Phone);
static_assert(KindFromClassOfDevice(0x0204) == DeviceKind::Phone);

static_assert(KindFromClassOfDevice(0x0400) == DeviceKind::Headset);
static_assert(KindFromClassOfDevice(0x0540) == DeviceKind::Keyboard);
static_assert(KindFromClassOfDevice(0x0580) == DeviceKind::Mouse);
static_assert(KindFromClassOfDevice(0x0504) == DeviceKind::Gamepad);

static_assert(KindFromClassOfDevice(0x0000) == DeviceKind::Other);


std::wstring StrProp(HDEVINFO di, SP_DEVINFO_DATA* dd, const DEVPROPKEY& key) {
    DEVPROPTYPE t;
    wchar_t buf[256] = {};
    if (SetupDiGetDevicePropertyW(di, dd, &key, &t, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) && t == DEVPROP_TYPE_STRING)
        return buf;
    return L"";
}

// Container properties like the category are string lists; only the first entry matters here.
std::wstring FirstStringProp(HDEVINFO di, SP_DEVINFO_DATA* dd, const DEVPROPKEY& key) {
    DEVPROPTYPE t;
    wchar_t buf[256] = {};
    if (SetupDiGetDevicePropertyW(di, dd, &key, &t, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) &&
        (t == DEVPROP_TYPE_STRING_LIST || t == DEVPROP_TYPE_STRING))
        return buf;
    return L"";
}

// Classic Bluetooth devices carry a Class of Device, but LE devices (BTHLE\DEV_...) usually do not,
// so KindFromClassOfDevice has nothing to work with and a LE keyboard or mouse ends up as "Other".
// Windows still files the device under a category for Settings > Bluetooth & devices; use it.
DeviceKind KindFromCategory(const std::wstring& c) {
    if (c == L"Communication.Phone" ||
        c.rfind(L"Communication.Phone.", 0) == 0)
        return DeviceKind::Phone;
    if (c == L"Input.Keyboard") return DeviceKind::Keyboard;
    if (c == L"Input.Mouse") return DeviceKind::Mouse;
    if (c == L"Input.Gaming") return DeviceKind::Gamepad;
    if (c.rfind(L"Communication.Headset", 0) == 0 || c == L"Audio.Headphones") return DeviceKind::Headset;
    return DeviceKind::Other;
}

// Bluetooth LE keyboards that also expose a mouse collection (for the knob or macros), so
// KindFromCollections would call them a mouse. Matched on the exact HID vid/pid seen on the LE link.
//  * 3151:4013 Attack Shark K86 (Rongyuan firmware) over Bluetooth, from the reporter's --dump in
//    krisgfx9/Wireless-Device-Battery-Monitor#168. Its USB ids are 3151:4011 / 4015 (OpenRGB issue 4074).
DeviceKind KindFromBleIds(USHORT vid, USHORT pid) {
    if (vid == 0x3151 && pid == 0x4013) return DeviceKind::Keyboard;
    return DeviceKind::Other;
}

std::wstring StripServiceSuffix(std::wstring n) {
    for (const wchar_t* suf : { L" Hands-Free AG", L" Hands-Free", L" Avrcp Transport", L" Stereo", L" Audio Gateway" }) {
        size_t l = wcslen(suf);
        if (n.size() > l && n.compare(n.size() - l, l, suf) == 0) return n.substr(0, n.size() - l);
    }
    return n;
}

}  // namespace

void PollGenericHid(PollContext& ctx) {
    // Drawing tablets and pen displays (any collection on the Digitizers page 0D) are never read:
    // polling one for a battery report flipped the Y axis of an XP-Pen Deco Fun S (issue #232).
    std::vector<std::pair<USHORT, USHORT>> digitizers;
    for (const auto& c : ctx.colls)
        if (c.page() == 0x0D) digitizers.push_back({ c.vid, c.pid });
    const auto claimedByVendors = ctx.claimed;
    std::set<std::wstring> readEndpoints;
    for (const auto& c : ctx.colls) {
        const auto identity = HidIdentity(c);
        const auto endpoint = identity.empty() ? Hex4(c.vid) + L":" + Hex4(c.pid) : identity;
        if (claimedByVendors.count({c.vid, c.pid}) || readEndpoints.count(endpoint)) continue;
        if (c.page() == 0x84 || c.page() == 0x85) continue;  // UPS / power supplies
        if (std::find(digitizers.begin(), digitizers.end(), std::make_pair(c.vid, c.pid)) != digitizers.end()) continue;
        if (!c.caps.NumberFeatureValueCaps && !c.caps.NumberInputValueCaps) continue;
        // Same physical device already shown with Windows' Bluetooth level (its HID field can read 0).
        if (ctx.btContainers.count(c.container)) continue;
        HANDLE h = CreateFileW(c.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp;
        if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); continue; }

        int pct = -1;
        for (HIDP_REPORT_TYPE type : { HidP_Feature, HidP_Input }) {
            USHORT n = type == HidP_Feature ? c.caps.NumberFeatureValueCaps : c.caps.NumberInputValueCaps;
            USHORT len = type == HidP_Feature ? c.featLen() : c.inLen();
            if (!n || !len) continue;
            std::vector<HIDP_VALUE_CAPS> vc(n);
            if (HidP_GetValueCaps(type, vc.data(), &n, pp) != HIDP_STATUS_SUCCESS) continue;
            for (USHORT i = 0; i < n && pct < 0; i++) {
                const auto& v = vc[i];
                USAGE u = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
                if (!IsBatteryUsage(v.UsagePage, u)) continue;
                std::vector<BYTE> buf(len, 0);
                buf[0] = v.ReportID;
                BOOL got = type == HidP_Feature ? HidD_GetFeature(h, buf.data(), len) : HidD_GetInputReport(h, buf.data(), len);
                if (!got) continue;
                ULONG val = 0;
                if (HidP_GetUsageValue(type, v.UsagePage, v.LinkCollection, u, &val, pp, (PCHAR)buf.data(), len) != HIDP_STATUS_SUCCESS)
                    continue;
                // Apple devices (Magic Trackpad / Mouse / Keyboard) report a plain percentage while
                // declaring a 0-255 range; Linux handles them with HID_BATTERY_QUIRK_PERCENT.
                bool percentQuirk = c.vid == 0x05AC && val <= 100;
                if (percentQuirk) pct = (int)val;
                else if (v.LogicalMax > v.LogicalMin) pct = Clamp100((int)(((LONG)val - v.LogicalMin) * 100 / (v.LogicalMax - v.LogicalMin)));
                else if (val <= 100) pct = (int)val;
            }
            if (pct >= 0) break;
        }
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if (pct < 0) continue;
        const auto bluetooth = std::find_if(ctx.out.begin(), ctx.out.end(), [&](const DeviceStatus& device) {
            return device.panelOnly && !c.container.empty() && device.identity == L"bt:" + c.container;
        });
        if (bluetooth != ctx.out.end()) {
            bluetooth->percent = pct;
            bluetooth->panelOnly = false;
            bluetooth->protocol = "HID battery report";
            ctx.Claim(c.vid, c.pid);
            readEndpoints.insert(endpoint);
            continue;
        }
        DeviceStatus s;
        s.name = HidProduct(c);
        if (s.name.empty()) s.name = L"Wireless device";
        s.key = L"hid:" + Hex4(c.vid) + L":" + Hex4(c.pid);
        s.via = L"HID";
        s.kind = KindFromCollections(ctx.colls, c.vid, c.pid);
        if (s.kind == DeviceKind::Other) s.kind = GuessKind(s.name);
        s.percent = pct;
        ctx.Add(s, c);
        readEndpoints.insert(endpoint);
    }
}

void PollBluetooth(PollContext& ctx) {
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return;

    struct Found { int pct; std::wstring name; bool connected; };
    std::map<std::wstring, std::wstring> containerName;  // container id -> name of the main BT device node
    std::map<std::wstring, UINT32> containerCod;         // container id -> class of device
    std::map<std::wstring, std::wstring> containerCat;   // container id -> device container category
    std::map<std::wstring, bool> containerConn;          // container id -> main BT device node connected
    std::map<std::wstring, Found> battery;
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, nullptr)) continue;
        if (wcsncmp(inst, L"BTHENUM\\", 8) && wcsncmp(inst, L"BTHLE\\", 6) && wcsncmp(inst, L"BTHLEDEVICE\\", 12)) continue;

        GUID container{};
        DEVPROPTYPE t;
        std::wstring cid = inst;
        if (SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_ContainerId, &t, (PBYTE)&container, sizeof(container), nullptr, 0)) {
            wchar_t g[64];
            StringFromGUID2(container, g, 64);
            cid = g;
        }
        std::wstring name = StrProp(di, &dd, DEVPKEY_Device_FriendlyName);
        if (name.empty()) name = StrProp(di, &dd, DEVPKEY_Device_DeviceDesc);
        if (!wcsncmp(inst, L"BTHENUM\\DEV_", 12) || !wcsncmp(inst, L"BTHLE\\DEV_", 10)) {
            containerName[cid] = name;
            UINT32 cod = 0;
            if (SetupDiGetDevicePropertyW(di, &dd, &kClassOfDevice, &t, (PBYTE)&cod, sizeof(cod), nullptr, 0) && t == DEVPROP_TYPE_UINT32)
                containerCod[cid] = cod;
            std::wstring cat = FirstStringProp(di, &dd, DEVPKEY_DeviceContainer_Category);
            if (!cat.empty()) containerCat[cid] = cat;
            DEVPROP_BOOLEAN conn;
            if (SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) &&
                t == DEVPROP_TYPE_BOOLEAN) {
                // Classic and LE nodes can share one container. Keep it connected when
                // either transport is active, regardless of device enumeration order.
                containerConn[cid] = MergeConnectedState(containerConn[cid], conn != DEVPROP_FALSE);
            }
        }

        BYTE pct = 0;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kBtBattery, &t, &pct, sizeof(pct), nullptr, 0) || t != DEVPROP_TYPE_BYTE) continue;
        // The service node's own connected flag is only a fallback: headphones keep their level on the
        // Hands-Free node, which reads as disconnected while audio runs over A2DP. The main device node
        // decides below when there is one.
        DEVPROP_BOOLEAN conn = DEVPROP_TRUE;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) ||
            t != DEVPROP_TYPE_BOOLEAN)
            conn = DEVPROP_TRUE;
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, dd.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)) continue;
        if (pct > 100) continue;
        auto prev = battery.find(cid);
        if (prev != battery.end() && prev->second.connected && conn == DEVPROP_FALSE) continue;  // prefer a live node
        battery[cid] = { pct, StripServiceSuffix(name), conn != DEVPROP_FALSE };
    }
    SetupDiDestroyDeviceInfoList(di);

    // Physical devices a driver above already reported over a HID interface on the Bluetooth link
    // itself (e.g. Logitech HID++ names the LIFT "Logitech LIFT VERTICAL ERGONOMIC MOUSE" while
    // Bluetooth calls it "LIFT"). Only suppress the Windows row with shared physical evidence.
    std::set<std::wstring> reported;
    std::map<std::wstring, std::pair<USHORT, USHORT>> hidIds;  // container id -> HID vid/pid on that device
    for (const auto& c : ctx.colls) {
        if (c.container.empty()) continue;
        hidIds[c.container] = { c.vid, c.pid };
        const auto identity = HidIdentity(c);
        const bool ambiguous = std::any_of(ctx.colls.begin(), ctx.colls.end(), [&](const HidColl& other) {
            return c.vid == other.vid && c.pid == other.pid && c.container != other.container;
        });
        if (!identity.empty() && std::any_of(ctx.out.begin(), ctx.out.end(), [&](const DeviceStatus& s) {
            return s.identity == identity || (!ambiguous && s.identity.empty() && s.vid == c.vid && s.pid == c.pid);
        })) reported.insert(c.container);
    }

    for (const auto& [cid, online] : containerConn) {
        if (online) battery.try_emplace(cid, Found{-1, L"", true});
    }
    for (const auto& [cid, f] : battery) {
        if (reported.count(cid) || ctx.btContainers.count(cid)) continue;
        auto mc = containerConn.find(cid);
        if (mc != containerConn.end() ? !mc->second : !f.connected) continue;
        DeviceStatus s;
        s.key = L"bt:" + cid;
        auto it = containerName.find(cid);
        s.name = (it != containerName.end() && !it->second.empty()) ? it->second : f.name;
        if (s.name.empty()) s.name = L"Bluetooth device";
        s.identity = s.key;
        s.via = L"Bluetooth";
        s.percent = f.pct;
        s.systemBatterySnapshot = f.pct >= 0;
        s.wirelessLink = mc == containerConn.end() ? WirelessLink::Unknown : WirelessLink::Linked;
        auto cod = containerCod.find(cid);
        s.kind = cod != containerCod.end() ? KindFromClassOfDevice(cod->second) : DeviceKind::Other;
        if (s.kind == DeviceKind::Other) {
            auto cat = containerCat.find(cid);
            if (cat != containerCat.end()) s.kind = KindFromCategory(cat->second);
        }
        // Bluetooth LE devices have no class of device; their HID collections tell a mouse from a keyboard.
        auto ids = hidIds.find(cid);
        if (ids != hidIds.end()) {
            s.vid = ids->second.first;
            s.pid = ids->second.second;
        }
        if (s.kind == DeviceKind::Other && ids != hidIds.end()) s.kind = KindFromBleIds(ids->second.first, ids->second.second);
        if (s.kind == DeviceKind::Other && ids != hidIds.end()) s.kind = KindFromCollections(ctx.colls, ids->second.first, ids->second.second);
        if (f.pct < 0) {
            if (s.kind == DeviceKind::Other || s.kind == DeviceKind::Phone || it == containerName.end()) continue;
            s.panelOnly = true;
        }
        if (s.kind == DeviceKind::Other) s.kind = GuessKind(s.name);
        ctx.out.push_back(s);
        if (f.pct >= 0) ctx.btContainers.insert(cid);
    }
}
