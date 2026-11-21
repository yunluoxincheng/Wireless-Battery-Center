#pragma once

#include <QObject>
#include <QHash>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration>

class DriverLauncher : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    using QObject::QObject;
    Q_INVOKABLE QVariantMap options(const QVariantMap& device);
    Q_INVOKABLE QString setClient(const QVariantMap& device, const QUrl& file);
    Q_INVOKABLE QString open(const QVariantMap& device, const QString& route);

private:
    QHash<QString, QString> m_installed;
};
