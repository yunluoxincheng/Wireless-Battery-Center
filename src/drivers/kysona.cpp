// Kysona, Pulsar and ATK / VXE wireless mice (VID 3554, ATK dongles 373B), G-Wolves models on this protocol
// (VID 33E4, e.g. Fenrir Asym) and the original Hitscan
// Hyperlight (VID 3770, verified on the 1K receiver 3770:0200), Pulsar's newer XS-1 generation
// (VID 3710, verified on the 8K Dongle 3710:5406) and Redragon's Compx-based mice (M916 Pro 1K,
// M913 tri-mode, G49: receiver 3554:F55D / F5D5, cable F55E; verified on an M916 Pro 1K through
// the F55D receiver). They share one protocol:
// 17-byte output/input report id 08: [08][cmd][0 x14][checksum], checksum = 0x55 - sum(bytes 0..15).
//   cmd 03 = online status -> reply [6] = 1 when the mouse is connected to the dongle
//   cmd 04 = battery       -> reply [6] = percent, [7] = charging, [8..9] = millivolts
// ATK VXE Dragonfly R1 family (receivers 3554:F58A / F58E / F505, cables F58C / F58F / F506) uses the same
// protocol on vendor collection FF02:0002; its receivers expose several other 17-byte vendor collections
// (FF03..FF06), so FF02:0002 is tried first. IDs and protocol from github.com/S1berla/vxe-r1-pro-max-battery,
// github.com/orcunturkol/BatteryTray (PR #1) and HeyOkay/HaloBattery issue #273 (verified on F505).
// Reference: Linux drivers/hid/hid-kysona.c (Lode Willems) and the hid-pulsar patch discussion;
// Redragon ids from github.com/EliottV17/redragon-battery-omarchy and github.com/pingpingjinping/g49-web-control.
// The older dual-mode M913 on the Areson 25A7:FA07 / FA08 receiver is handled by areson.cpp.
#include "drivers.h"
#ifdef _WIN32
#include "../atk8k.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {

// Redragon uses Compx's own ids, and its receiver only calls itself "2.4G Wireless Receiver".
bool IsRedragon(USHORT vid, USHORT pid) { return vid == 0x3554 && (pid == 0xF55D || pid == 0xF5D5 || pid == 0xF55E); }

bool IsVxeR1(USHORT vid, USHORT pid) {
    return vid == 0x3554 &&
           (pid == 0xF58A || pid == 0xF58C || pid == 0xF58E || pid == 0xF58F || pid == 0xF505 || pid == 0xF506);
}

// A VXE R1 cable reports under its receiver's id, so the mouse is one entry on either connection.
// F58A / F58C verified; the other pairs follow the order PR #69 lists them in.
USHORT KeyPid(USHORT vid, USHORT pid) {
    if (vid == 0x3554 && pid == 0xF58C) return 0xF58A;
    if (vid == 0x3554 && pid == 0xF58F) return 0xF58E;
    if (vid == 0x3554 && pid == 0xF506) return 0xF505;
    return pid;
}

// HECATE (Edifier) also sells headsets under 35BB (G2 Wireless 7.1: dongles A009 / A209) with a 64-byte
// FF02:0002 vendor collection that speaks something else; only the mouse ids from HECATE Connect's
// Config.ini (dongle D3FB, mouse over cable D3FA / D3FC / D3FD) get Kysona frames.
bool IsHecateMouse(USHORT pid) { return pid == 0xD3FA || pid == 0xD3FB || pid == 0xD3FC || pid == 0xD3FD; }

bool IsCable(USHORT vid, USHORT pid) {
    bool hecate_cable = vid == 0x35BB && (pid == 0xD3FA || pid == 0xD3FC || pid == 0xD3FD);
    return (vid == 0x33E4 && GWolvesIsCable(pid)) || hecate_cable ||
           (vid == 0x3554 && (pid == 0xF55E || pid == 0xF58C || pid == 0xF58F || pid == 0xF506));
}

BYTE Checksum(const BYTE* b) {
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) sum += b[i];
    return (BYTE)(0x55 - sum);
}

// Set WIRELESS_BATTERY_ATK_TRACE to a log file path before starting the Windows backend.
std::ofstream OpenAtkTrace() {
#ifdef _WIN32
    if (const auto* path = _wgetenv(L"WIRELESS_BATTERY_ATK_TRACE"); path && *path)
        return std::ofstream(std::filesystem::path(path), std::ios::app);
#endif
    return {};
}

auto UnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void TraceCollection(std::ostream& trace, const HidColl& c) {
    // The MI and COL identifiers identify the collection without logging the device instance path.
    const auto pos = c.path.find(L"&col");
    trace << "collection mi=" << c.iface << " col=";
    if (pos != std::wstring::npos) {
        const auto id = c.path.substr(pos + 4, 2);
        trace << std::string(id.begin(), id.end());
    } else {
        trace << '-';
    }
    trace << std::hex << " page=" << c.page() << " usage=" << c.usage() << std::dec
          << " in=" << c.inLen() << " out=" << c.outLen() << " feature=" << c.featLen() << '\n';
}

// Sends `cmd` and waits for its checksummed reply.
bool Query(HidDev& d, BYTE cmd, std::vector<BYTE>& r, std::ostream* trace, int attempt = 1,
           bool* pendingBattery = nullptr) {
    std::vector<BYTE> req(17, 0);
    req[0] = 0x08;
    req[1] = cmd;
    req[16] = Checksum(req.data());
    using Clock = std::chrono::steady_clock;
    const auto flushStart = trace ? Clock::now() : Clock::time_point{};
    // Keep queued replies when an earlier ATK request might still answer.
    if (!pendingBattery || !*pendingBattery) d.Flush();
    const auto sendTime = trace ? UnixMs() : 0;
    const auto sendStart = trace ? Clock::now() : Clock::time_point{};
    const auto elapsed = [](auto start, auto finish) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count();
    };
    const bool written = d.Write(req);
    if (written && pendingBattery && cmd == 0x04) *pendingBattery = true;
    ULONGLONG end = GetTickCount64() + 500;
    if (trace) {
        *trace << "cmd=0x0" << (int)cmd << " attempt=" << attempt << " tx_unix_ms=" << sendTime
               << " flush_ms=" << elapsed(flushStart, sendStart) << " write_ms=" << elapsed(sendStart, Clock::now())
               << " result=" << (written ? "written" : "write_failed") << '\n';
    }
    if (!written) return false;
    for (;;) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0) {
            if (trace) *trace << "rx_ms=" << elapsed(sendStart, Clock::now()) << " result=deadline\n";
            return false;
        }
#ifdef _WIN32
        HidDev::ReadDiagnostics read;
        const int size = trace ? d.Read(r, left, &read) : d.Read(r, left);
#else
        const int size = d.Read(r, left);
#endif
        const char* result = size < 0 ? "read_failed" : size < 17 ? "short_reply"
            : r[0] != 0x08 ? "report_id_mismatch" : r[1] != cmd ? "command_mismatch"
            : r[16] != Checksum(r.data()) ? "checksum_error" : nullptr;
        if (trace) {
#ifdef _WIN32
            if (read.timedOut) result = "read_timeout";
#endif
            *trace << "rx_ms=" << elapsed(sendStart, Clock::now()) << " result=" << (result ? result : "ok");
            if (size >= 0) {
                *trace << " raw=";
#ifdef _WIN32
                if (read.strippedReportId) *trace << "00";
#endif
                constexpr char hex[] = "0123456789ABCDEF";
                for (BYTE b : r) *trace << hex[b >> 4] << hex[b & 0x0F];
            }
#ifdef _WIN32
            else *trace << " winerror=" << read.error;
#endif
            *trace << '\n';
        }
        if (size < 17) return false;
        if (pendingBattery && r[0] == 0x08 && r[1] == 0x04 && r[16] == Checksum(r.data())) {
            *pendingBattery = false;
            if (trace && cmd != 0x04) *trace << "discarded_late_battery\n";
        }
        if (!result) return true;
    }
}

bool QueryBattery(HidDev& d, std::vector<BYTE>& reply, DeviceStatus& status, std::ostream* trace, int attempt,
                  bool* pendingBattery = nullptr) {
    if (!Query(d, 0x04, reply, trace, attempt, pendingBattery)) return false;
    status.connected = true;
    if (reply[6] <= 100) status.percent = reply[6];
    status.charging = reply[7] != 0;
    status.millivolts = (reply[8] << 8) | reply[9];
    if (trace) *trace << "battery=" << (status.percent >= 0 ? "valid" : "invalid_level") << " level=" << (int)reply[6] << '\n';
    return true;
}

std::wstring CleanName(std::wstring n, USHORT vid, USHORT pid) {
    if (vid == 0x33E4) return GWolvesModelName(pid);
    if (IsRedragon(vid, pid)) return L"Redragon wireless mouse";  // shared by several models
    if (IsVxeR1(vid, pid)) return L"VXE Dragonfly R1";  // the receivers call themselves "VXE NordicMouse 1K Dongle" etc.
    for (const wchar_t* suffix : { L" Dongle", L" Receiver", L" 2.4G", L" Wireless Receiver" }) {
        size_t l = wcslen(suffix);
        if (n.size() > l && n.compare(n.size() - l, l, suffix) == 0) n = n.substr(0, n.size() - l);
    }
    if (vid == 0x3770) return L"Hitscan Hyperlight";  // the receiver only calls itself "Hitscan USB Receiver"
    if (vid == 0x35BB) return L"HECATE mouse";
    if (n.empty()) n = vid == 0x373B ? L"ATK wireless mouse" : L"Wireless mouse";
    return n;
}

void DescribeMouse(DeviceStatus& s, const HidColl& c, const PollContext& ctx) {
    s.key = L"kysona:" + Hex4(c.vid) + L":" + Hex4(KeyPid(c.vid, c.pid));
    s.name = CleanName(HidProduct(c), c.vid, c.pid);
    s.kind = KindFromCollections(ctx.colls, c.vid, c.pid);
    if (s.kind == DeviceKind::Other) s.kind = DeviceKind::Mouse;
    s.via = IsCable(c.vid, c.pid) ? L"USB cable" : L"2.4 GHz receiver";
    s.identity = HidIdentity(c);
}

#ifdef _WIN32
struct AtkReader {
    HidDev device;
    HidColl collection;
    DeviceStatus identity;
    bool pendingBattery = false;
    bool activityUnambiguous = false;
};
AtkReader atkReader;  // Owned by the poll worker (or the standalone --list scan).

bool IsAtkChannel(const HidColl& c) {
    return c.vid == atk8k::VendorId && c.pid == atk8k::ProductId &&
           c.page() == 0xFF02 && c.usage() == 2 && c.inLen() == 17 && c.outLen() == 17;
}

void DrainAtkBatteryReply(HidDev& d, bool& pending, std::ostream* trace) {
    if (!pending) return;
    const auto end = GetTickCount64() + 25;
    std::vector<BYTE> reply;
    while (GetTickCount64() < end) {
        if (d.Read(reply, 25) < 0) break;
        if (reply.size() == 17 && reply[0] == 0x08 && reply[1] == 0x04 && reply[16] == Checksum(reply.data())) {
            pending = false;
            if (trace) *trace << "discarded_late_battery\n";
            break;
        }
    }
}

DeviceStatus ReadAtkBattery(ULONGLONG activityTick) {
    DeviceStatus status = atkReader.identity;
    auto& d = atkReader.device;
    auto& pending = atkReader.pendingBattery;
    auto traceFile = OpenAtkTrace();
    std::ostringstream traceBuffer;
    auto* trace = traceFile.is_open() ? &traceBuffer : nullptr;
    if (trace) {
        *trace << "scan vid=373B pid=101B unix_ms=" << UnixMs()
               << " source=" << (activityTick ? "activity" : "poll") << '\n';
    }
    const auto read = [&] {
        if (atkReader.collection.path.empty()) return "no_channel";
        if (trace) TraceCollection(*trace, atkReader.collection);
        if (!d.ok() && !d.Open(atkReader.collection)) return "open_failed";

        std::vector<BYTE> reply;
        if (!Query(d, 0x03, reply, trace, 1, &pending)) return "link_unknown";
        if (reply[6] == 0) status.wirelessLink = WirelessLink::Unlinked;
        if (reply[6] == 1) status.wirelessLink = WirelessLink::Linked;
        if (status.wirelessLink != WirelessLink::Linked) return "skipped_unlinked";

        // The protocol has no request ID. A lost reply cannot be distinguished from a late one
        // after another 04. Keep this handle and consume the old reply before sending a new 04.
        DrainAtkBatteryReply(d, pending, trace);
        if (pending) return "waiting_for_prior_reply";
        if (!QueryBattery(d, reply, status, trace, 1, &pending)) return "failed";
        if (status.percent < 0) {
            // An invalid level still completed the first transaction, so a retry is unambiguous.
            Sleep(50);
            if (Query(d, 0x03, reply, trace, 1, &pending) && reply[6] == 1)
                QueryBattery(d, reply, status, trace, 2, &pending);
        }
        if (status.percent < 0) {
            status.charging = false;
            return "invalid_level";
        }
        status.readingTimeMs = UnixMs();
        return "success";
    };
    const char* result = read();
    if (trace) {
        *trace << "scan_end battery=" << result;
        if (activityTick) *trace << " after_input_ms=" << GetTickCount64() - activityTick;
        *trace << '\n';
        traceFile << trace->str();
    }
    return status;
}
#endif

}  // namespace

#ifdef _WIN32
void atk8k::ReceiverRemoved() {
    atkReader.device.Close();
    atkReader.collection = {};
    atkReader.identity = {};
    atkReader.pendingBattery = false;
    atkReader.activityUnambiguous = false;
}

bool atk8k::CanReadAfterActivity() { return atkReader.activityUnambiguous; }

std::optional<DeviceStatus> atk8k::ReadAfterActivity(ULONGLONG activityTick) {
    if (!CanReadAfterActivity()) return std::nullopt;
    auto result = ReadAtkBattery(activityTick);
    return result.percent >= 0 ? std::optional<DeviceStatus>(std::move(result)) : std::nullopt;
}
#endif

void PollKysona(PollContext& ctx) {
#ifdef _WIN32
    const auto atkChannel = std::find_if(ctx.colls.begin(), ctx.colls.end(), IsAtkChannel);
    if (atkChannel != ctx.colls.end() && atkReader.collection.path != atkChannel->path) {
        atk8k::ReceiverRemoved();
        atkReader.collection = *atkChannel;
    } else if (atkChannel == ctx.colls.end() && !atkReader.collection.path.empty()) {
        atk8k::ReceiverRemoved();
    }
    // The stable device key is per product. Two identical receivers cannot be routed by that key.
    atkReader.activityUnambiguous = atkChannel != ctx.colls.end() &&
        std::find_if(std::next(atkChannel), ctx.colls.end(), IsAtkChannel) == ctx.colls.end();
#endif
    for (USHORT vendor : { (USHORT)0x3554, (USHORT)0x373B, (USHORT)0x3770, (USHORT)0x33E4, (USHORT)0x3710, (USHORT)0x35BB }) {
        for (auto [vid, pid] : ctx.Products(vendor)) {
#ifdef _WIN32
            if (vid == atk8k::VendorId && pid == atk8k::ProductId) {
                const auto receiver = std::find_if(ctx.colls.begin(), ctx.colls.end(), [=](const HidColl& c) {
                    return c.vid == vid && c.pid == pid;
                });
                DescribeMouse(atkReader.identity, atkChannel != ctx.colls.end() ? *atkChannel : *receiver, ctx);
                atkReader.identity.vid = vid;
                atkReader.identity.pid = pid;
                atkReader.identity.wirelessLink = WirelessLink::Unknown;
                atkReader.identity.protocol = "Kysona";
                auto status = ReadAtkBattery(0);
                ctx.Add(status, vid, pid);
                continue;
            }
#endif
            // ATK N9 Pro / N9 Ultra headsets speak a serial protocol (atk_serial.cpp); never send
            // them mouse frames, even when the ATK app holds their port and they stay unclaimed.
            if (vid == 0x373B && (pid == 0x11CA || pid == 0x11CB || pid == 0x1203 || pid == 0x1204)) continue;
            // 3554:FA09 / FB40 are the "BY" keyboard receivers (AULA, MCHOSE, also used by the Redragon
            // K673) that aula.cpp handles. Their FF02:0002 collection passes the size check below, so a
            // keyboard aula.cpp could not read got two mouse frames every poll, which kept a K673 from
            // going to sleep (issue #191). Never send them mouse frames.
            if (vid == 0x3554 && (pid == 0xFA09 || pid == 0xFB40)) continue;
            // Same for HECATE headsets (G2 Wireless 7.1, issue #1358): mouse frames only go to HECATE mouse ids.
            if (vid == 0x35BB && !IsHecateMouse(pid)) continue;
            // Kysona exposes a dedicated 17-byte collection; ATK / VXE receivers put report 08 in a
            // vendor collection that also carries larger reports. The checksummed reply filters out
            // anything that does not speak this protocol.
            auto colls = ctx.Find(vid, pid, [](const HidColl& c) {
                return (c.outLen() == 17 && c.inLen() == 17) || (c.page() >= 0xFF00 && c.outLen() >= 17 && c.inLen() >= 17);
            });
            // The battery channel is FF02:0002 where it exists; try it before the other vendor collections.
            std::stable_sort(colls.begin(), colls.end(), [](const HidColl* a, const HidColl* b) {
                return (a->page() == 0xFF02 && a->usage() == 0x0002) > (b->page() == 0xFF02 && b->usage() == 0x0002);
            });
            const bool atk8kReceiver = vid == 0x373B && pid == 0x101B;
            auto traceFile = atk8kReceiver ? OpenAtkTrace() : std::ofstream{};
            std::ostringstream traceBuffer;
            auto* trace = traceFile.is_open() ? &traceBuffer : nullptr;
            if (trace) *trace << "scan vid=373B pid=101B unix_ms=" << UnixMs() << '\n';
            bool retriedBattery = false;
            const char* batteryResult = "no_response";
            std::set<std::wstring> answeredEndpoints;
            for (const HidColl* c : colls) {
                const auto endpoint = HidIdentity(*c);
                if (answeredEndpoints.count(endpoint)) continue;
                HidDev d;
                if (trace) TraceCollection(*trace, *c);
                if (!d.Open(*c)) {
#ifdef _WIN32
                    if (trace) *trace << "open_failed winerror=" << GetLastError() << '\n';
#endif
                    continue;
                }
                std::vector<BYTE> r;
                DeviceStatus s;
                if (atk8kReceiver) s.wirelessLink = WirelessLink::Unknown;
                bool answered = false;
                if (Query(d, 0x03, r, trace)) {
                    answered = true;
                    if (atk8kReceiver) {
                        // 373B:101B returns the same 0x03 frame in idle and with the mouse switched off.
                        if (r[6] == 0) s.wirelessLink = WirelessLink::Unlinked;
                        if (r[6] == 1) s.wirelessLink = WirelessLink::Linked;
                        if (trace) *trace << "link_byte=" << (int)r[6] << '\n';
                    } else {
                        s.connected = r[6] != 0;
                        if (s.connected) s.wirelessLink = WirelessLink::Linked;
                    }
                }
                if (!answered || (s.connected && s.wirelessLink != WirelessLink::Unlinked)) {
                    answered = QueryBattery(d, r, s, trace, 1) || answered;
                    batteryResult = s.percent >= 0 ? "first_success" : "failed";
                    // The ATK 8K receiver can miss a battery reply while the mouse is active.
                    if (atk8kReceiver && s.percent < 0 && !retriedBattery) {
                        retriedBattery = true;
                        Sleep(50);
                        answered = QueryBattery(d, r, s, trace, 2) || answered;
                        if (s.percent >= 0) batteryResult = "retry_success";
                    }
                } else {
                    batteryResult = "skipped_unlinked";
                }
                // A failed battery request leaves the level unknown even when the receiver reports the mouse online.
                if (!answered) continue;
                answeredEndpoints.insert(endpoint);
                if (!s.connected) {
                    ctx.Claim(vid, pid);
                    continue;
                }
                DescribeMouse(s, *c, ctx);
                if (atk8kReceiver) {
                    // Always publish this receiver's own key while it is enumerated, with no grace after removal.
                    ctx.Add(s, vid, pid);
                    break;
                }
                s.graceSec = 1800;
                ctx.Add(s, vid, pid);
            }
            if (atk8kReceiver && !ctx.IsClaimed(vid, pid)) {
                // Products() saw this VID/PID even if every protocol exchange (or open) failed.
                const auto receiver = std::find_if(ctx.colls.begin(), ctx.colls.end(), [=](const HidColl& c) {
                    return c.vid == vid && c.pid == pid;
                });
                DeviceStatus s;
                DescribeMouse(s, colls.empty() ? *receiver : *colls.front(), ctx);
                s.wirelessLink = WirelessLink::Unknown;
                ctx.Add(s, vid, pid);
            }
            if (trace) {
                *trace << "scan_end battery=" << batteryResult << '\n';
                traceFile << trace->str();
            }
        }
    }
}
