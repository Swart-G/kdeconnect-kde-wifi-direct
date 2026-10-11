// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#pragma once

#include "kdeconnectcore_export.h"
#include "wifidirectcapabilities.h"
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QMap>
#include <QNetworkInterface>
#include <QObject>
#include <QTimer>
#include <functional>

struct WifiDirectTimeouts {
    int search = 60000;
    int negotiation = 90000;
    int tls = 30000;
    int poll = 1000;
};

class KDECONNECTCORE_EXPORT WifiDirectManager : public QObject
{
    Q_OBJECT
public:
    using CapabilityProbe = std::function<WifiDirectCapabilities::Result(const QString &)>;
    explicit WifiDirectManager(QObject *parent, const QDBusConnection &bus = QDBusConnection::systemBus(),
                               CapabilityProbe probe = WifiDirectCapabilities::probe, WifiDirectTimeouts timeouts = {});
    ~WifiDirectManager() override;
    bool available() const { return m_available; }
    QString unavailableReason() const { return m_unavailableReason; }
    QString state() const { return m_state; }
    QString status() const { return m_status; }
    QVariantList peers() const;
    QString connectedDeviceId() const { return m_connectedDeviceId; }
    void startDiscovery();
    void stopDiscovery();
    void connectPeer(const QString &id);
    void disconnectSession();
    void authenticatedDevice(const QString &id, bool paired);
    void devicePaired(const QString &id, bool paired);

Q_SIGNALS:
    void changed();
    void interfaceReady(const QString &name);
    void sessionEnded();

private Q_SLOTS:
    void peerChanged(const QDBusObjectPath &path);
    void deviceRemoved(const QDBusObjectPath &path);
    void poll();
    void deviceAdded(const QDBusObjectPath &path);
    void managerPropertiesChanged(const QString &interface, const QVariantMap &properties, const QStringList &invalidated);

private:
    struct Peer { QString id; QString name; QString address; bool available = false; };
    bool selectAdapter();
    void refreshPeers();
    void stopFind();
    void finishActivation(const QString &path, unsigned generation, const QString &owner);
    void endSession(const QString &message);
    void setState(const QString &state, const QString &status);
    bool originalWifiIntact() const;
    bool subnetConflict(const QNetworkInterface &iface) const;

    friend class WifiDirectTest;
    WifiDirectTimeouts m_timeouts;
    QDBusConnection m_bus;
    CapabilityProbe m_probe;
    QTimer m_discoveryTimer;
    QTimer m_monitor;
    QElapsedTimer m_attempt;
    QElapsedTimer m_tlsWait;
    QMap<QString, Peer> m_peers;
    QMap<QString, QString> m_originalWifi;
    QString m_device;
    QString m_interface;
    QString m_activeConnection;
    QString m_owner;
    QString m_connectedDeviceId;
    QString m_state = QStringLiteral("unavailable");
    QString m_status;
    QString m_unavailableReason;
    bool m_available = false;
    bool m_findRequested = false;
    bool m_session = false;
    bool m_interfaceReady = false;
    unsigned m_generation = 0;
};
