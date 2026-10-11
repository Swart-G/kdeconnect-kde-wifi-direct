// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include "wifidirectmanager.h"
#include <KLocalizedString>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>

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
static const QString p2pInterface = nmService + QStringLiteral(".Device.WifiP2P");
static QVariant nmProperty(const QDBusConnection &bus, const QString &path, const QString &interface, const char *name)
{
    QDBusInterface object(nmService, path, interface, bus);
    object.setTimeout(1000);
    return object.property(name);
}

WifiDirectManager::WifiDirectManager(QObject *parent, const QDBusConnection &bus, CapabilityProbe probe, WifiDirectTimeouts timeouts)
    : QObject(parent), m_timeouts(timeouts), m_bus(bus), m_probe(std::move(probe))
{
    qDBusRegisterMetaType<ConnectionSettings>();
    m_discoveryTimer.setSingleShot(true);
    m_discoveryTimer.setInterval(m_timeouts.search);
    connect(&m_discoveryTimer, &QTimer::timeout, this, &WifiDirectManager::stopDiscovery);
    m_monitor.setInterval(m_timeouts.poll);
    connect(&m_monitor, &QTimer::timeout, this, &WifiDirectManager::poll);
    auto *watcher = new QDBusServiceWatcher(nmService, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
        // Object paths from the old service must never be used with a new owner.
        m_activeConnection.clear();
        m_findRequested = false;
        endSession(i18n("NetworkManager restarted. Start a new search to reconnect."));
        selectAdapter();
    });
    m_bus.connect(nmService, nmPath, nmService, QStringLiteral("DeviceRemoved"), this, SLOT(deviceRemoved(QDBusObjectPath)));
    m_bus.connect(nmService, nmPath, nmService, QStringLiteral("DeviceAdded"), this, SLOT(deviceAdded(QDBusObjectPath)));
    m_bus.connect(nmService, nmPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                  this, SLOT(managerPropertiesChanged(QString,QVariantMap,QStringList)));
    selectAdapter();
}
WifiDirectManager::~WifiDirectManager()
{
    stopFind();
    if (!m_activeConnection.isEmpty()) {
        QDBusInterface nm(m_owner, nmPath, nmService, m_bus);
        nm.asyncCall(QStringLiteral("DeactivateConnection"), QVariant::fromValue(QDBusObjectPath(m_activeConnection)));
    }
}
void WifiDirectManager::setState(const QString &state, const QString &status)
{
    m_state = state;
    m_status = status;
    Q_EMIT changed();
}
QVariantList WifiDirectManager::peers() const
{
    QVariantList result;
    for (const auto &peer : m_peers) {
        result.append(QVariantMap{{QStringLiteral("id"), peer.id}, {QStringLiteral("name"), peer.name},
                                  {QStringLiteral("available"), peer.available}});
    }
    return result;
}
bool WifiDirectManager::selectAdapter()
{
    if (!m_device.isEmpty()) {
        m_bus.disconnect(nmService, m_device, p2pInterface, QStringLiteral("PeerAdded"), this, SLOT(peerChanged(QDBusObjectPath)));
        m_bus.disconnect(nmService, m_device, p2pInterface, QStringLiteral("PeerRemoved"), this, SLOT(peerChanged(QDBusObjectPath)));
    }
    m_device.clear();
    m_available = false;
    m_unavailableReason = i18n("No compatible Wi-Fi Direct adapter is available.");
    QDBusInterface nm(nmService, nmPath, nmService, m_bus);
    nm.setTimeout(1000);
    if (!nm.isValid() || !nm.property("WirelessEnabled").toBool()) {
        m_unavailableReason = i18n("Enable Wi-Fi and NetworkManager on your computer.");
    } else {
        const QDBusReply<QList<QDBusObjectPath>> devices = nm.call(QStringLiteral("GetDevices"));
        struct Candidate { QString path; QString name; bool station; };
        QList<Candidate> candidates;
        QStringList stations;
        bool sawP2p = false;
        if (!devices.isValid()) m_unavailableReason = i18n("Cannot list Wi-Fi adapters: %1", devices.error().message());
        for (const auto &path : devices.value()) {
            QDBusInterface dev(nmService, path.path(), nmService + QStringLiteral(".Device"), m_bus);
            dev.setTimeout(1000);
            const auto type = dev.property("DeviceType").toUInt();
            if (type == 2) stations.append(dev.property("Interface").toString());
            if (type != 30 || !dev.property("Managed").toBool()) continue;
            sawP2p = true;
            const auto capabilities = m_probe(dev.property("Interface").toString());
            if (!capabilities.available) {
                m_unavailableReason = capabilities.reason;
                continue;
            }
            const auto active = qdbus_cast<QDBusObjectPath>(dev.property("ActiveConnection")).path();
            if (!active.isEmpty() && active != QLatin1String("/")) {
                m_unavailableReason = i18n("Another Wi-Fi Direct group is active on this adapter.");
                continue;
            }
            candidates.append({path.path(), dev.property("Interface").toString(), capabilities.stationConnected});
        }
        if (!sawP2p && devices.isValid()) {
            m_unavailableReason = i18n("NetworkManager does not expose a Wi-Fi Direct device.");
            stations.sort();
            for (const auto &station : stations) {
                const auto capability = m_probe(station);
                if (!capability.available) { m_unavailableReason = capability.reason; break; }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            return a.station != b.station ? a.station > b.station : a.name < b.name;
        });
        if (!candidates.isEmpty()) {
            m_device = candidates.first().path;
            m_available = true;
            m_unavailableReason.clear();
            m_bus.connect(nmService, m_device, p2pInterface, QStringLiteral("PeerAdded"), this, SLOT(peerChanged(QDBusObjectPath)));
            m_bus.connect(nmService, m_device, p2pInterface, QStringLiteral("PeerRemoved"), this, SLOT(peerChanged(QDBusObjectPath)));
        }
    }
    setState(m_available ? QStringLiteral("ready") : QStringLiteral("unavailable"),
             m_available ? i18n("Make your phone available in Nearby connection, then find nearby devices.") : m_unavailableReason);
    return m_available;
}
void WifiDirectManager::startDiscovery()
{
    if (m_session || m_findRequested) return;
    if (!selectAdapter()) return;
    m_peers.clear();
    m_originalWifi.clear();
    QDBusInterface nm(nmService, nmPath, nmService, m_bus);
    const QDBusReply<QList<QDBusObjectPath>> devices = nm.call(QStringLiteral("GetDevices"));
    for (const auto &path : devices.value()) {
        if (nmProperty(m_bus, path.path(), nmService + QStringLiteral(".Device"), "DeviceType").toUInt() != 2) continue;
        const auto active = qdbus_cast<QDBusObjectPath>(nmProperty(m_bus, path.path(), nmService + QStringLiteral(".Device"), "ActiveConnection")).path();
        if (!active.isEmpty() && active != QLatin1String("/")) m_originalWifi.insert(path.path(), active);
    }
    m_owner = m_bus.interface()->serviceOwner(nmService).value();
    const unsigned generation = ++m_generation;
    const QString device = m_device;
    const QString owner = m_owner;
    m_findRequested = true;
    m_discoveryTimer.start();
    m_monitor.start();
    setState(QStringLiteral("searching"), i18n("Searching for nearby devices for 60 seconds..."));
    QDBusInterface p2p(owner, device, p2pInterface, m_bus);
    auto *watcher = new QDBusPendingCallWatcher(p2p.asyncCall(QStringLiteral("StartFind"), QVariantMap{{QStringLiteral("timeout"), 60}}), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation, device, owner] {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        if (generation != m_generation || !m_findRequested) {
            if (!reply.isError() && !(m_findRequested && m_device == device && m_owner == owner)) {
                QDBusInterface p2p(owner, device, p2pInterface, m_bus);
                p2p.asyncCall(QStringLiteral("StopFind"));
            }
            return;
        }
        if (reply.isError()) endSession(i18n("Nearby search failed: %1", reply.error().message()));
        else refreshPeers();
    });
    refreshPeers();
}
void WifiDirectManager::stopFind()
{
    m_discoveryTimer.stop();
    if (m_findRequested) {
        QDBusInterface p2p(m_owner, m_device, p2pInterface, m_bus);
        p2p.asyncCall(QStringLiteral("StopFind"));
        m_findRequested = false;
    }
}
void WifiDirectManager::stopDiscovery()
{
    if (!m_findRequested) return;
    stopFind();
    if (!m_session) {
        m_monitor.stop();
        setState(QStringLiteral("ready"), i18n("Search finished. Select an available device or start a new search."));
    }
}
void WifiDirectManager::refreshPeers()
{
    const auto paths = qdbus_cast<QList<QDBusObjectPath>>(nmProperty(m_bus, m_device, p2pInterface, "Peers"));
    for (auto &peer : m_peers) peer.available = false;
    for (const auto &path : paths) {
        QDBusInterface object(nmService, path.path(), nmService + QStringLiteral(".WifiP2PPeer"), m_bus);
        object.setTimeout(1000);
        if (!object.isValid()) continue;
        auto &peer = m_peers[path.path()];
        if (peer.id.isEmpty()) peer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        peer.name = object.property("Name").toString();
        if (peer.name.isEmpty()) peer.name = i18n("Unnamed device");
        peer.address = object.property("HwAddress").toString();
        peer.available = true;
    }
    Q_EMIT changed();
}
void WifiDirectManager::peerChanged(const QDBusObjectPath &)
{
    if (m_findRequested) refreshPeers();
}
void WifiDirectManager::deviceRemoved(const QDBusObjectPath &path)
{
    if (path.path() == m_device || m_originalWifi.contains(path.path())) {
        endSession(i18n("Wi-Fi adapter removed. Start a new search to reconnect."));
        selectAdapter();
    }
}
void WifiDirectManager::deviceAdded(const QDBusObjectPath &)
{
    if (!m_session && !m_findRequested) selectAdapter();
}
void WifiDirectManager::managerPropertiesChanged(const QString &interface, const QVariantMap &properties, const QStringList &invalidated)
{
    if (interface == nmService && (properties.contains(QStringLiteral("WirelessEnabled")) || invalidated.contains(QStringLiteral("WirelessEnabled")))) {
        if (m_session || m_findRequested) poll();
        else selectAdapter();
    }
}
bool WifiDirectManager::originalWifiIntact() const
{
    for (auto it = m_originalWifi.cbegin(); it != m_originalWifi.cend(); ++it) {
        if (qdbus_cast<QDBusObjectPath>(nmProperty(m_bus, it.key(), nmService + QStringLiteral(".Device"), "ActiveConnection")).path() != it.value()
            || nmProperty(m_bus, it.key(), nmService + QStringLiteral(".Device"), "State").toUInt() != 100) return false;
    }
    return true;
}
void WifiDirectManager::connectPeer(const QString &id)
{
    if (m_session || !m_available || m_device.isEmpty()) return;
    refreshPeers();
    auto it = std::find_if(m_peers.cbegin(), m_peers.cend(), [&id](const Peer &peer) { return peer.id == id; });
    if (it == m_peers.cend() || !it->available) {
        setState(m_state, i18n("This device is no longer available. Start a new search."));
        return;
    }
    const auto capability = m_probe(nmProperty(m_bus, m_device, nmService + QStringLiteral(".Device"), "Interface").toString());
    if (!capability.available || !originalWifiIntact()) {
        endSession(capability.available ? i18n("Ordinary Wi-Fi was lost. The adapter cannot preserve this connection.") : capability.reason);
        return;
    }
    const auto active = qdbus_cast<QDBusObjectPath>(nmProperty(m_bus, m_device, nmService + QStringLiteral(".Device"), "ActiveConnection")).path();
    if (!active.isEmpty() && active != QLatin1String("/")) {
        endSession(i18n("Another Wi-Fi Direct group is active on this adapter."));
        return;
    }
    const QString name = it->name;
    const QString address = it->address;
    if (!QRegularExpression(QStringLiteral("^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$")).match(address).hasMatch()) {
        endSession(i18n("Cannot read the selected device's Wi-Fi Direct address."));
        return;
    }
    const QString peerPath = it.key();
    stopFind();
    m_session = true;
    m_interfaceReady = false;
    m_attempt.start();
    m_monitor.start();
    const unsigned generation = ++m_generation;
    const QString owner = m_owner;
    setState(QStringLiteral("connecting"), i18n("Connecting to %1. Accept the system Wi-Fi Direct request on your phone.", name));
    ConnectionSettings settings;
    settings[QStringLiteral("connection")] = {
        {QStringLiteral("id"), QStringLiteral("KDE Connect P2P %1").arg(name)},
        {QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("type"), QStringLiteral("wifi-p2p")}, {QStringLiteral("autoconnect"), false}};
    settings[QStringLiteral("wifi-p2p")] = {{QStringLiteral("peer"), address}, {QStringLiteral("wps-method"), uint(4)}};
    settings[QStringLiteral("ipv4")] = {{QStringLiteral("method"), QStringLiteral("auto")},
        {QStringLiteral("never-default"), true}, {QStringLiteral("ignore-auto-dns"), true}, {QStringLiteral("ignore-auto-routes"), true}};
    settings[QStringLiteral("ipv6")] = {{QStringLiteral("method"), QStringLiteral("disabled")}};
    QDBusInterface nm(owner, nmPath, nmService, m_bus);
    auto *watcher = new QDBusPendingCallWatcher(nm.asyncCall(QStringLiteral("AddAndActivateConnection2"), QVariant::fromValue(settings),
        QVariant::fromValue(QDBusObjectPath(m_device)), QVariant::fromValue(QDBusObjectPath(peerPath)),
        QVariantMap{{QStringLiteral("persist"), QStringLiteral("volatile")}, {QStringLiteral("bind-activation"), QStringLiteral("dbus-client")}}), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation, owner] {
        const QDBusPendingReply<QDBusObjectPath, QDBusObjectPath, QVariantMap> reply = *watcher;
        watcher->deleteLater();
        finishActivation(reply.isError() ? QString() : reply.argumentAt<1>().path(), generation, owner);
        if (reply.isError() && generation == m_generation) endSession(i18n("Wi-Fi Direct connection failed: %1", reply.error().message()));
    });
}
void WifiDirectManager::finishActivation(const QString &path, unsigned generation, const QString &owner)
{
    if (generation != m_generation || !m_session) {
        if (!path.isEmpty()) {
            QDBusInterface nm(owner, nmPath, nmService, m_bus);
            nm.asyncCall(QStringLiteral("DeactivateConnection"), QVariant::fromValue(QDBusObjectPath(path)));
        }
        return;
    }
    m_activeConnection = path;
    poll();
}
bool WifiDirectManager::subnetConflict(const QNetworkInterface &iface) const
{
    for (const auto &entry : iface.addressEntries()) {
        if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.prefixLength() < 0) continue;
        for (const auto &other : QNetworkInterface::allInterfaces()) {
            if (other.index() == iface.index() || other.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
            for (const auto &address : other.addressEntries()) {
                if (address.ip().protocol() == QAbstractSocket::IPv4Protocol && address.prefixLength() >= 0
                    && (entry.ip().isInSubnet(address.ip(), address.prefixLength()) || address.ip().isInSubnet(entry.ip(), entry.prefixLength()))) return true;
            }
        }
    }
    return false;
}
void WifiDirectManager::poll()
{
    if (!nmProperty(m_bus, nmPath, nmService, "WirelessEnabled").toBool()) {
        endSession(i18n("Wi-Fi is disabled. Start a new search after enabling it."));
        m_available = false;
        m_unavailableReason = m_status;
        setState(QStringLiteral("unavailable"), m_status);
        return;
    }
    if (!originalWifiIntact()) {
        endSession(i18n("Ordinary Wi-Fi was lost. The adapter cannot preserve this connection."));
        return;
    }
    if (!m_session) { if (m_findRequested) refreshPeers(); return; }
    if (m_activeConnection.isEmpty()) {
        if (m_attempt.elapsed() >= m_timeouts.negotiation) endSession(i18n("Wi-Fi Direct negotiation and IP configuration timed out."));
        return;
    }
    const auto state = nmProperty(m_bus, m_activeConnection, nmService + QStringLiteral(".Connection.Active"), "State");
    if (!state.isValid() || state.toUInt() == 4) {
        endSession(i18n("Wi-Fi Direct disconnected. Start a new search to reconnect."));
        return;
    }
    if (!m_interfaceReady) {
        if (m_attempt.elapsed() >= m_timeouts.negotiation) {
            endSession(i18n("Wi-Fi Direct negotiation and IP configuration timed out."));
            return;
        }
        if (nmProperty(m_bus, m_device, nmService + QStringLiteral(".Device"), "State").toUInt() >= 70 && m_state == QLatin1String("connecting")) {
            setState(QStringLiteral("getting-ip"), i18n("Wi-Fi Direct group created. Waiting for an IP address..."));
        }
        if (state.toUInt() != 2) return;
        const QString name = nmProperty(m_bus, m_device, nmService + QStringLiteral(".Device"), "IpInterface").toString();
        const auto iface = QNetworkInterface::interfaceFromName(name);
        bool hasAddress = false;
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback() && entry.prefixLength() > 0) hasAddress = true;
        }
        if (!hasAddress || !iface.flags().testFlag(QNetworkInterface::IsUp)) return;
        if (subnetConflict(iface)) {
            endSession(i18n("The Wi-Fi Direct subnet conflicts with an existing local network."));
            return;
        }
        m_interface = name;
        m_interfaceReady = true;
        m_tlsWait.start();
        setState(QStringLiteral("verifying"), i18n("Wi-Fi Direct group ready. Checking KDE Connect over TLS..."));
        Q_EMIT interfaceReady(name);
    } else if (m_connectedDeviceId.isEmpty() && m_tlsWait.elapsed() >= m_timeouts.tls) {
        endSession(i18n("No KDE Connect TLS connection was established on this device within 30 seconds."));
    }
}
void WifiDirectManager::authenticatedDevice(const QString &id, bool paired)
{
    if (!m_session || !m_interfaceReady || (!m_connectedDeviceId.isEmpty() && m_connectedDeviceId != id)) return;
    m_connectedDeviceId = id;
    devicePaired(id, paired);
}
void WifiDirectManager::devicePaired(const QString &id, bool paired)
{
    if (id != m_connectedDeviceId || id.isEmpty()) return;
    setState(paired ? QStringLiteral("connected") : QStringLiteral("awaiting-pairing"),
             paired ? i18n("Connected through Wi-Fi Direct") : i18n("TLS connection established. Compare verification codes and confirm KDE Connect pairing."));
}
void WifiDirectManager::endSession(const QString &message)
{
    ++m_generation;
    stopFind();
    m_monitor.stop();
    m_session = false;
    m_interfaceReady = false;
    m_interface.clear();
    m_connectedDeviceId.clear();
    if (!m_activeConnection.isEmpty()) {
        QDBusInterface nm(m_owner, nmPath, nmService, m_bus);
        nm.asyncCall(QStringLiteral("DeactivateConnection"), QVariant::fromValue(QDBusObjectPath(m_activeConnection)));
        m_activeConnection.clear();
    }
    Q_EMIT sessionEnded();
    setState(m_available ? QStringLiteral("ready") : QStringLiteral("unavailable"), message);
}
void WifiDirectManager::disconnectSession()
{
    endSession(i18n("Wi-Fi Direct disconnected. Start a new search to reconnect."));
}
#include "moc_wifidirectmanager.cpp"
