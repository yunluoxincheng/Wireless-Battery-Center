// Evision keyboards: ATK V75X (320F:5088 receiver, 5055 cable) and VGN V98Pro V2 (5088 receiver).
// Ported from HaloBattery (github.com/HeyOkay/HaloBattery, providers/atk_keyboard.py, MIT; confirmed there by
// its author on both), from ATK HUB 3.2.29's Evision keyboard driver (keyboard-info.util-*.js).
//   Output report 04 on FF1C:0092, 31 payload bytes: [04][seq lo][seq hi][1A][00 ...]
//   Input report 04: [04][seq lo][seq hi][1A][..][00 00][..][percent][power state]
// These ids are shared with other Evision keyboards, so the product name is checked as well.
// The receiver is also sold under the generic name "Wireless Gaming Keyboard" (Feker Alice80 and others); the same
// request and reply were confirmed on a Feker Alice80 on that receiver (seq echoed, command 1A, level at byte 8),
// and its backlight still timed out while it was asked every minute. Such keyboards are shown under that name.
// HaloBattery only asks over the receiver while the keyboard is being typed on, so a sleeping keyboard is
// not woken. This app asks automatically at most every 10 minutes over the receiver
// (every poll on the cable), and allows an explicit refresh for VGN.
#include "drivers.h"

#include <algorithm>
#include <map>

namespace {

constexpr USHORT kVid = 0x320F, kReceiver = 0x5088, kCable = 0x5055;
constexpr ULONGLONG kReceiverIntervalMs = 10 * 60 * 1000;
constexpr ULONGLONG kVgnManualIntervalMs = 2000;
constexpr wchar_t kVgnName[] = L"VGN V98Pro V2";

bool ReadBattery(const HidColl& c, int& percent, BYTE& powerState) {
    static USHORT seq = 0;
    seq = (USHORT)(seq % 0xFFFF + 1);
    HidDev d;
    if (!d.Open(c)) return false;
    d.Flush();
    std::vector<BYTE> req(32, 0);
    req[0] = 0x04;
    req[1] = (BYTE)(seq & 0xFF);
    req[2] = (BYTE)(seq >> 8);
    req[3] = 0x1A;
    if (!d.Write(req)) return false;
    ULONGLONG end = GetTickCount64() + 1000;
    std::vector<BYTE> r;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int n = d.Read(r, 100);
        if (n < 10 || r[0] != 0x04 || r[1] != (BYTE)(seq & 0xFF) || r[2] != (BYTE)(seq >> 8) || r[3] != 0x1A || r[5] || r[6])
            continue;
        if (r[8] > 100 || r[9] > 2) return false;
        percent = r[8];
        powerState = r[9];
        return true;
    }
    return false;
}

}  // namespace

void PollAtkKeyboard(PollContext& ctx) {
    struct State {
        ULONGLONG lastAsk = 0;
        int lastPercent = -1;
        bool lastCharging = false;
    };
    static std::map<std::wstring, State> states;
    const bool receiverAlreadyClaimed = ctx.IsClaimed(kVid, kReceiver);
    for (USHORT pid : { kCable, kReceiver }) {
        bool cable = pid == kCable;
        if (cable ? ctx.IsClaimed(kVid, pid) : receiverAlreadyClaimed) continue;
        const wchar_t* product = cable ? L"ATK V75X" : L"ATK V75X 2.4G Dongle";
        std::set<std::wstring> seen;
        for (const HidColl* c : ctx.Find(kVid, pid, [](const HidColl& c) { return c.page() == 0xFF1C && c.usage() == 0x0092; })) {
            std::wstring name = HidProduct(*c);
            while (!name.empty() && name.back() == L' ') name.pop_back();
            bool generic = !cable && _wcsicmp(name.c_str(), L"Wireless Gaming Keyboard") == 0;
            // VGN V98Pro V2 shares the same protocol and VID/PID; verified on the 64-byte FF1C:0092 collection.
            bool vgn = !cable && _wcsicmp(name.c_str(), L"VGN V98pro V2 2.4G Dongle") == 0;
            if (vgn && (c->inLen() != 64 || c->outLen() != 64)) continue;
            if (!generic && !vgn && _wcsicmp(name.c_str(), product) != 0) continue;
            if (!seen.insert(HidIdentity(*c)).second) continue;
            DeviceStatus s;
            s.key = vgn ? L"vgn_v98pro_v2:5088" : L"atk_keyboard:5088";
            s.name = vgn ? kVgnName : generic ? L"Wireless Gaming Keyboard" : L"ATK V75X";
            auto& state = states[c->path];
            s.kind = DeviceKind::Keyboard;
            s.via = cable ? L"USB cable" : L"2.4 GHz receiver";
            // The VGN entry represents the physical receiver even when the keyboard does not answer.
            s.graceSec = vgn ? 0 : 1800;
            ULONGLONG now = GetTickCount64();
            const auto interval = vgn && ctx.manualRefresh ? kVgnManualIntervalMs : kReceiverIntervalMs;
            bool ask = cable || !state.lastAsk || now - state.lastAsk >= interval;
            int percent = -1;
            bool charging = false;
            if (ask) {
                state.lastAsk = now;
                BYTE powerState = 0;
                if (ReadBattery(*c, percent, powerState)) {
                    // ATK HUB's Evision parser uses 02; VGN confirmed 00 on battery and 02 on USB power.
                    charging = vgn ? powerState == 2 : powerState != 0;
                    state.lastPercent = percent;
                    state.lastCharging = charging;
                }
            }
            if (vgn) {
                // Silence does not distinguish a sleeping keyboard from a switched-off one.
                s.wirelessLink = percent >= 0 ? WirelessLink::Linked : WirelessLink::Unknown;
                s.percent = percent >= 0 ? percent : state.lastPercent;
                s.cached = percent < 0 && s.percent >= 0;
                s.charging = percent >= 0 && charging;
            } else if (ask && percent < 0) {
                s.asleep = true;  // no reply: asleep, off or busy; keep the last value
            } else {
                s.percent = ask ? percent : state.lastPercent;
                s.cached = !ask && s.percent >= 0;
                s.charging = ask ? charging : state.lastCharging;
                if (s.percent < 0) s.asleep = true;
            }
            ctx.Add(s, *c);
            if (cable) ctx.Claim(kVid, kReceiver);
        }
    }
    for (auto it = states.begin(); it != states.end();) {
        if (std::none_of(ctx.colls.begin(), ctx.colls.end(), [&](const HidColl& c) { return c.path == it->first; }))
            it = states.erase(it);
        else ++it;
    }
}
