// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#pragma once

#include <core/kdeconnectplugin.h>

class WifiDirectPlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.wifidirect")
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    WifiDirectPlugin(QObject *parent, const QVariantList &args);
    void receivePacket(const NetworkPacket &packet) override;
    QString dbusPath() const override;
    QString status() const;
    Q_SCRIPTABLE void retry();
    Q_SCRIPTABLE void disconnectNetwork();

Q_SIGNALS:
    Q_SCRIPTABLE void statusChanged();
};
