// "Request support for a device" window and the extra `--dump` sections. See report.h.
//
// Privacy: the only network call in the app is the POST in SendRequest, made when the user clicks Send.
// What is sent is shown word for word in the window first: the device name, its type, the user's optional
// name and notes, and a short technical summary (vendor/product id, HID usage pages and report sizes, USB
// interface classes, the Windows build). No serial numbers, device paths or Bluetooth addresses.
#include "report.h"

#include <commctrl.h>
#include <setupapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "devices.h"
#include "hid.h"
#include "i18n.h"

namespace {

const wchar_t kApiHost[] = L"krisgfx.com";
const wchar_t kApiPath[] = L"/tools/api/requests";
const wchar_t kRequestedPage[] = L"krisgfx.com/tools/#requested";

const DEVPROPKEY kBusReportedDesc = { { 0x540B947E, 0x8B40, 0x45BC, { 0xA8, 0xA2, 0x6A, 0x0B, 0x89, 0x4C, 0xBD, 0xA2 } }, 4 };
const DEVPROPKEY kBtBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const GUID kBluetoothClass = { 0xE0CBF06C, 0xCD8B, 0x4647, { 0xBB, 0x8A, 0x26, 0x3B, 0x43, 0xF0, 0xF9, 0x74 } };

// ---- device scan --------------------------------------------------------------------------------

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

std::wstring RegProp(HDEVINFO di, SP_DEVINFO_DATA& dd, DWORD prop) {
    wchar_t buf[512] = {};
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(di, &dd, prop, &type, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr) &&
        (type == REG_SZ || type == REG_MULTI_SZ))
        return buf;
    return L"";
}

std::wstring StrProp(HDEVINFO di, SP_DEVINFO_DATA& dd, const DEVPROPKEY& key) {
    wchar_t buf[512] = {};
    DEVPROPTYPE type = 0;
    if (SetupDiGetDevicePropertyW(di, &dd, &key, &type, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) &&
        type == DEVPROP_TYPE_STRING)
        return buf;
    return L"";
}

std::wstring DeviceName(HDEVINFO di, SP_DEVINFO_DATA& dd) {
    std::wstring n = StrProp(di, dd, kBusReportedDesc);  // what the device itself reports, e.g. "Pulsar 8K Dongle"
    if (n.empty()) n = RegProp(di, dd, SPDRP_FRIENDLYNAME);
    if (n.empty()) n = RegProp(di, dd, SPDRP_DEVICEDESC);
    return n;
}

// Reads the vendor/product id from a USB instance id or HID path ("VID_046D&PID_C547") or a Bluetooth
// HID path ("_VID&0002046D_PID&B023", where the first four digits name the id's source).
bool ParseIds(const std::wstring& id, USHORT& vid, USHORT& pid) {
    std::wstring u = Upper(id);
    size_t v = u.find(L"VID_"), p = u.find(L"PID_");
    if (v != std::wstring::npos && p != std::wstring::npos) {
        vid = (USHORT)wcstoul(u.substr(v + 4, 4).c_str(), nullptr, 16);
        pid = (USHORT)wcstoul(u.substr(p + 4, 4).c_str(), nullptr, 16);
        return vid != 0;
    }
    v = u.find(L"VID&");
    p = u.find(L"PID&");
    if (v != std::wstring::npos && p != std::wstring::npos && u.size() >= v + 12) {
        vid = (USHORT)wcstoul(u.substr(v + 8, 4).c_str(), nullptr, 16);
        pid = (USHORT)wcstoul(u.substr(p + 4, 4).c_str(), nullptr, 16);
        return vid != 0;
    }
    return false;
}

int InterfaceOf(const std::wstring& path) {
    std::wstring u = Upper(path);
    size_t mi = u.find(L"&MI_");
    return mi == std::wstring::npos ? -1 : (int)wcstol(u.c_str() + mi + 4, nullptr, 16);
}

struct UsbDevice {
    USHORT vid = 0, pid = 0;
    std::wstring name;
    std::map<std::wstring, int> interfaces;  // "HIDClass", "MEDIA (usbaudio)", "Ports (usbser)" ... -> count
    bool hub = false;
};

// Every present USB device, including the ones that have no HID interface (audio-only or WinUSB dongles,
// serial adapters): those never show up in the HID list, which is why some devices were missing from --dump.
std::vector<UsbDevice> EnumUsb() {
    std::map<DWORD, UsbDevice> byId;
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return {};
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t idBuf[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, idBuf, 511, nullptr)) continue;
        USHORT vid = 0, pid = 0;
        if (!ParseIds(idBuf, vid, pid)) continue;  // root hubs and host controllers have no ids
        UsbDevice& d = byId[((DWORD)vid << 16) | pid];
        d.vid = vid;
        d.pid = pid;
        std::wstring cls = RegProp(di, dd, SPDRP_CLASS), svc = RegProp(di, dd, SPDRP_SERVICE);
        std::wstring label = (cls.empty() ? L"unknown" : cls) + (svc.empty() ? L"" : L" (" + svc + L")");
        bool isInterface = Upper(idBuf).find(L"&MI_") != std::wstring::npos;
        if (!isInterface) {
            if (d.name.empty()) d.name = DeviceName(di, dd);
            if (Upper(svc).find(L"HUB") != std::wstring::npos) d.hub = true;
            if (Upper(cls) != L"USB") d.interfaces[label]++;  // single-function device: the device is its own interface
        } else {
            d.interfaces[label]++;
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    std::vector<UsbDevice> out;
    for (auto& [k, d] : byId)
        if (!d.hub) out.push_back(d);
    return out;
}

struct Unopened {
    USHORT vid = 0, pid = 0;
    int iface = -1;
    std::wstring name;
};

// HID collections EnumHid() could not open (held exclusively by another program, or access denied).
// EnumHid() skips them, so before this they were simply missing from --dump.
std::vector<Unopened> UnopenedHid(const std::vector<HidColl>& opened) {
    std::set<std::wstring> seen;
    for (const auto& c : opened) seen.insert(Upper(c.path));
    std::vector<Unopened> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, &dd)) continue;
        if (seen.count(Upper(det->DevicePath))) continue;
        Unopened u;
        if (!ParseIds(det->DevicePath, u.vid, u.pid)) continue;
        u.iface = InterfaceOf(det->DevicePath);
        u.name = DeviceName(di, dd);
        out.push_back(u);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

// One physical device (by vendor/product id) that the user can pick in the window.
struct Candidate {
    USHORT vid = 0, pid = 0;
    std::wstring name;
    std::vector<std::wstring> hid;              // "if2 FFFF:0001 65/65/65" per collection
    std::map<std::wstring, int> usbInterfaces;  // from EnumUsb
    bool vendor = false, mouse = false, keyboard = false, gamepad = false, audio = false;
    const wchar_t* Type() const {
        if (gamepad) return L"controller";
        if (audio) return L"headset";
        if (mouse) return L"mouse";
        if (keyboard) return L"keyboard";
        return L"other";
    }
};

std::vector<Candidate> BuildCandidates() {
    std::map<DWORD, Candidate> m;
    auto colls = EnumHid();
    for (const auto& c : colls) {
        if (!c.vid) continue;
        Candidate& d = m[((DWORD)c.vid << 16) | c.pid];
        d.vid = c.vid;
        d.pid = c.pid;
        if (d.name.empty()) d.name = HidProduct(c);
        wchar_t line[64];
        swprintf(line, 64, L"if%d %04X:%04X %u/%u/%u", c.iface, c.page(), c.usage(), c.inLen(), c.outLen(), c.featLen());
        d.hid.push_back(line);
        if (c.page() >= 0xFF00) d.vendor = true;
        if (c.page() == 0x01 && c.usage() == 0x02) d.mouse = true;
        if (c.page() == 0x01 && c.usage() == 0x06) d.keyboard = true;
        if (c.page() == 0x01 && (c.usage() == 0x04 || c.usage() == 0x05)) d.gamepad = true;
        if (c.page() == 0x0B) d.audio = true;  // telephony page: headset buttons
    }
    for (const auto& u : UnopenedHid(colls)) {
        Candidate& d = m[((DWORD)u.vid << 16) | u.pid];
        d.vid = u.vid;
        d.pid = u.pid;
        if (d.name.empty()) d.name = u.name;
        d.hid.push_back(L"if" + std::to_wstring(u.iface) + L" (could not open)");
    }
    for (const auto& u : EnumUsb()) {
        Candidate& d = m[((DWORD)u.vid << 16) | u.pid];
        d.vid = u.vid;
        d.pid = u.pid;
        if (d.name.empty()) d.name = u.name;
        d.usbInterfaces = u.interfaces;
        for (const auto& [label, n] : u.interfaces)
            if (Upper(label).rfind(L"MEDIA", 0) == 0) d.audio = true;
    }
    std::vector<Candidate> out;
    for (auto& [k, d] : m) {
        if (d.name.empty()) d.name = L"Unknown device";
        out.push_back(d);
    }
    // Devices with a vendor-defined collection or audio first: that is where battery protocols live.
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        bool ia = a.vendor || a.audio, ib = b.vendor || b.audio;
        if (ia != ib) return ia;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::wstring WindowsBuild() {
    wchar_t build[32] = {};
    DWORD size = sizeof(build), ubr = 0, ubrSize = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", RRF_RT_REG_SZ, nullptr,
                 build, &size);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr,
                 &ubrSize);
    return std::wstring(build) + L"." + std::to_wstring(ubr);
}

// The short technical summary that goes into the request's notes (the API allows 500 characters).
std::wstring TechSummary(const Candidate* c) {
    std::wstring s = L"[Sent from the Wireless Battery Center app, Windows build " + WindowsBuild() + L"] ";
    if (!c) return s + L"Device not in the app's list.";
    wchar_t ids[16];
    swprintf(ids, 16, L"%04X:%04X", c->vid, c->pid);
    s += L"IDs " + std::wstring(ids) + L".";
    if (!c->hid.empty()) {
        s += L" HID:";
        for (size_t i = 0; i < c->hid.size(); i++) s += (i ? L", " : L" ") + c->hid[i];
        s += L".";
    } else {
        s += L" No HID interface.";
    }
    if (!c->usbInterfaces.empty()) {
        s += L" USB:";
        bool first = true;
        for (const auto& [label, n] : c->usbInterfaces) {
            s += (first ? L" " : L", ") + label + (n > 1 ? L" x" + std::to_wstring(n) : L"");
            first = false;
        }
        s += L".";
    }
    return s;
}

// ---- sending --------------------------------------------------------------------------------------

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string JsonString(const std::wstring& w) {
    std::string out = "\"";
    for (unsigned char ch : Utf8(w)) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", ch);
                    out += esc;
                } else {
                    out += (char)ch;
                }
        }
    }
    return out + "\"";
}

struct Request {
    std::wstring name, model, type, notes;
};

std::string RequestJson(const Request& r) {
    std::string j = "{";
    if (!r.name.empty()) j += "\"name\":" + JsonString(r.name) + ",";
    j += "\"model\":" + JsonString(r.model) + ",\"type\":" + JsonString(r.type) + ",\"notes\":" + JsonString(r.notes) + "}";
    return j;
}

// POSTs the JSON body. Returns the HTTP status (0 = could not reach the server) and the reply body.
int Post(const std::string& body, std::string& reply) {
    int status = 0;
    HINTERNET s = WinHttpOpen(L"WirelessBatteryCenter", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", kApiPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r && WinHttpSendRequest(r, L"Content-Type: application/json\r\n", (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX))
            status = (int)code;
        for (DWORD avail = 0; WinHttpQueryDataAvailable(r, &avail) && avail && reply.size() < 16384;) {
            std::string chunk(avail, 0);
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
            reply.append(chunk.data(), got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return status;
}

// The "error" string of a JSON reply, or empty.
std::wstring JsonError(const std::string& reply) {
    size_t k = reply.find("\"error\"");
    if (k == std::string::npos) return L"";
    size_t q = reply.find('"', reply.find(':', k) + 1);
    if (q == std::string::npos) return L"";
    std::string v;
    for (size_t i = q + 1; i < reply.size() && reply[i] != '"'; i++) {
        if (reply[i] == '\\' && i + 1 < reply.size()) i++;
        v += reply[i];
    }
    return Wide(v);
}

// What the site answered; the message is put together (and translated) on the UI thread.
struct SendResult {
    int status = 0;
    bool repeat = false;
    std::wstring error;  // the site's own sentence for a 400
};

SendResult SendRequest(Request req) {
    std::string reply;
    int status = Post(RequestJson(req), reply);
    if (status == 400 && req.name.empty() && Upper(JsonError(reply)).find(L"NAME") != std::wstring::npos) {
        req.name = L"App user";  // the site needs a name: use a neutral one rather than asking again
        reply.clear();
        status = Post(RequestJson(req), reply);
    }
    return { status, reply.find("\"repeat\":true") != std::string::npos, JsonError(reply) };
}

std::wstring ResultMessage(const SendResult& r) {
    if (r.status == 201 || (r.status == 200 && !r.repeat))
        return TF(L"Thank you! Your request was sent. You can follow it at {page}.", { { L"page", kRequestedPage } });
    if (r.status == 200) return T(L"You already sent this device in the last 24 hours, so it's on the list. Thank you!");
    if (r.status == 400)
        return TF(L"The request was not accepted: {reason}", { { L"reason", r.error.empty() ? std::wstring(T(L"please check the fields.")) : r.error } });
    if (r.status == 429) return T(L"Too many requests from your network. Please try again in an hour.");
    if (r.status == 0) return T(L"Couldn't reach krisgfx.com. Check your connection, or use \"Copy full report\" and post it on GitHub.");
    return TF(L"The site answered with an error ({code}). Please try again later.", { { L"code", std::to_wstring(r.status) } });
}

// ---- window ---------------------------------------------------------------------------------------

enum : int { IDC_DEVICE = 101, IDC_MODEL, IDC_TYPE, IDC_NAME, IDC_NOTES, IDC_PREVIEW, IDC_COPY, IDC_SEND, IDC_STATUS };
constexpr UINT WM_CANDIDATES = WM_APP + 20, WM_SENT = WM_APP + 21, WM_FULLREPORT = WM_APP + 22;
const wchar_t* const kTypes[] = { L"mouse", L"keyboard", L"headset", L"controller", L"other" };
const wchar_t kClass[] = L"WirelessBatteryCenterReport";

HWND g_wnd;
HFONT g_font;
std::vector<Candidate> g_cands;
bool g_scanned = false, g_sending = false, g_quitOnClose = false;

std::wstring Text(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    int written = GetWindowTextW(h, s.data(), (int)s.size());
    s.resize((size_t)std::max(0, written));
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}

const Candidate* Selected() {
    int i = (int)SendDlgItemMessageW(g_wnd, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < (int)g_cands.size() ? &g_cands[i] : nullptr;
}

bool DeviceChosen() { return SendDlgItemMessageW(g_wnd, IDC_DEVICE, CB_GETCURSEL, 0, 0) >= 0; }

Request CurrentRequest() {
    Request r;
    r.name = Text(IDC_NAME).substr(0, 40);
    r.model = Text(IDC_MODEL).substr(0, 80);
    int t = (int)SendDlgItemMessageW(g_wnd, IDC_TYPE, CB_GETCURSEL, 0, 0);
    r.type = kTypes[t >= 0 && t < 5 ? t : 4];
    std::wstring notes = Text(IDC_NOTES);
    std::wstring tech = TechSummary(Selected());
    r.notes = notes.empty() ? tech : notes + L"\n" + tech;
    if (r.notes.size() > 500) r.notes = r.notes.substr(0, 497) + L"...";
    return r;
}

void UpdatePreview() {
    Request r = CurrentRequest();
    std::wstring p = std::wstring(T(L"Device")) + L": " + (r.model.empty() ? std::wstring(T(L"(type the device name)")) : r.model) +
                     L"\r\n" + T(L"Type") + L": " + T(r.type) + L"\r\n" + T(L"Your name") + L": " +
                     (r.name.empty() ? std::wstring(T(L"(not sent)")) : r.name + L" " + T(L"(kept private)")) + L"\r\n" + T(L"Notes") + L": ";
    for (wchar_t ch : r.notes) p += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
    SetDlgItemTextW(g_wnd, IDC_PREVIEW, p.c_str());
    EnableWindow(GetDlgItem(g_wnd, IDC_SEND), !g_sending && DeviceChosen() && r.model.size() >= 2);
}

void SetStatus(const std::wstring& s) { SetDlgItemTextW(g_wnd, IDC_STATUS, s.c_str()); }

void OnDeviceChanged() {
    const Candidate* c = Selected();
    SetDlgItemTextW(g_wnd, IDC_MODEL, c ? c->name.c_str() : L"");
    int type = 4;
    if (c)
        for (int i = 0; i < 5; i++)
            if (wcscmp(kTypes[i], c->Type()) == 0) type = i;
    SendDlgItemMessageW(g_wnd, IDC_TYPE, CB_SETCURSEL, type, 0);
    UpdatePreview();
}

void FillDevices() {
    HWND cb = GetDlgItem(g_wnd, IDC_DEVICE);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (const auto& c : g_cands) {
        wchar_t ids[16];
        swprintf(ids, 16, L"%04X:%04X", c.vid, c.pid);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)(c.name + L"  (" + ids + L")").c_str());
    }
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)T(L"My device is not in this list"));
    EnableWindow(cb, TRUE);
    SetStatus(T(L"Pick the device that isn't shown. Devices with a vendor-specific interface are listed first."));
    UpdatePreview();
}

int S(int v) { return MulDiv(v, g_wnd ? (int)GetDpiForWindow(g_wnd) : 96, 96); }

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(g_wnd, GWLP_HINSTANCE), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void MakeFont() {
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(g_wnd));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
}

void CreateControls() {
    MakeFont();
    const int L = 16, W = 428;
    Add(L"STATIC",
        T(L"Is one of your devices not shown? Pick it below and send it to us. It goes on the public request list at "
          L"krisgfx.com/tools, and we'll look into supporting it."),
        0, -1, L, 12, W, 56);
    Add(L"STATIC", T(L"Device"), 0, -1, L, 74, W, 18);
    HWND dev = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_DEVICE, L, 92, W, 300);
    SendMessageW(dev, CB_ADDSTRING, 0, (LPARAM)T(L"Scanning devices..."));
    SendMessageW(dev, CB_SETCURSEL, 0, 0);
    EnableWindow(dev, FALSE);
    Add(L"STATIC", T(L"Device name (as it's sold)"), 0, -1, L, 124, W, 18);
    HWND model = Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_MODEL, L, 142, W, 24, WS_EX_CLIENTEDGE);
    SendMessageW(model, EM_LIMITTEXT, 80, 0);
    Add(L"STATIC", T(L"Type"), 0, -1, L, 174, W, 18);
    HWND type = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_TYPE, L, 192, 200, 200);
    for (auto t : kTypes) SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)T(t));  // shown translated, sent in English
    SendMessageW(type, CB_SETCURSEL, 4, 0);
    Add(L"STATIC", T(L"Your name (optional, kept private and never shown)"), 0, -1, L, 224, W, 18);
    HWND name = Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NAME, L, 242, W, 24, WS_EX_CLIENTEDGE);
    SendMessageW(name, EM_LIMITTEXT, 40, 0);
    Add(L"STATIC", T(L"Anything else we should know? (optional)"), 0, -1, L, 274, W, 18);
    HWND notes = Add(WC_EDITW, L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, IDC_NOTES, L, 292, W, 54,
                     WS_EX_CLIENTEDGE);
    SendMessageW(notes, EM_LIMITTEXT, 200, 0);
    Add(L"STATIC", T(L"This is exactly what will be sent:"), 0, -1, L, 354, W, 18);
    Add(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, IDC_PREVIEW, L, 372, W, 110, WS_EX_CLIENTEDGE);
    Add(L"STATIC", T(L"Scanning your devices..."), 0, IDC_STATUS, L, 490, W, 36);
    Add(WC_BUTTONW, T(L"Copy full report"), WS_TABSTOP, IDC_COPY, L, 532, 226, 28);
    Add(WC_BUTTONW, T(L"Send"), BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_SEND, L + W - 186, 532, 90, 28);
    Add(WC_BUTTONW, T(L"Close"), WS_TABSTOP, IDCANCEL, L + W - 90, 532, 90, 28);
    EnableWindow(GetDlgItem(g_wnd, IDC_SEND), FALSE);
}

template <class T>
void PostOrDelete(HWND h, UINT msg, T* data) {
    if (!h || !PostMessageW(h, msg, 0, (LPARAM)data)) delete data;
}

DWORD WINAPI ScanThread(LPVOID p) {
    PostOrDelete((HWND)p, WM_CANDIDATES, new std::vector<Candidate>(BuildCandidates()));
    return 0;
}

struct SendJob {
    HWND wnd;
    Request req;
};

DWORD WINAPI SendThread(LPVOID p) {
    std::unique_ptr<SendJob> job((SendJob*)p);
    PostOrDelete(job->wnd, WM_SENT, new SendResult(SendRequest(job->req)));
    return 0;
}

struct ReportJob {
    HWND wnd;
    std::wstring header;
};

DWORD WINAPI ReportThread(LPVOID p) {
    std::unique_ptr<ReportJob> job((ReportJob*)p);
    PostOrDelete(job->wnd, WM_FULLREPORT, new std::wstring(job->header + DumpHidCollections()));
    return 0;
}

void StartThread(LPTHREAD_START_ROUTINE fn, void* arg) {
    HANDLE t = CreateThread(nullptr, 0, fn, arg, 0, nullptr);
    if (t) CloseHandle(t);
}

void CopyToClipboard(const std::wstring& text) {
    std::wstring crlf;
    for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (crlf.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

bool LightTheme(const wchar_t* value) {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", value,
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

// Title bar follows the Windows app theme, with the matching icon: white (icon 1) on dark, black (icon 2) on
// light. The taskbar button's icon follows the taskbar's own theme.
void ApplyTheme(HWND h) {
    bool lightApps = LightTheme(L"AppsUseLightTheme"), lightTaskbar = LightTheme(L"SystemUsesLightTheme");
    using SetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto fn = (SetAttrFn)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = !lightApps;
            fn(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto icon = [&](bool light, int cx, int cy) {
        return (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(light ? 2 : 1), IMAGE_ICON, GetSystemMetrics(cx),
                                  GetSystemMetrics(cy), LR_SHARED);
    };
    SendMessageW(h, WM_SETICON, ICON_SMALL, icon(lightApps, SM_CXSMICON, SM_CYSMICON));
    SendMessageW(h, WM_SETICON, ICON_BIG, icon(lightTaskbar, SM_CXICON, SM_CYICON));
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) ApplyTheme(h);
            break;
        case WM_CREATE:
            g_wnd = h;
            ApplyTheme(h);
            CreateControls();
            StartThread(ScanThread, h);
            return 0;
        case WM_CANDIDATES: {
            std::unique_ptr<std::vector<Candidate>> v((std::vector<Candidate>*)lp);
            g_cands = std::move(*v);
            g_scanned = true;
            FillDevices();
            return 0;
        }
        case WM_SENT: {
            std::unique_ptr<SendResult> s((SendResult*)lp);
            g_sending = false;
            SetStatus(ResultMessage(*s));
            UpdatePreview();
            return 0;
        }
        case WM_FULLREPORT: {
            std::unique_ptr<std::wstring> s((std::wstring*)lp);
            CopyToClipboard(*s);
            EnableWindow(GetDlgItem(h, IDC_COPY), TRUE);
            SetStatus(T(L"The full report is copied. Paste it into a GitHub issue if we ask for more details."));
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDCANCEL) { DestroyWindow(h); return 0; }
            if (id == IDC_DEVICE && code == CBN_SELCHANGE && g_scanned) { OnDeviceChanged(); return 0; }
            if ((id == IDC_MODEL || id == IDC_NAME || id == IDC_NOTES) && code == EN_CHANGE) { UpdatePreview(); return 0; }
            if (id == IDC_TYPE && code == CBN_SELCHANGE) { UpdatePreview(); return 0; }
            if ((id == IDC_SEND || id == IDOK) && IsWindowEnabled(GetDlgItem(h, IDC_SEND))) {
                g_sending = true;
                EnableWindow(GetDlgItem(h, IDC_SEND), FALSE);
                SetStatus(T(L"Sending..."));
                StartThread(SendThread, new SendJob{ h, CurrentRequest() });
                return 0;
            }
            if (id == IDC_COPY) {
                EnableWindow(GetDlgItem(h, IDC_COPY), FALSE);
                SetStatus(T(L"Collecting the full report (this takes about 10 seconds)..."));
                const Candidate* c = Selected();
                std::wstring header = L"Wireless Battery Center device report\nWindows build " + WindowsBuild() + L"\n";
                if (c) {
                    wchar_t ids[16];
                    swprintf(ids, 16, L"%04X:%04X", c->vid, c->pid);
                    header += L"Selected device: " + Text(IDC_MODEL) + L" (" + ids + L")\n";
                }
                StartThread(ReportThread, new ReportJob{ h, header + L"\n" });
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_DESTROY:
            g_wnd = nullptr;
            g_cands.clear();
            g_scanned = g_sending = false;
            if (g_font) { DeleteObject(g_font); g_font = nullptr; }
            if (g_quitOnClose) PostQuitMessage(0);  // opened with --report: the process ends with the window
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace report {

void Show(HINSTANCE inst, HWND owner) {
    if (g_wnd) {
        ShowWindow(g_wnd, SW_RESTORE);
        SetForegroundWindow(g_wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    // Size for the client area at the monitor's DPI, centred on the screen with the cursor.
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, MulDiv(460, dpiX, 96), MulDiv(576, dpiX, 96) };
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpiX);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - hgt) / 2;
    HWND h = CreateWindowExW(i18n::Rtl() ? WS_EX_LAYOUTRTL : 0, kClass, (std::wstring(L"Wireless Battery Center: ") + T(L"request support for a device")).c_str(), style, x, y, w, hgt, nullptr, nullptr, inst,
                             nullptr);
    g_quitOnClose = owner == nullptr;
    if (!h) return;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

bool PreTranslate(MSG* msg) { return g_wnd && IsDialogMessageW(g_wnd, msg); }

std::wstring DumpExtras() {
    std::wstring out;
    auto unopened = UnopenedHid(EnumHid());
    if (!unopened.empty()) {
        out += L"\nHID collections that could not be opened (in use by another program):\n";
        for (const auto& u : unopened) {
            wchar_t line[64];
            swprintf(line, 64, L"%04X:%04X  %2d  ", u.vid, u.pid, u.iface);
            out += line + u.name + L"\n";
        }
    }
    out += L"\nUSB devices, including ones without a HID interface (VID:PID  name  [interfaces]):\n";
    for (const auto& u : EnumUsb()) {
        wchar_t ids[16];
        swprintf(ids, 16, L"%04X:%04X", u.vid, u.pid);
        std::wstring line = std::wstring(ids) + L"  " + (u.name.empty() ? L"(no name)" : u.name) + L"  [";
        bool first = true;
        for (const auto& [label, n] : u.interfaces) {
            line += (first ? L"" : L", ") + label + (n > 1 ? L" x" + std::to_wstring(n) : L"");
            first = false;
        }
        out += line + L"]\n";
    }
    out += L"\nBluetooth devices Windows knows about:\n";
    HDEVINFO di = SetupDiGetClassDevsW(&kBluetoothClass, nullptr, nullptr, DIGCF_PRESENT);
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
            wchar_t idBuf[512] = {};
            SetupDiGetDeviceInstanceIdW(di, &dd, idBuf, 511, nullptr);
            if (Upper(idBuf).rfind(L"BTH", 0) != 0) continue;  // the radio itself and its services
            std::wstring name = RegProp(di, dd, SPDRP_FRIENDLYNAME);
            if (name.empty()) continue;
            BYTE level = 0;
            DEVPROPTYPE type = 0;
            bool battery = SetupDiGetDevicePropertyW(di, &dd, &kBtBattery, &type, &level, sizeof(level), nullptr, 0) && type == DEVPROP_TYPE_BYTE;
            out += L"  " + name + (battery ? L"  (battery reported by Windows: " + std::to_wstring(level) + L"%)" : L"") + L"\n";
        }
        SetupDiDestroyDeviceInfoList(di);
    }
    return out;
}

}  // namespace report
