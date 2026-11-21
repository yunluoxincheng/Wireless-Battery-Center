#include "devicemodel.h"
#include "windowframe.h"
#include "../src/panel_lifecycle.h"
#include "../src/product.h"
#include "../src/settings.h"

#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTranslator>
#include <QWindow>

namespace {

bool EnsureBackground() {
    const HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, panel::BackgroundMutex);
    if (running) {
        CloseHandle(running);
        return true;
    }
    const QString directory = QCoreApplication::applicationDirPath();
    return QProcess::startDetached(directory + u'/' + QString::fromWCharArray(product::BackgroundExe), {}, directory);
}

int RunPanel(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("WirelessBatteryCenter"));
    app.setApplicationName(QString::fromWCharArray(product::Name));
    if (!ImportLegacySettings(HKEY_CURRENT_USER, product::LegacyPanelSettingsKey, product::PanelSettingsKey, true))
        qWarning("Could not import Wireless Battery appearance settings.");
    const QIcon icon(QStringLiteral(":/branding/app.ico"));
    app.setWindowIcon(icon);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QTranslator translator;
    const QLocale locale = QLocale::system();
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"), locale.script() == QLocale::TraditionalHanScript
        ? QStringLiteral("Microsoft JhengHei UI") : QStringLiteral("Microsoft YaHei UI")});
    app.setFont(font);
    if (locale.language() == QLocale::Chinese) {
        const QString catalog = locale.script() == QLocale::TraditionalHanScript
            ? QStringLiteral("panel_zh_TW") : QStringLiteral("panel_zh_CN");
        if (translator.load(catalog, QStringLiteral(":/i18n"))) app.installTranslator(&translator);
    }

    DeviceModel model;
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("devices"), QVariant::fromValue(&model)}});
    engine.loadFromModule(QStringLiteral("WirelessBattery"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QWindow*>(engine.rootObjects().constFirst());
    window->setFlag(Qt::FramelessWindowHint);
    window->setIcon(icon);
    WindowFrame frame(*window);
    QObject::connect(window, SIGNAL(toggleMaximizeRequested()), &frame, SLOT(toggleMaximized()));
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    SetPropW(hwnd, panel::PanelWindowProperty, reinterpret_cast<HANDLE>(1));
    window->setProperty("backgroundStartFailed", !EnsureBackground());
    window->show();
    const int result = app.exec();
    RemovePropW(hwnd, panel::PanelWindowProperty);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, panel::PanelMutex);
    if (!mutex) return 1;
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    int result = 0;
    if (alreadyRunning) panel::ActivatePanelWindow();
    else result = RunPanel(argc, argv);
    CloseHandle(mutex);
    return result;
}
