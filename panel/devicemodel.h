#pragma once

#include <QAbstractListModel>
#include <QLocalSocket>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration>

class DeviceModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The application supplies the device model")

    Q_PROPERTY(int count READ count NOTIFY rowsChanged FINAL)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY rowsChanged FINAL)
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged FINAL)

public:
    explicit DeviceModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return rowCount(); }
    int hiddenCount() const { return m_hiddenCount; }
    QVariantMap snapshot() const { return m_snapshot; }
    bool connected() const { return m_connected; }
    bool busy() const { return m_inFlight && m_command != "snapshot"; }
    bool refreshing() const { return m_refreshTarget != 0 || (m_inFlight && m_command == "refresh"); }
    QString error() const { return m_error; }
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);

    Q_INVOKABLE QVariantMap deviceAt(int row) const;
    Q_INVOKABLE QVariantMap deviceByKey(const QString& key) const;
    Q_INVOKABLE int indexOf(const QString& key) const;
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void rename(const QString& key, const QString& name);
    Q_INVOKABLE void forget(const QString& key);
    Q_INVOKABLE void hide(const QString& key, bool hidden);
    Q_INVOKABLE void setSurface(const QString& key, bool shown);
    Q_INVOKABLE void move(const QString& key, int direction);
    Q_INVOKABLE void moveTo(const QString& key, const QString& targetKey);
    Q_INVOKABLE void setThreshold(int percent);
    Q_INVOKABLE void setLowBatteryNotifications(bool enabled);
    Q_INVOKABLE void setWidget(bool enabled);
    Q_INVOKABLE void exitApplication();

signals:
    void rowsChanged();
    void stateChanged();
    void showHiddenChanged();

private:
    void request(const QByteArray& command, const QString& key = {}, const QByteArray& value = {});
    void connectPipe();
    void receive();
    void finish();
    void fail();
    void updateRows();

    QLocalSocket m_socket;
    QTimer m_poll, m_retry, m_timeout, m_refreshTimeout;
    QByteArray m_command, m_request;
    QVariantMap m_snapshot;
    QList<QVariantMap> m_devices, m_rows;
    QString m_error;
    qint64 m_refreshTarget = 0;
    int m_hiddenCount = 0;
    bool m_connected = false;
    bool m_inFlight = false;
    bool m_showHidden = false;
};
