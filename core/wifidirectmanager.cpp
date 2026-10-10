// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include "wifidirectmanager.h"

#include <KLocalizedString>
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QRegularExpression>
#include <QUuid>

using ConnectionSettings = QMap<QString, QVariantMap>;
Q_DECLARE_METATYPE(ConnectionSettings)

static QDBusArgument &operator<<(QDBusArgument &argument, const ConnectionSettings &settings)
{
    argument.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QVariantMap>());
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        argument.beginMapEntry();
        argument << it.key() << it.value();
        argument.endMapEntry();
    }
    argument.endMap();
    return argument;
}

static const QDBusArgument &operator>>(const QDBusArgument &argument, ConnectionSettings &settings)
{
    argument.beginMap();
    while (!argument.atEnd()) {
        QString key;
        QVariantMap value;
        argument.beginMapEntry();
        argument >> key >> value;
        argument.endMapEntry();
        settings.insert(key, value);
    }
    argument.endMap();
    return argument;
}

static const QString nmService = QStringLiteral("org.freedesktop.NetworkManager");
static const QString nmPath = QStringLiteral("/org/freedesktop/NetworkManager");

WifiDirectManager::WifiDirectManager(QObject *parent, const QDBusConnection &bus)
    : QObject(parent)
    , m_bus(bus)
{
    qDBusRegisterMetaType<ConnectionSettings>();
    m_discoveryTimer.setSingleShot(true);
    m_discoveryTimer.setInterval(60000);
    connect(&m_discoveryTimer, &QTimer::timeout, this, [this] {
        m_scanTimer.stop();
        setStatus(m_networks.isEmpty() ? i18n("Search finished. Start the direct network on your phone and try again.")
                                      : i18n("Search finished. Select a phone's network to connect."));
        Q_EMIT changed();
    });
    m_scanTimer.setInterval(2000);
    connect(&m_scanTimer, &QTimer::timeout, this, &WifiDirectManager::refreshNetworks);
    m_connectionTimer.setInterval(1000);
    connect(&m_connectionTimer, &QTimer::timeout, this, &WifiDirectManager::pollConnection);
    setStatus(i18n("Start the direct network on your phone, then search for nearby Wi-Fi networks."));
    QDBusInterface nm(nmService, nmPath, nmService, m_bus);
    if (nm.isValid()) {
        const auto activeConnections = qdbus_cast<QList<QDBusObjectPath>>(nm.property("ActiveConnections"));
        for (const auto &path : activeConnections) {
            QDBusInterface active(nmService, path.path(), nmService + QStringLiteral(".Connection.Active"), m_bus);
            if (active.property("State").toUInt() == 2
                && active.property("Id").toString().startsWith(QLatin1String("KDE Connect Direct DIRECT-KC-KDEConnect"))) {
                finishActivation(path.path(), m_generation);
                setStatus(i18n("Direct Wi-Fi is connected. Select the phone and confirm KDE Connect pairing."));
                break;
            }
        }
    }
}

void WifiDirectManager::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    Q_EMIT changed();
}

void WifiDirectManager::startDiscovery()
{
    if (m_connecting) return;
    m_networks.clear();
    m_scanTicks = 0;
    m_discoveryTimer.start();
    m_scanTimer.start();
    setStatus(i18n("Searching for nearby Wi-Fi networks for 60 seconds..."));
    Q_EMIT changed();
    refreshNetworks();
}

void WifiDirectManager::stopDiscovery()
{
    m_discoveryTimer.stop();
    m_scanTimer.stop();
    setStatus(i18n("Wi-Fi search stopped"));
    Q_EMIT changed();
}

void WifiDirectManager::refreshNetworks()
{
    if (!searching()) return;
    QDBusInterface nm(nmService, nmPath, nmService, m_bus);
    nm.setTimeout(1000);
    if (!nm.isValid() || !nm.property("WirelessEnabled").toBool()) {
        stopDiscovery();
        setStatus(i18n("Enable Wi-Fi and NetworkManager on your computer."));
        return;
    }
    const QDBusReply<QList<QDBusObjectPath>> devices = nm.call(QStringLiteral("GetDevices"));
    if (!devices.isValid()) {
        stopDiscovery();
        setStatus(i18n("Cannot list Wi-Fi adapters: %1", devices.error().message()));
        return;
    }
    QMap<QString, AccessPoint> networks;
    bool hasWifi = false;
    for (const auto &path : devices.value()) {
        QDBusInterface adapter(nmService, path.path(), nmService + QStringLiteral(".Device"), m_bus);
        if (adapter.property("DeviceType").toUInt() != 2 || !adapter.property("Managed").toBool()) continue;
        hasWifi = true;
        QDBusInterface wifi(nmService, path.path(), nmService + QStringLiteral(".Device.Wireless"), m_bus);
        wifi.setTimeout(1000);
        if (m_scanTicks % 5 == 0) {
            auto *watcher = new QDBusPendingCallWatcher(wifi.asyncCall(QStringLiteral("RequestScan"), QVariantMap{}), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
        }
        const QDBusReply<QList<QDBusObjectPath>> points = wifi.call(QStringLiteral("GetAllAccessPoints"));
        if (!points.isValid()) continue;
        for (const auto &point : points.value()) {
            QDBusInterface ap(nmService, point.path(), nmService + QStringLiteral(".AccessPoint"), m_bus);
            const QByteArray rawSsid = ap.property("Ssid").toByteArray();
            const QString ssid = QString::fromUtf8(rawSsid);
            if (ssid.startsWith(QLatin1String("DIRECT-KC-KDEConnect")) && rawSsid.size() <= 32 && ssid.toUtf8() == rawSsid) {
                networks.insert(ssid, {path.path(), point.path()});
            }
        }
    }
    ++m_scanTicks;
    if (!hasWifi) {
        stopDiscovery();
        setStatus(i18n("No available Wi-Fi adapter. Enable Wi-Fi on your computer."));
        return;
    }
    const bool networksChanged = m_networks.keys() != networks.keys();
    m_networks = networks;
    if (networksChanged) Q_EMIT changed();
}

QString WifiDirectManager::savedConnection(const QString &ssid) const
{
    QDBusInterface settings(nmService, nmPath + QStringLiteral("/Settings"), nmService + QStringLiteral(".Settings"), m_bus);
    settings.setTimeout(1000);
    const QDBusReply<QList<QDBusObjectPath>> connections = settings.call(QStringLiteral("ListConnections"));
    if (!connections.isValid()) return {};
    for (const auto &path : connections.value()) {
        QDBusInterface connection(nmService, path.path(), nmService + QStringLiteral(".Settings.Connection"), m_bus);
        connection.setTimeout(1000);
        const QDBusReply<ConnectionSettings> info = connection.call(QStringLiteral("GetSettings"));
        if (info.isValid() && info.value().value(QStringLiteral("connection")).value(QStringLiteral("id")).toString()
            == QStringLiteral("KDE Connect Direct %1").arg(ssid)
            && info.value().value(QStringLiteral("802-11-wireless")).value(QStringLiteral("ssid")).toByteArray() == ssid.toUtf8()) {
            return path.path();
        }
    }
    return {};
}

void WifiDirectManager::connectNetwork(const QString &ssid, const QString &password)
{
    if (m_connecting) return;
    if (!m_networks.contains(ssid)) {
        setStatus(i18n("Search for the phone's Wi-Fi network before connecting."));
        return;
    }
    const QString saved = savedConnection(ssid);
    if (password.isEmpty() && saved.isEmpty()) {
        setStatus(i18n("Enter the Wi-Fi password shown on your phone."));
        return;
    }
    if (!password.isEmpty() && !QRegularExpression(QStringLiteral("^[\\x20-\\x7e]{8,63}$")).match(password).hasMatch()) {
        setStatus(i18n("The Wi-Fi password must contain 8 to 63 ASCII characters."));
        return;
    }
    const auto point = m_networks.value(ssid);
    if (!m_activeConnection.isEmpty()) {
        m_bus.disconnect(nmService, m_activeConnection, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                         this, SLOT(activePropertiesChanged(QString,QVariantMap,QStringList)));
        m_activeConnection.clear();
    }
    m_discoveryTimer.stop();
    m_scanTimer.stop();
    m_connecting = true;
    m_connectionTicks = 0;
    m_connectionTimer.start();
    const unsigned generation = ++m_generation;
    setStatus(i18n("Connecting to %1...", ssid));
    Q_EMIT changed();
    QDBusInterface nm(nmService, nmPath, nmService, m_bus);
    if (!saved.isEmpty()) {
        if (!password.isEmpty()) {
            QDBusInterface connection(nmService, saved, nmService + QStringLiteral(".Settings.Connection"), m_bus);
            connection.setTimeout(1000);
            const QDBusReply<ConnectionSettings> info = connection.call(QStringLiteral("GetSettings"));
            if (!info.isValid()) {
                finishActivation({}, generation);
                setStatus(i18n("Cannot read the saved Wi-Fi profile."));
                return;
            }
            auto settings = info.value();
            settings[QStringLiteral("802-11-wireless-security")][QStringLiteral("psk")] = password;
            const QDBusReply<void> update = connection.call(QStringLiteral("Update"), QVariant::fromValue(settings));
            if (!update.isValid()) {
                finishActivation({}, generation);
                setStatus(i18n("Cannot update the Wi-Fi password: %1", update.error().message()));
                return;
            }
        }
        auto *watcher = new QDBusPendingCallWatcher(nm.asyncCall(QStringLiteral("ActivateConnection"),
            QVariant::fromValue(QDBusObjectPath(saved)), QVariant::fromValue(QDBusObjectPath(point.device)),
            QVariant::fromValue(QDBusObjectPath(point.path))), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
            const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            finishActivation(reply.isError() ? QString() : reply.value().path(), generation);
            if (reply.isError() && generation == m_generation) setStatus(i18n("Wi-Fi connection failed: %1", reply.error().message()));
        });
        return;
    }
    ConnectionSettings settings;
    settings[QStringLiteral("connection")] = {
        {QStringLiteral("id"), QStringLiteral("KDE Connect Direct %1").arg(ssid)},
        {QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("type"), QStringLiteral("802-11-wireless")},
        {QStringLiteral("autoconnect"), false}};
    settings[QStringLiteral("802-11-wireless")] = {
        {QStringLiteral("ssid"), ssid.toUtf8()}, {QStringLiteral("mode"), QStringLiteral("infrastructure")}};
    settings[QStringLiteral("802-11-wireless-security")] = {
        {QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk")}, {QStringLiteral("psk"), password}};
    settings[QStringLiteral("ipv4")] = {
        {QStringLiteral("method"), QStringLiteral("auto")}, {QStringLiteral("never-default"), true},
        {QStringLiteral("ignore-auto-dns"), true}};
    settings[QStringLiteral("ipv6")] = {{QStringLiteral("method"), QStringLiteral("disabled")}};
    auto *watcher = new QDBusPendingCallWatcher(nm.asyncCall(QStringLiteral("AddAndActivateConnection2"),
        QVariant::fromValue(settings), QVariant::fromValue(QDBusObjectPath(point.device)),
        QVariant::fromValue(QDBusObjectPath(point.path)), QVariantMap{{QStringLiteral("persist"), QStringLiteral("disk")}}), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        const QDBusPendingReply<QDBusObjectPath, QDBusObjectPath, QVariantMap> reply = *watcher;
        watcher->deleteLater();
        finishActivation(reply.isError() ? QString() : reply.argumentAt<1>().path(), generation);
        if (reply.isError() && generation == m_generation) setStatus(i18n("Wi-Fi connection failed: %1", reply.error().message()));
    });
}

void WifiDirectManager::finishActivation(const QString &activePath, unsigned generation)
{
    if (generation != m_generation) {
        if (!activePath.isEmpty()) {
            QDBusInterface nm(nmService, nmPath, nmService, m_bus);
            nm.asyncCall(QStringLiteral("DeactivateConnection"), QVariant::fromValue(QDBusObjectPath(activePath)));
        }
        return;
    }
    if (activePath.isEmpty()) {
        m_connecting = false;
        m_connectionTimer.stop();
        Q_EMIT changed();
        return;
    }
    m_activeConnection = activePath;
    m_bus.connect(nmService, activePath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                  this, SLOT(activePropertiesChanged(QString,QVariantMap,QStringList)));
}

void WifiDirectManager::activePropertiesChanged(const QString &interface, const QVariantMap &properties, const QStringList &)
{
    if (interface != nmService + QStringLiteral(".Connection.Active")) return;
    if (properties.value(QStringLiteral("State")).toUInt() == 4) {
        disconnectNetwork();
        setStatus(i18n("Direct Wi-Fi disconnected. Start a new search to reconnect."));
    }
}

void WifiDirectManager::pollConnection()
{
    if (!m_activeConnection.isEmpty()) {
        QDBusInterface active(nmService, m_activeConnection, nmService + QStringLiteral(".Connection.Active"), m_bus);
        const uint state = active.property("State").toUInt();
        if (state == 2) {
            m_connecting = false;
            m_connectionTimer.stop();
            setStatus(i18n("Direct Wi-Fi is connected. Select the phone and confirm KDE Connect pairing."));
            Q_EMIT changed();
            Q_EMIT networkActivated();
            return;
        }
        if (state == 4 || !active.isValid()) {
            disconnectNetwork();
            setStatus(i18n("Wi-Fi connection failed. Check the password and try again."));
            return;
        }
    }
    if (++m_connectionTicks >= 45) {
        disconnectNetwork();
        setStatus(i18n("Wi-Fi connection timed out. Check the phone and try again."));
    }
}

void WifiDirectManager::disconnectNetwork()
{
    ++m_generation;
    m_discoveryTimer.stop();
    m_scanTimer.stop();
    m_connectionTimer.stop();
    m_connecting = false;
    if (!m_activeConnection.isEmpty()) {
        m_bus.disconnect(nmService, m_activeConnection, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                         this, SLOT(activePropertiesChanged(QString,QVariantMap,QStringList)));
        QDBusInterface nm(nmService, nmPath, nmService, m_bus);
        nm.asyncCall(QStringLiteral("DeactivateConnection"), QVariant::fromValue(QDBusObjectPath(m_activeConnection)));
        m_activeConnection.clear();
    }
    setStatus(i18n("Direct Wi-Fi disconnected"));
    Q_EMIT changed();
}

#include "moc_wifidirectmanager.cpp"
