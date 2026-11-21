#include "driverlauncher.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <windows.h>

namespace {

struct Driver {
    const char* id;
    const char* label;
    const char* executable;
    const char* displayName;
    const char* publisher;
    const char* web;
    const char* support;
};

constexpr Driver atk{"atk", "ATK HUB", "ATK HUB.exe", R"(ATK (?:V )?HUB(?:\s*[\d.]+)?)", "ATK",
                     "https://desktop.atkgear.com/", "https://www.atk.store/blogs/news/atk-v-hub-instructions"};
constexpr Driver hecate{"hecate-g5", "HECATE", "HECATE.exe",
                        R"(HECATE(?:\s+(?:version|版本)?\s*[\d.]+)?)", "EDIFIER Esports Technology Co.,Ltd",
                        "", "https://cnweb.edifier.com/driver.html"};
constexpr Driver vgn{"vgn", "VGN V-HUB", "VGN V-HUB.exe", R"(VGN V-HUB(?:\s*[\d.]+)?)", "VGN",
                     "", "https://download.vgnlab.com.cn/"};
constexpr Driver logitech{"logitech-g", "Logitech G HUB", "lghub.exe", R"(Logitech G HUB(?:\s*[\d.]+)?)", "Logitech",
                          "", "https://www.logitechg.com/innovation/g-hub.html"};

const Driver* Match(const QVariantMap& device) {
    const int vid = device.value(QStringLiteral("vid")).toInt();
    const int pid = device.value(QStringLiteral("pid")).toInt();
    const QString name = device.value(QStringLiteral("name")).toString();
    if ((vid == 0x373B && (pid == 0x1015 || pid == 0x101B)) ||
        (vid == 0x320F && name == QStringLiteral("ATK V75X"))) return &atk;
    if (vid == 0x35BB && pid == 0xA008) return &hecate;
    if (vid == 0x320F && pid == 0x5088 && name == QStringLiteral("VGN V98Pro V2")) return &vgn;
    if (vid == 0x046D && (pid == 0xC084 || pid == 0xC092 || pid == 0xC09D)) return &logitech;
    return nullptr;
}

QString ClientSetting(const Driver& driver) {
    return QStringLiteral("Drivers/%1/client").arg(QLatin1StringView(driver.id));
}

bool LocalDrivePath(const QString& path) {
    if (path.size() < 4 || !path[0].isLetter() || path[1] != u':' || path[2] != u'/' ||
        path.indexOf(u':', 2) != -1 || path.contains(QChar::Null)) return false;
    const auto root = QDir::toNativeSeparators(path.first(3)).toStdWString();
    return GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
}

QString LocalExecutable(const QString& path) {
    const auto normalized = QDir::fromNativeSeparators(path.trimmed());
    if (!LocalDrivePath(normalized) || !normalized.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) return {};
    const QFileInfo file(normalized);
    const QString resolved = file.canonicalFilePath();
    if (!file.isFile() || !LocalDrivePath(resolved) ||
        !resolved.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) return {};
    DWORD type;
    return GetBinaryTypeW(QDir::toNativeSeparators(resolved).toStdWString().c_str(), &type) &&
        (type == SCS_32BIT_BINARY || type == SCS_64BIT_BINARY) ? resolved : QString();
}

QString DisplayIconPath(QString icon) {
    // DisplayIcon contains a path and optional resource index, never a launch command.
    icon.remove(QRegularExpression(QStringLiteral(R"(,\s*-?\d+\s*$)")));
    icon = icon.trimmed();
    if (icon.startsWith(u'"') && icon.endsWith(u'"')) icon = icon.sliced(1, icon.size() - 2);
    return icon;
}

QString FindInstalled(const Driver& driver) {
    const QString executable = QString::fromLatin1(driver.executable);
    const QRegularExpression name(
        QRegularExpression::anchoredPattern(QString::fromLatin1(driver.displayName)),
        QRegularExpression::CaseInsensitiveOption);
    const QString publisher = QString::fromLatin1(driver.publisher);
    for (auto root : {QStringLiteral("HKEY_CURRENT_USER"), QStringLiteral("HKEY_LOCAL_MACHINE")}) {
        for (auto format : {QSettings::Registry64Format, QSettings::Registry32Format}) {
            QSettings apps(root + QStringLiteral("\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"), format);
            for (const auto& item : apps.childGroups()) {
                apps.beginGroup(item);
                const bool matches = name.match(apps.value(QStringLiteral("DisplayName")).toString()).hasMatch() &&
                    apps.value(QStringLiteral("Publisher")).toString().trimmed().compare(publisher, Qt::CaseInsensitive) == 0;
                QString path;
                if (matches) {
                    const auto icon = DisplayIconPath(apps.value(QStringLiteral("DisplayIcon")).toString());
                    if (QFileInfo(icon).fileName().compare(executable, Qt::CaseInsensitive) == 0)
                        path = LocalExecutable(icon);
                    if (path.isEmpty()) {
                        const auto location = apps.value(QStringLiteral("InstallLocation")).toString().trimmed();
                        if (!location.isEmpty()) path = LocalExecutable(QDir(location).filePath(executable));
                    }
                }
                apps.endGroup();
                if (!path.isEmpty()) return path;
            }
        }
    }
    return {};
}

} // namespace

QVariantMap DriverLauncher::options(const QVariantMap& device) {
    const auto* driver = Match(device);
    if (!driver) return {};
    const auto id = QString::fromLatin1(driver->id);
    if (!m_installed.contains(id)) m_installed.insert(id, FindInstalled(*driver));
    const auto manual = QSettings().value(ClientSetting(*driver)).toString();
    const auto selected = LocalExecutable(manual);
    return {{QStringLiteral("label"), QString::fromLatin1(driver->label)},
            {QStringLiteral("client"), selected.isEmpty() ? m_installed.value(id) : selected},
            {QStringLiteral("manual"), manual},
            {QStringLiteral("invalid"), !manual.isEmpty() && selected.isEmpty()},
            {QStringLiteral("web"), *driver->web != '\0'},
            {QStringLiteral("support"), *driver->support != '\0'}};
}

QString DriverLauncher::setClient(const QVariantMap& device, const QUrl& file) {
    const auto* driver = Match(device);
    if (!driver) return tr("No verified official driver is available.");
    const auto path = LocalExecutable(file.toLocalFile());
    if (!file.isEmpty() && path.isEmpty()) return tr("Choose an existing local Windows executable (.exe).");
    QSettings settings;
    if (path.isEmpty()) settings.remove(ClientSetting(*driver));
    else settings.setValue(ClientSetting(*driver), path);
    settings.sync();
    m_installed.remove(QString::fromLatin1(driver->id));
    return settings.status() == QSettings::NoError ? QString() : tr("Could not save the client path.");
}

QString DriverLauncher::open(const QVariantMap& device, const QString& route) {
    const auto* driver = Match(device);
    if (!driver) return tr("No verified official driver is available.");
    const auto browse = [](const char* url) {
        return *url && QDesktopServices::openUrl(QUrl(QString::fromLatin1(url)));
    };
    if (route == QStringLiteral("web"))
        return browse(driver->web) ? QString() : tr("Could not open the web driver.");
    if (route == QStringLiteral("support"))
        return browse(driver->support) ? QString() : tr("Could not open official support.");

    m_installed.remove(QString::fromLatin1(driver->id));
    const auto choices = options(device);
    const auto client = choices.value(QStringLiteral("client")).toString();
    if (!client.isEmpty() && QProcess::startDetached(client, {}, QFileInfo(client).absolutePath())) return {};
    const bool unavailable = choices.value(QStringLiteral("invalid")).toBool();
    const bool failed = !client.isEmpty();
    if (browse(driver->web)) {
        if (failed) return tr("Client could not start. Opened the official web driver.");
        return unavailable ? tr("Saved client is unavailable. Opened the official web driver.") : QString();
    }
    if (browse(driver->support)) {
        if (failed) return tr("Client could not start. Opened official support.");
        if (unavailable) return tr("Saved client is unavailable. Opened official support.");
        return *driver->web ? tr("Web driver could not open. Opened official support.") : QString();
    }
    return failed ? tr("Client could not start and no official page could be opened.")
                  : tr("Could not open an official driver.");
}
