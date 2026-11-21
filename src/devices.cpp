// Runs every driver in order. Vendor drivers go first and claim the devices they handle; the
// generic fallbacks then only look at what is left.
#include "devices.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>

#include "drivers/drivers.h"
#include "report.h"
#include "hid.h"
#include "product.h"

namespace {

void IdentifyPanelDevices(PollContext& ctx, std::set<std::wstring>& endpoints) {
    using Product = std::pair<USHORT, USHORT>;
    std::map<Product, std::set<std::wstring>> containers;
    for (const auto& c : ctx.colls) {
        auto identity = HidIdentity(c);
        if (!identity.empty()) endpoints.insert(identity);
        containers[{c.vid, c.pid}].insert(std::move(identity));
    }

    std::set<std::wstring> reported;
    std::map<Product, size_t> counts;
    for (const auto& s : ctx.out) ++counts[{s.vid, s.pid}];
    for (auto& s : ctx.out) {
        if (s.identity.empty() && counts[{s.vid, s.pid}] == 1 &&
            std::strcmp(s.protocol, "XInput") && std::strcmp(s.protocol, "Logitech HID++")) {
            const auto found = containers.find({s.vid, s.pid});
            if (found != containers.end() && found->second.size() == 1)
                s.identity = *found->second.begin();
        }
        if (!s.identity.empty()) reported.insert(s.identity);
    }

    // Exact, verified USB products only. A HID mouse/keyboard collection by itself could
    // belong to a system node, a multimedia interface or an idle wireless receiver.
    static constexpr struct {
        USHORT vid, pid;
        const wchar_t* name;
        DeviceKind kind;
        BatteryCapability battery;
        bool receiver;
    } known[] = {
        {0x35BB, 0xA008, L"HECATE G5 Wireless", DeviceKind::Headset, BatteryCapability::Unsupported, true},
        {0x413C, 0x2113, L"Dell KB216", DeviceKind::Keyboard, BatteryCapability::None, false},
        {0x046D, 0xC084, L"Logitech G203", DeviceKind::Mouse, BatteryCapability::None, false},
        {0x046D, 0xC092, L"Logitech G102/G203 LIGHTSYNC", DeviceKind::Mouse, BatteryCapability::None, false},
        {0x046D, 0xC09D, L"Logitech G102 LIGHTSYNC", DeviceKind::Mouse, BatteryCapability::None, false},
    };
    for (const auto& c : ctx.colls) {
        const auto model = std::find_if(std::begin(known), std::end(known), [&](const auto& item) {
            return c.vid == item.vid && c.pid == item.pid;
        });
        if (model == std::end(known)) continue;
        auto identity = HidIdentity(c);
        if (identity.empty() || !reported.insert(identity).second) continue;
        DeviceStatus s;
        s.key = s.identity = std::move(identity);
        s.name = model->name;
        s.kind = model->kind;
        s.vid = c.vid;
        s.pid = c.pid;
        s.via = model->receiver ? L"2.4 GHz receiver" : L"USB cable";
        s.wirelessLink = model->receiver ? WirelessLink::Unknown : WirelessLink::Unreported;
        s.battery = model->battery;
        s.panelOnly = true;
        s.protocol = "Windows HID";
        ctx.out.push_back(std::move(s));
    }
}

// IgnoreDevices (REG_SZ): "VID:PID" pairs in hex, separated by spaces,
// commas or semicolons (e.g. "28BD:0947, 046D:C547"). No driver ever opens those devices (issue #232).
std::vector<std::pair<USHORT, USHORT>> IgnoredDevices() {
    std::vector<std::pair<USHORT, USHORT>> out;
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    const auto* path = product::SettingsKey;
    HKEY settings;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &settings);
    if (opened == ERROR_SUCCESS) RegCloseKey(settings);
    else if (opened == ERROR_FILE_NOT_FOUND) path = product::LegacySettingsKey; // CLI before first import
    if (RegGetValueW(HKEY_CURRENT_USER, path, L"IgnoreDevices", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buf; *p;) {
        unsigned vid = 0, pid = 0;
        int used = 0;
        if (swscanf(p, L" %x:%x%n", &vid, &pid, &used) == 2 && used > 0) {
            out.push_back({ (USHORT)vid, (USHORT)pid });
            p += used;
        } else {
            p++;
        }
        while (*p == L',' || *p == L';' || *p == L' ') p++;
    }
    return out;
}

}  // namespace

std::vector<DeviceStatus> PollAllDevices(bool manualRefresh, std::set<std::wstring>* endpoints) {
    PollContext ctx;
    ctx.manualRefresh = manualRefresh;
    ctx.colls = EnumHid();
    auto ignored = IgnoredDevices();
    if (!ignored.empty())
        ctx.colls.erase(std::remove_if(ctx.colls.begin(), ctx.colls.end(), [&](const HidColl& c) {
                            return std::find(ignored.begin(), ignored.end(), std::make_pair(c.vid, c.pid)) != ignored.end();
                        }),
                        ctx.colls.end());
#define RUN_DRIVER(driver) run(driver, #driver + 4)
    auto run = [&](void (*driver)(PollContext&), const char* protocol) {
        const size_t first = ctx.out.size();
        driver(ctx);
        for (size_t i = first; i < ctx.out.size(); ++i) ctx.out[i].protocol = protocol;
    };
    RUN_DRIVER(PollGlorious);
    RUN_DRIVER(PollAula);       // before Kysona, which shares the 3554 vendor id and would probe it first
    RUN_DRIVER(PollDareu);
    RUN_DRIVER(PollDarmoshark);
    RUN_DRIVER(PollAtkSerial);  // before Kysona, which shares the ATK vendor id
    RUN_DRIVER(PollAtkKeyboard);
    RUN_DRIVER(PollPulsar);     // before Kysona, which also scans the 3710 vendor id
    RUN_DRIVER(PollAtkBitmouse);  // before Kysona, which would probe the NK dongle with the wrong protocol
    RUN_DRIVER(PollKysona);
    RUN_DRIVER(PollKeychron);
    RUN_DRIVER(PollCherry);
    RUN_DRIVER(PollArbiter);    // before Lamzu, which shares the 373E vendor id
    RUN_DRIVER(PollLamzu);
    RUN_DRIVER(PollEndgame);
    RUN_DRIVER(PollEndgameWe);
    RUN_DRIVER(PollVaxee);
    RUN_DRIVER(PollWlmouse);
    RUN_DRIVER(PollFinalmouse);
    RUN_DRIVER(PollNinjutso);
    RUN_DRIVER(PollSprime);
    RUN_DRIVER(PollRongyuan);
    RUN_DRIVER(PollSonix);
    RUN_DRIVER(PollSemico);
    RUN_DRIVER(PollSonyInzone);
    RUN_DRIVER(PollFractal);
    RUN_DRIVER(PollJabra);
    RUN_DRIVER(PollJbl);
    RUN_DRIVER(PollRog);
    RUN_DRIVER(PollZowie);
    RUN_DRIVER(PollRoyalKludge);
    RUN_DRIVER(PollMchose);
    RUN_DRIVER(PollRapoo);
    RUN_DRIVER(PollAreson);
    RUN_DRIVER(PollMchoseG3);
    RUN_DRIVER(PollMsi);
    RUN_DRIVER(PollMachenike);  // before Ajazz in PollMiscMice, which shares the L7's 248A vendor id
    RUN_DRIVER(PollNiceboy);    // exact 248A:FA02 match, also before the Ajazz / Delux pollers
    RUN_DRIVER(PollMchoseModern);
    RUN_DRIVER(PollMchoseHeadset);
    RUN_DRIVER(PollMiscMice);
    RUN_DRIVER(PollFantech);
    RUN_DRIVER(PollRazerHeadsets);  // before the generic Razer driver, which shares the vendor id
    RUN_DRIVER(PollBarracudaPro);   // before the generic Razer driver as well
    RUN_DRIVER(PollHyperX);
    RUN_DRIVER(PollLogitechG435);  // before the HID++ driver, which shares the vendor id
    RUN_DRIVER(PollAstro);
    run(PollLogitech, "Logitech HID++");
    RUN_DRIVER(PollRazer);
    RUN_DRIVER(PollSteelSeriesElite);
    RUN_DRIVER(PollSteelSeries);
    RUN_DRIVER(PollAudeze);
    RUN_DRIVER(PollNzxt);
    RUN_DRIVER(PollPlantronics);
    RUN_DRIVER(PollCorsair);
    RUN_DRIVER(PollCoolerMaster);
    RUN_DRIVER(PollPlayStation);
    RUN_DRIVER(PollSwitchPro);
    RUN_DRIVER(PollSteamController);
    RUN_DRIVER(PollGameSir);
    RUN_DRIVER(PollFlydigi);
    RUN_DRIVER(PollGameSirCyclone);
    RUN_DRIVER(Poll8BitDo);
    RUN_DRIVER(PollBeitong);  // before the XInput fallback, which only sees the receiver as wired
    run(PollAirPods, "Apple BLE");  // before the Bluetooth fallback, which has no level for AirPods
    run(PollPhoneLink, "Microsoft Phone Link");
    run(PollGalaxyBuds, "Samsung RFCOMM");  // before the Bluetooth fallback, which skips devices already listed by name
    RUN_DRIVER(PollMoondrop);
    RUN_DRIVER(PollAlienware);
    RUN_DRIVER(PollMaono);  // MAONO PD200W receiver (352F:040E), experimental battery query
    RUN_DRIVER(PollHeadsetControl);
    run(PollBluetooth, "Windows Bluetooth");   // before the generic HID check: Windows' own level wins for the same device
    run(PollGenericHid, "HID battery report");
    RUN_DRIVER(PollXInput);
#undef RUN_DRIVER
    if (endpoints) IdentifyPanelDevices(ctx, *endpoints);
    return ctx.out;
}

std::wstring DumpHidCollections() {
    std::wstring out = L"VID:PID    IF  UsagePg:Usage  In  Out Feat  Product\n";
    for (const auto& c : EnumHid()) {
        wchar_t line[160];
        swprintf(line, 160, L"%04X:%04X  %2d  %04X:%04X    %3u %3u %3u   ", c.vid, c.pid, c.iface, c.page(), c.usage(), c.inLen(), c.outLen(),
                 c.featLen());
        out += line + HidProduct(c) + L"\n";
    }
    out += report::DumpExtras();  // unopenable HID collections, non-HID USB devices, Bluetooth devices
    out += DumpXentaFrames();
    return out;
}
