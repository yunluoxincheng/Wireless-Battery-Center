#include "../panel/driverlauncher.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QtEndian>
#include <windows.h>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}

class Browser : public QObject {
    Q_OBJECT
public:
    QUrl url;
public slots:
    void capture(const QUrl& value) { url = value; }
};

} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsSet("WIRELESS_BATTERY_LAUNCH_TEST_CHILD")) return 0;
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("WirelessBatteryTest"));
    app.setApplicationName(QStringLiteral("DriverLauncher"));
    QTemporaryDir temp;
    Check(temp.isValid(), "Could not create isolated test preferences");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());

    // HKLM and HKCU overrides only affect this process. Use fabricated registration data
    // so the test never launches an actual installed vendor utility.
    const auto keyName = L"Software\\WirelessBattery.LaunchTest." + std::to_wstring(GetCurrentProcessId());
    HKEY registry;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                          KEY_ALL_ACCESS, nullptr, &registry, nullptr) == ERROR_SUCCESS, "Test registry");
    HKEY entry;
    Check(RegCreateKeyExW(registry, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ATK",
                          0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &entry, nullptr) == ERROR_SUCCESS,
          "Test uninstall registration");
    RegCloseKey(entry);
    Check(RegOverridePredefKey(HKEY_CURRENT_USER, registry) == ERROR_SUCCESS, "HKCU override");
    Check(RegOverridePredefKey(HKEY_LOCAL_MACHINE, registry) == ERROR_SUCCESS, "HKLM override");
    Browser browser;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "capture");

    const QVariantMap atk{{QStringLiteral("vid"), 0x373B}, {QStringLiteral("pid"), 0x1015}};
    const QVariantMap hecate{{QStringLiteral("vid"), 0x35BB}, {QStringLiteral("pid"), 0xA008}};
    DriverLauncher launcher;
    Check(launcher.options({}).isEmpty(), "Missing driver mapping");
    Check(!launcher.options(hecate).value(QStringLiteral("web")).toBool(), "HECATE should have support only");
    Check(launcher.open(atk, QStringLiteral("preferred")).isEmpty(), "Web fallback");
    Check(browser.url.host() == QStringLiteral("desktop.atkgear.com"), "ATK web host");

    const auto executable = QCoreApplication::applicationFilePath();
    const auto installed = temp.filePath(QStringLiteral("ATK HUB.exe"));
    Check(QFile::copy(executable, installed), "Fake installed client");
    {
        QSettings apps(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
                       QSettings::Registry64Format);
        apps.setValue(QStringLiteral("ATK/DisplayName"), QStringLiteral("ATK HUB3.2.33"));
        apps.setValue(QStringLiteral("ATK/Publisher"), QStringLiteral("ATK"));
        apps.setValue(QStringLiteral("ATK/DisplayIcon"), QStringLiteral("\"%1\",0").arg(installed));
        apps.sync();
        Check(apps.status() == QSettings::NoError, "Write fake client registration");
    }
    DriverLauncher afterInstall;
    Check(afterInstall.options(atk).value(QStringLiteral("client")).toString() == QFileInfo(installed).canonicalFilePath(),
          "Registered client detection");
    Check(afterInstall.setClient(atk, QUrl::fromLocalFile(temp.filePath(QStringLiteral("missing.exe")))).size() > 0,
          "Invalid path rejection");

    const auto manual = temp.filePath(QStringLiteral("My client.exe"));
    Check(QFile::copy(executable, manual), "Fake manually selected client");
    Check(afterInstall.setClient(atk, QUrl::fromLocalFile(manual)).isEmpty(), "Save manual path");
    DriverLauncher reopened;
    Check(reopened.options(atk).value(QStringLiteral("client")).toString() == QFileInfo(manual).canonicalFilePath(),
          "Manual path priority and persistence");
    Check(QFile::remove(manual) && QFile::remove(installed), "Remove test files");
    Check(reopened.open(atk, QStringLiteral("preferred")).contains(QStringLiteral("unavailable")),
          "Missing client should explain fallback");
    Check(reopened.setClient(atk, {}).isEmpty(), "Restore detection");

    const auto blocked = temp.filePath(QStringLiteral("Native client.exe"));
    QFile source(executable);
    Check(source.open(QIODevice::ReadOnly), "Read test executable");
    auto native = source.readAll();
    const auto peOffset = qFromLittleEndian<quint32>(native.constData() + 0x3c);
    qToLittleEndian<quint16>(IMAGE_SUBSYSTEM_NATIVE, native.data() + peOffset + 24 + 68);
    QFile fixture(blocked);
    Check(fixture.open(QIODevice::WriteOnly) && fixture.write(native) == native.size(), "Write non-launchable PE");
    fixture.close();
    qputenv("WIRELESS_BATTERY_LAUNCH_TEST_CHILD", "1");
    Check(reopened.setClient(atk, QUrl::fromLocalFile(blocked)).isEmpty(), "PE fixture should pass file validation");
    Check(reopened.open(atk, QStringLiteral("preferred")).contains(QStringLiteral("could not start")),
          "Failed client launch should explain fallback");
    Check(reopened.open(hecate, QStringLiteral("preferred")).isEmpty(), "Official support fallback");
    Check(browser.url.host() == QStringLiteral("cnweb.edifier.com"), "HECATE support host");

    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    RegCloseKey(registry);
    qInfo("Driver detection, path persistence and launch fallbacks passed.");
}

#include "driver_launcher_test.moc"
