#include "devicemodel.h"
#include "../src/panel_lifecycle.h"
#include "../src/panel_protocol.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace {
constexpr int DeviceRole = Qt::UserRole + 1;
}

DeviceModel::DeviceModel(QObject* parent) : QAbstractListModel(parent) {
    m_socket.setReadBufferSize(panel::MaxResponse);
    m_poll.setInterval(1200);
    m_timeout.setSingleShot(true);
    m_retry.setSingleShot(true);
    m_refreshTimeout.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, [this] { request("snapshot"); });
    connect(&m_retry, &QTimer::timeout, this, &DeviceModel::connectPipe);
    connect(&m_timeout, &QTimer::timeout, this, &DeviceModel::fail);
    connect(&m_socket, &QLocalSocket::readyRead, this, &DeviceModel::receive);
    connect(&m_socket, &QLocalSocket::disconnected, this, &DeviceModel::fail);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, &DeviceModel::fail);
    connect(&m_refreshTimeout, &QTimer::timeout, this, [this] {
        m_refreshTarget = 0;
        m_error = tr("The background scan is taking longer than expected.");
        emit stateChanged();
    });
    m_poll.start();
    QTimer::singleShot(0, this, [this] { request("snapshot"); });
}

int DeviceModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant DeviceModel::data(const QModelIndex& index, int role) const {
    return index.isValid() && role == DeviceRole ? QVariant(deviceAt(index.row())) : QVariant();
}

QHash<int, QByteArray> DeviceModel::roleNames() const { return {{DeviceRole, "device"}}; }

QVariantMap DeviceModel::deviceAt(int row) const { return m_rows.value(row); }

QVariantMap DeviceModel::deviceByKey(const QString& key) const {
    for (const auto& device : m_devices)
        if (device.value(QStringLiteral("key")).toString() == key) return device;
    return {};
}

int DeviceModel::indexOf(const QString& key) const {
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].value(QStringLiteral("key")).toString() == key) return i;
    return -1;
}

void DeviceModel::setShowHidden(bool show) {
    if (m_showHidden == show) return;
    m_showHidden = show;
    updateRows();
    emit showHiddenChanged();
}

void DeviceModel::refresh() { request("refresh"); }
void DeviceModel::rename(const QString& key, const QString& name) { request("name", key, name.trimmed().toUtf8()); }
void DeviceModel::forget(const QString& key) { request("forget", key); }
void DeviceModel::hide(const QString& key, bool hidden) { request("hidden", key, hidden ? "1" : "0"); }
void DeviceModel::setSurface(const QString& key, bool shown) { request("surface", key, shown ? "1" : "0"); }
void DeviceModel::setThreshold(int percent) { request("threshold", {}, QByteArray::number(percent)); }
void DeviceModel::setLowBatteryNotifications(bool enabled) {
    request("lowBatteryNotifications", {}, enabled ? "1" : "0");
}
void DeviceModel::setWidget(bool enabled) { request("widget", {}, enabled ? "1" : "0"); }

void DeviceModel::exitApplication() {
    panel::RequestClose(panel::FindBackgroundWindow());
    QCoreApplication::quit();
}

void DeviceModel::move(const QString& key, int direction) {
    QByteArray value = direction < 0 ? "up" : "down";
    if (m_showHidden) value += "All";
    request("move", key, value);
}

void DeviceModel::moveTo(const QString& key, const QString& targetKey) {
    request(m_showHidden ? "moveToAll" : "moveTo", key, targetKey.toUtf8().toHex());
}

void DeviceModel::request(const QByteArray& command, const QString& key, const QByteArray& value) {
    if (m_inFlight) {
        if (command == "snapshot" || m_command != "snapshot") return;
        // A user action can supersede a passive snapshot. Once sent, mutations are never replayed.
        finish();
    }
    m_command = command;
    m_request = command;
    if (command != "snapshot" && command != "refresh")
        m_request += '\t' + key.toUtf8().toHex() + '\t' + value;
    m_request += '\n';
    m_inFlight = true;
    if (command != "snapshot") {
        m_error.clear();
        emit stateChanged();
    }
    m_timeout.start(3000);
    connectPipe();
}

void DeviceModel::connectPipe() {
    // QLocalSocket::connectToServer waits synchronously when the Windows pipe is busy.
    // Open the overlapped handle once and let Qt handle its asynchronous reads and writes.
    const std::wstring path = panel::PipePath();
    HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_PIPE_BUSY) m_retry.start(80);
        else fail();
        return;
    }
    if (!m_socket.setSocketDescriptor(reinterpret_cast<qintptr>(pipe))) {
        CloseHandle(pipe);
        fail();
        return;
    }
    if (m_socket.write(m_request) != m_request.size()) fail();
}

void DeviceModel::receive() {
    if (!m_inFlight) return;
    if (!m_socket.canReadLine()) {
        if (m_socket.bytesAvailable() >= qint64(panel::MaxResponse)) fail();
        return;
    }
    const auto reply = QJsonDocument::fromJson(m_socket.readLine()).object();
    if (!reply.value(QStringLiteral("devices")).isArray() || !reply.value(QStringLiteral("ok")).isBool()) {
        fail();
        return;
    }
    const QByteArray command = m_command;
    finish();
    m_connected = true;
    m_snapshot = reply.toVariantMap();
    const auto list = m_snapshot.take(QStringLiteral("devices")).toList();
    m_devices.clear();
    m_devices.reserve(list.size());
    m_hiddenCount = 0;
    for (const auto& item : list) {
        auto device = item.toMap();
        if (device.value(QStringLiteral("hidden")).toBool()) ++m_hiddenCount;
        m_devices.append(std::move(device));
    }
    if (!reply.value(QStringLiteral("ok")).toBool()) {
        m_error = tr("The background could not apply the change.");
    } else if (command == "refresh") {
        m_refreshTarget = m_snapshot.value(QStringLiteral("refreshRequested")).toLongLong();
        m_refreshTimeout.start(30000);
    }
    if (m_refreshTarget && m_snapshot.value(QStringLiteral("refreshCompleted")).toLongLong() >= m_refreshTarget) {
        m_refreshTarget = 0;
        m_refreshTimeout.stop();
    }
    updateRows();
    emit stateChanged();
}

void DeviceModel::finish() {
    m_inFlight = false;
    m_timeout.stop();
    m_retry.stop();
    m_socket.abort();
}

void DeviceModel::fail() {
    if (!m_inFlight) return;
    if (m_command != "snapshot") m_error = tr("The connection was interrupted. Check the current setting and try again.");
    finish();
    m_connected = false;
    m_refreshTarget = 0;
    m_refreshTimeout.stop();
    emit stateChanged();
}

void DeviceModel::updateRows() {
    QList<QVariantMap> rows;
    for (const auto& device : m_devices) {
        if (m_showHidden || !device.value(QStringLiteral("hidden")).toBool()) rows.append(device);
    }
    const auto sameKeys = [](const QVariantMap& a, const QVariantMap& b) {
        return a.value(QStringLiteral("key")) == b.value(QStringLiteral("key"));
    };
    if (rows.size() != m_rows.size() || !std::equal(rows.begin(), rows.end(), m_rows.begin(), sameKeys)) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    } else {
        for (int i = 0; i < rows.size(); ++i) {
            if (m_rows[i] == rows[i]) continue;
            m_rows[i] = std::move(rows[i]);
            emit dataChanged(index(i), index(i), {DeviceRole});
        }
    }
    emit rowsChanged();
}
