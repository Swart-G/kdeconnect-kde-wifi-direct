// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include "wifidirectplugin.h"

#include <core/daemon.h>
#include <KLocalizedString>
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(WifiDirectPlugin, "kdeconnect_wifidirect.json")

WifiDirectPlugin::WifiDirectPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    connect(Daemon::instance(), &Daemon::wifiDirectChanged, this, &WifiDirectPlugin::statusChanged);
}

QString WifiDirectPlugin::dbusPath() const
{
    return QStringLiteral("/modules/kdeconnect/devices/%1/wifidirect").arg(device()->id());
}

QString WifiDirectPlugin::status() const
{
    return Daemon::instance()->wifiDirectStatus();
}

void WifiDirectPlugin::retry()
{
    Daemon::instance()->startWifiDirectDiscovery();
}

void WifiDirectPlugin::disconnectNetwork()
{
    Daemon::instance()->disconnectWifiDirectNetwork();
}

void WifiDirectPlugin::receivePacket(const NetworkPacket &)
{
}

#include "moc_wifidirectplugin.cpp"
#include "wifidirectplugin.moc"
