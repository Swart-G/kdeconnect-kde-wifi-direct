// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include <core/wifidirectmanager.h>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    WifiDirectManager manager(nullptr);
    if (!manager.available()) manager.startDiscovery();
    QJsonObject result{{QStringLiteral("available"), manager.available()}, {QStringLiteral("state"), manager.state()},
        {QStringLiteral("reason"), manager.unavailableReason()}, {QStringLiteral("status"), manager.status()}};
    if (app.arguments().size() > 1) {
        const auto radio = WifiDirectCapabilities::probe(app.arguments().at(1));
        result.insert(QStringLiteral("driverAvailable"), radio.available);
        result.insert(QStringLiteral("driverReason"), radio.reason);
        result.insert(QStringLiteral("stationConnected"), radio.stationConnected);
    }
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return manager.available() ? 2 : 0;
}
