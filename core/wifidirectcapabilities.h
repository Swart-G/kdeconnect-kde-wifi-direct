// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#pragma once

#include "kdeconnectcore_export.h"
#include <QMap>
#include <QSet>
#include <QString>
#include <QList>

namespace WifiDirectCapabilities
{
struct Limit {
    QSet<int> types;
    int maximum = 0;
};
struct Combination {
    QList<Limit> limits;
    int maximum = 0;
    int channels = 0;
};
struct Radio {
    QSet<int> modes;
    QSet<int> softwareModes;
    QList<Combination> combinations;
    QMap<int, int> running;
    bool stationConnected = false;
    int occupiedChannels = 0;
};
struct Result {
    bool available = false;
    bool stationConnected = false;
    QString reason;
    QStringList stationInterfaces;
};
KDECONNECTCORE_EXPORT Result evaluate(const Radio &radio);
KDECONNECTCORE_EXPORT Result probe(const QString &interfaceName);
}
