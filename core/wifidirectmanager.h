// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#pragma once

#include "kdeconnectcore_export.h"

#include <QDBusConnection>
#include <QMap>
#include <QObject>
#include <QTimer>

class KDECONNECTCORE_EXPORT WifiDirectManager : public QObject
{
    Q_OBJECT

public:
    explicit WifiDirectManager(QObject *parent, const QDBusConnection &bus = QDBusConnection::systemBus());
    QString status() const { return m_status; }
    QStringList networks() const { return m_networks.keys(); }
    bool searching() const { return m_discoveryTimer.isActive(); }
    bool connecting() const { return m_connecting; }
    void startDiscovery();
    void stopDiscovery();
    void connectNetwork(const QString &ssid, const QString &password);
    void disconnectNetwork();

Q_SIGNALS:
    void changed();
    void networkActivated();

private Q_SLOTS:
    void activePropertiesChanged(const QString &interface, const QVariantMap &properties, const QStringList &invalidated);

private:
    struct AccessPoint {
        QString device;
        QString path;
    };
    void refreshNetworks();
    void pollConnection();
    void setStatus(const QString &status);
    void finishActivation(const QString &activePath, unsigned generation);
    QString savedConnection(const QString &ssid) const;

    QDBusConnection m_bus;
    QTimer m_discoveryTimer;
    QTimer m_scanTimer;
    QTimer m_connectionTimer;
    QMap<QString, AccessPoint> m_networks;
    QString m_status;
    QString m_activeConnection;
    bool m_connecting = false;
    unsigned m_generation = 0;
    int m_scanTicks = 0;
    int m_connectionTicks = 0;
};
