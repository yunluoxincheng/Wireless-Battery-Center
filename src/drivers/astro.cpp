// Logitech / ASTRO wireless headsets (A50 X, A50 Gen 5, A50 Gen 4, A50 Gen 3, A20 Gen 1/2).
//
// 1. ASTRO A50 X (046D:0B0B) and A50 Gen 5 (046D:0B1C):
//    Vendor HID collection on usage page FF32 / usage 0074 (interface 8), 64-byte reports, ID 02:
//      request: [02][0C][03][00][06][0C][00...]
//      reply:   [02][0C][len][00][06][handle][percent][...][charging]
//    (Protocol from Sapd/HeadsetControl logitech_astro_a50.hpp.)
//
// 2. ASTRO A50 Gen 4 ("Mimas"), A50 Gen 3 ("Europa") and A20 ("Aquarius"), VID 9886:
//    * Gen 4 base stations: 002C (PlayStation/PC), 002D / 002A (Xbox/PC)
//    * Gen 3 base stations: 0015, 0014, 0018, 001D, 0026
//    * A20 transmitters:    0037, 0038, 0039, 0043, 0044, 0045
//    Vendor HID collection on usage page FF32 or FF31, usage 0074 (interface 6), 64-byte reports,
//    report ID 02:
//      request: [02][cmd]
//      reply:   [02][status][len][payload...]   (status 02 = OK, 00 = accepted, 01 = error)
//    cmd 0x54 (headset status) -> payload[0]: bit 0 (0x01) = docked, bit 1 (0x02) = live 2.4 GHz link
//    cmd 0x7C (battery status) -> payload[0]: bit 7 (0x80) = charging, bits 0..6 = battery % (0..100)
//    (Protocol from tdryer/eh-fifty, nichtlegacy/AstroBar, Sapd/HeadsetControl logitech_astro_a50_gen4.hpp.)
//
//    Coexistence with Astro Command Center (Microsoft Store / UWP):
//    The UWP app opens FF32:0074 with FileAccessMode.ReadWrite, placing an exclusive kernel lock
//    (ShareMode = 0) on the collection while open. EnumHid() recovers the VID/PID from the device path
//    when CreateFileW returns ERROR_SHARING_VIOLATION, so PollAstro keeps the headset visible (with
//    its last cached reading) and resumes live polling as soon as Astro Command Center is closed.
#include "drivers.h"

#include <map>

namespace {

constexpr USHORT kVidAstro = 0x9886;
constexpr USHORT kVidLogitech = 0x046D;

struct LegacyModel { USHORT pid; const wchar_t* name; };
constexpr LegacyModel kLegacyModels[] = {
    // A50 Gen 4 (Mimas)
    { 0x002C, L"ASTRO A50 Gen 4" },
    { 0x002D, L"ASTRO A50 Gen 4" },
    { 0x002A, L"ASTRO A50 Gen 4" },
    // A50 Gen 3 (Europa)
    { 0x0015, L"ASTRO A50 Gen 3" },
    { 0x0014, L"ASTRO A50 Gen 3" },
    { 0x0018, L"ASTRO A50 Gen 3" },
    { 0x001D, L"ASTRO A50 Gen 3" },
    { 0x0026, L"ASTRO A50 Gen 3" },
    // A20 Wireless (Aquarius)
    { 0x0037, L"ASTRO A20 Wireless" },
    { 0x0038, L"ASTRO A20 Wireless" },
    { 0x0039, L"ASTRO A20 Wireless" },
    { 0x0043, L"ASTRO A20 Wireless" },
    { 0x0044, L"ASTRO A20 Wireless" },
    { 0x0045, L"ASTRO A20 Wireless" },
};

struct CachedState {
    int percent = -1;
    bool charging = false;
    bool docked = false;
};
std::map<USHORT, CachedState> g_cache;

bool QueryLegacy(HidDev& d, BYTE cmd, BYTE& value) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        d.Flush();
        if (!d.Write({ 0x02, cmd })) return false;
        ULONGLONG end = GetTickCount64() + 500;
        std::vector<BYTE> r;
        while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
            int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
            if (d.Read(r, left) < 4) break;
            if (r[0] != 0x02) continue;
            if (r[1] == 0x01) return false;
            if (r[1] == 0x02) {
                value = r[3];
                return true;
            }
        }
    }
    return false;
}

bool PollLegacyCollection(const HidColl& c, const wchar_t* name, DeviceStatus& s) {
    s.key = L"astro:" + Hex4(c.pid);
    s.name = name;
    s.kind = DeviceKind::Headset;
    s.via = L"USB base station";
    s.graceSec = 1800;

    HidDev d;
    if (d.Open(c)) {
        BYTE st = 0;
        if (!QueryLegacy(d, 0x54, st)) return false;
        bool docked = (st & 0x01) != 0;
        bool linked = (st & 0x02) != 0;
        s.connected = docked || linked;
        if (!s.connected) return true;

        BYTE bat = 0;
        if (QueryLegacy(d, 0x7C, bat)) {
            int pct = bat & 0x7F;
            if (pct <= 100) {
                s.percent = pct;
                s.charging = (bat & 0x80) != 0 || (docked && pct < 100);
                if (docked) s.detail = L"docked";
                g_cache[c.pid] = { s.percent, s.charging, docked };
                return true;
            }
        }
        return true;
    }

    // Exclusive lock held by Astro Command Center (UWP): keep the headset visible with cached reading.
    s.connected = true;
    auto it = g_cache.find(c.pid);
    if (it != g_cache.end() && it->second.percent >= 0) {
        s.cached = true;
        s.percent = it->second.percent;
        s.charging = it->second.charging;
        s.detail = it->second.docked ? L"docked, Astro Command Center" : L"Astro Command Center";
    } else {
        s.detail = L"Astro Command Center";
    }
    return true;
}

}  // namespace

void PollAstro(PollContext& ctx) {
    // 1. ASTRO A50 X (046D:0B0B) and A50 Gen 5 (046D:0B1C)
    for (const auto& c : ctx.colls) {
        if (c.vid != kVidLogitech || (c.pid != 0x0B0B && c.pid != 0x0B1C) ||
            ctx.IsClaimed(c.vid, c.pid) || c.page() != 0xFF32 || c.usage() != 0x0074 ||
            c.inLen() != 64 || c.outLen() != 64) continue;
        HidDev d;
        if (!d.Open(c)) continue;
        d.Flush();
        // No payload: read battery only; never change audio or headset settings.
        std::vector<BYTE> request(64, 0);
        request[0] = 0x02;
        request[1] = 0x0C;
        request[2] = 3;
        request[4] = 0x06;
        request[5] = 0x0C;
        if (!d.Write(request)) continue;
        const auto deadline = GetTickCount64() + 1000;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            std::vector<BYTE> reply;
            int n = d.Read(reply, (int)(deadline - now));
            if (n < 0) break;
            // Ignore unsolicited volume/mix events and malformed or partial frames.
            if (n < 9 || reply.size() < 9 || reply[0] != 0x02 || reply[1] != 0x0C ||
                reply[2] < 6 || (size_t)reply[2] + 3 > reply.size() ||
                (int)reply[2] + 3 > n || reply[3] != 0 || reply[4] != 0x06 ||
                (reply[5] != 0 && reply[5] != 0x0C) || reply[6] > 100) continue;
            DeviceStatus s;
            s.key = L"astro:" + c.path;
            s.name = c.pid == 0x0B0B ? L"ASTRO A50 X" : L"ASTRO A50 Gen 5";
            s.kind = DeviceKind::Headset;
            s.via = L"USB base station";
            s.percent = reply[6];
            s.charging = reply[8] != 0;
            ctx.Add(s, c.vid, c.pid);
            break;
        }
    }

    // 2. ASTRO A50 Gen 4 / Gen 3 and A20 Wireless (VID 9886)
    for (const LegacyModel& m : kLegacyModels) {
        if (ctx.IsClaimed(kVidAstro, m.pid)) continue;
        for (const HidColl* c : ctx.Find(kVidAstro, m.pid, [](const HidColl& c) {
                 bool validPage = (c.page() == 0xFF32 || c.page() == 0xFF31 || c.page() == 0xFF00) &&
                                  c.inLen() >= 64 && c.outLen() >= 64;
                 bool lockedVendorIf = c.page() == 0 && (c.iface == 6 || c.iface == -1);
                 return validPage || lockedVendorIf;
             })) {
            DeviceStatus s;
            if (PollLegacyCollection(*c, m.name, s)) {
                ctx.Add(s, kVidAstro, m.pid);
                break;
            }
        }
    }
}
