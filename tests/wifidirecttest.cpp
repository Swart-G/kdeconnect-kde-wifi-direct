// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include <core/wifidirectmanager.h>
#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>
#ifdef Q_OS_LINUX
#include <linux/nl80211.h>
#endif

using ConnectionSettings = QMap<QString, QVariantMap>;
Q_DECLARE_METATYPE(ConnectionSettings)
static QDBusArgument &operator<<(QDBusArgument &a, const ConnectionSettings &settings)
{
    a.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QVariantMap>());
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) { a.beginMapEntry(); a << it.key() << it.value(); a.endMapEntry(); }
    a.endMap(); return a;
}
static const QDBusArgument &operator>>(const QDBusArgument &a, ConnectionSettings &settings)
{
    a.beginMap();
    while (!a.atEnd()) { QString key; QVariantMap value; a.beginMapEntry(); a >> key >> value; a.endMapEntry(); settings.insert(key, value); }
    a.endMap(); return a;
}
static const QString service = QStringLiteral("org.freedesktop.NetworkManager");
static const QString root = QStringLiteral("/org/freedesktop/NetworkManager");
static const QString p2pPath = root + QStringLiteral("/Devices/1");
static const QString staPath = root + QStringLiteral("/Devices/2");
static const QString peerPath = root + QStringLiteral("/Peers/1");
static const QString peer2Path = root + QStringLiteral("/Peers/2");
static const QString activePath = root + QStringLiteral("/ActiveConnection/1");

class MockActive : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Connection.Active")
    Q_PROPERTY(uint State MEMBER state)
public:
    uint state = 1;
};
class MockDevice : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device")
    Q_PROPERTY(uint DeviceType MEMBER type)
    Q_PROPERTY(uint State MEMBER state)
    Q_PROPERTY(bool Managed MEMBER managed)
    Q_PROPERTY(QString Interface MEMBER name)
    Q_PROPERTY(QString IpInterface MEMBER ipInterface)
    Q_PROPERTY(QDBusObjectPath ActiveConnection MEMBER active)
public:
    MockDevice(QObject *parent, uint deviceType) : QDBusAbstractAdaptor(parent), type(deviceType) {}
    uint type;
    uint state = 100;
    bool managed = true;
    QString name = QStringLiteral("test-wlan");
    QString ipInterface;
    QDBusObjectPath active{QStringLiteral("/")};
};
class MockP2p : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device.WifiP2P")
    Q_PROPERTY(QList<QDBusObjectPath> Peers MEMBER peers)
public:
    explicit MockP2p(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    QList<QDBusObjectPath> peers{QDBusObjectPath(peerPath)};
    int starts = 0;
    int stops = 0;
    QVariantMap options;
public Q_SLOTS:
    void StartFind(const QVariantMap &value) { ++starts; options = value; }
    void StopFind() { ++stops; }
Q_SIGNALS:
    void PeerAdded(const QDBusObjectPath &path);
    void PeerRemoved(const QDBusObjectPath &path);
};
class MockPeer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.WifiP2PPeer")
    Q_PROPERTY(QString Name MEMBER name)
    Q_PROPERTY(QString HwAddress MEMBER address)
public:
    QString name = QStringLiteral("Phone");
    QString address = QStringLiteral("02:00:00:00:00:01");
};
class MockNM : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager")
    Q_PROPERTY(bool WirelessEnabled MEMBER wireless)
public:
    bool wireless = true;
    bool haveP2p = true;
    bool deny = false;
    QList<QDBusObjectPath> extraDevices;
    int delay = 0;
    int additions = 0;
    QStringList deactivated;
    ConnectionSettings settings;
    QVariantMap options;
    QDBusObjectPath device{QStringLiteral("/")};
    QDBusObjectPath selected{QStringLiteral("/")};
public Q_SLOTS:
    QList<QDBusObjectPath> GetDevices() {
        auto result = haveP2p ? QList<QDBusObjectPath>{QDBusObjectPath(p2pPath), QDBusObjectPath(staPath)} : QList<QDBusObjectPath>{QDBusObjectPath(staPath)};
        return result + extraDevices;
    }
    QDBusObjectPath AddAndActivateConnection2(const ConnectionSettings &value, const QDBusObjectPath &dev, const QDBusObjectPath &peer,
                                            const QVariantMap &opts, QDBusObjectPath &active, QVariantMap &result)
    {
        ++additions; settings = value; options = opts; device = dev; selected = peer;
        if (deny) { sendErrorReply(QDBusError::AccessDenied, QStringLiteral("denied")); return QDBusObjectPath(QStringLiteral("/")); }
        active = QDBusObjectPath(activePath); result = {};
        if (delay) {
            setDelayedReply(true);
            const auto call = message(); const auto bus = connection();
            QTimer::singleShot(delay, this, [call, bus] {
                bus.send(call.createReply({QVariant::fromValue(QDBusObjectPath(root + QStringLiteral("/Settings/1"))),
                                           QVariant::fromValue(QDBusObjectPath(activePath)), QVariantMap{}}));
            });
        }
        return QDBusObjectPath(root + QStringLiteral("/Settings/1"));
    }
    void DeactivateConnection(const QDBusObjectPath &path) { deactivated.append(path.path()); }
Q_SIGNALS:
    void DeviceRemoved(const QDBusObjectPath &path);
};
class WifiDirectTest : public QObject {
    Q_OBJECT
    MockNM nm;
    QObject p2pObject, staObject;
    MockDevice *p2p = nullptr, *sta = nullptr;
    MockP2p *find = nullptr;
    MockPeer peer, peer2;
    MockActive active;
    WifiDirectCapabilities::Result capability;
    WifiDirectManager::CapabilityProbe probe() { return [this](const QString &) { return capability; }; }
    QString peerId(const WifiDirectManager &manager, int index = 0) { return manager.peers().at(index).toMap().value(QStringLiteral("id")).toString(); }
private Q_SLOTS:
    void initTestCase()
    {
        qDBusRegisterMetaType<ConnectionSettings>();
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(service));
        p2p = new MockDevice(&p2pObject, 30); sta = new MockDevice(&staObject, 2);
        find = new MockP2p(&p2pObject);
        for (const auto &item : QList<QPair<QString, QObject *>>{{root, &nm}, {p2pPath, &p2pObject}, {staPath, &staObject},
                    {peerPath, &peer}, {peer2Path, &peer2}, {activePath, &active}}) {
            QVERIFY(bus.registerObject(item.first, item.second, QDBusConnection::ExportAllContents | QDBusConnection::ExportAdaptors));
        }
    }
    void init()
    {
        nm.wireless = true; nm.haveP2p = true; nm.deny = false; nm.delay = 0; nm.additions = 0; nm.deactivated.clear();
        active.state = 1; find->starts = find->stops = 0; find->peers = {QDBusObjectPath(peerPath)};
        p2p->active = QDBusObjectPath(QStringLiteral("/"));
        sta->active = QDBusObjectPath(root + QStringLiteral("/ActiveConnection/STA"));
        sta->state = 100;
        peer.name = peer2.name = QStringLiteral("Phone");
        peer2.address = QStringLiteral("02:00:00:00:00:02");
        capability = {true, true, {}, {QStringLiteral("test-wlan")}};
    }
    void capabilities()
    {
#ifdef Q_OS_LINUX
        using namespace WifiDirectCapabilities;
        Radio radio;
        QVERIFY(!evaluate(radio).available);
        radio.modes = {NL80211_IFTYPE_STATION, NL80211_IFTYPE_P2P_CLIENT, NL80211_IFTYPE_P2P_GO};
        radio.stationConnected = true;
        radio.running[NL80211_IFTYPE_STATION] = 1;
        QVERIFY(!evaluate(radio).available);
        Combination combination{{{{NL80211_IFTYPE_STATION}, 1}, {{NL80211_IFTYPE_P2P_CLIENT, NL80211_IFTYPE_P2P_GO}, 1}}, 2, 1};
        radio.combinations = {combination};
        QVERIFY(!evaluate(radio).available);
        radio.combinations[0].channels = 2;
        QVERIFY(evaluate(radio).available);
        radio.occupiedChannels = 2;
        QVERIFY(!evaluate(radio).available);
        radio.combinations[0].channels = 3;
        QVERIFY(evaluate(radio).available);
        radio.occupiedChannels = 1;
        radio.combinations[0].channels = 2;
        radio.running[NL80211_IFTYPE_AP] = 1;
        QVERIFY(!evaluate(radio).available);
        radio.running.remove(NL80211_IFTYPE_AP);
        radio.running[NL80211_IFTYPE_P2P_DEVICE] = 1;
        radio.softwareModes = {NL80211_IFTYPE_P2P_DEVICE};
        QVERIFY(evaluate(radio).available);
        radio.modes.remove(NL80211_IFTYPE_P2P_GO);
        QVERIFY(!evaluate(radio).available);
        QVERIFY(!probeInvalidInterface().available);
#endif
    }
    void unavailable()
    {
        nm.haveP2p = false;
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        QVERIFY(!manager.available()); manager.startDiscovery(); QCOMPARE(find->starts, 0);
    }
    void prefersConnectedRadioThenStableName()
    {
        QObject secondObject;
        auto *second = new MockDevice(&secondObject, 30);
        second->name = QStringLiteral("a-radio");
        const auto secondPath = root + QStringLiteral("/Devices/3");
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerObject(secondPath, &secondObject, QDBusConnection::ExportAdaptors));
        nm.extraDevices = {QDBusObjectPath(secondPath)};
        p2p->name = QStringLiteral("z-radio");
        bool connected = true;
        auto read = [&connected](const QString &name) {
            WifiDirectCapabilities::Result result;
            result.available = true;
            result.stationConnected = connected && name == QLatin1String("z-radio");
            return result;
        };
        {
            WifiDirectManager manager(nullptr, bus, read);
            QCOMPARE(manager.m_device, p2pPath);
            connected = false;
            QVERIFY(manager.selectAdapter());
            QCOMPARE(manager.m_device, secondPath);
        }
        nm.extraDevices.clear();
        p2p->name = QStringLiteral("test-wlan");
        bus.unregisterObject(secondPath);
    }
    void unreadableCapabilities()
    {
        capability = {false, false, QStringLiteral("nl80211 unavailable"), {}};
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        QVERIFY(!manager.available()); QCOMPARE(manager.unavailableReason(), capability.reason);
        manager.startDiscovery(); QCOMPARE(find->starts, 0); QCOMPARE(nm.additions, 0);
    }
    void peerLifecycle()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        QCOMPARE(find->starts, 0); manager.startDiscovery(); QTRY_COMPARE(find->starts, 1);
        QCOMPARE(manager.peers().size(), 1); const auto id = peerId(manager);
        peer.name = QStringLiteral("Renamed"); find->PeerAdded(QDBusObjectPath(peerPath));
        QTRY_COMPARE(manager.peers()[0].toMap().value(QStringLiteral("name")).toString(), peer.name);
        QCOMPARE(peerId(manager), id);
        find->peers.append(QDBusObjectPath(peer2Path)); find->PeerAdded(QDBusObjectPath(peer2Path));
        QTRY_COMPARE(manager.peers().size(), 2); QVERIFY(peerId(manager) != peerId(manager, 1));
        find->peers.removeAll(QDBusObjectPath(peerPath)); find->PeerRemoved(QDBusObjectPath(peerPath));
        QTRY_VERIFY(!manager.peers()[0].toMap().value(QStringLiteral("available")).toBool());
        manager.stopDiscovery(); manager.connectPeer(id); QCOMPARE(nm.additions, 0);
    }
    void expiresWithoutRescanning()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe(), {60, 90000, 30000, 10});
        manager.startDiscovery(); QTRY_COMPARE(find->starts, 1);
        QCOMPARE(find->options.value(QStringLiteral("timeout")).toInt(), 60);
        QTRY_COMPARE(manager.state(), QStringLiteral("ready")); QTRY_COMPARE(find->stops, 1);
        QCOMPARE(manager.peers().size(), 1); QTest::qWait(80); QCOMPARE(find->starts, 1);
    }
    void temporaryP2pProfile()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        manager.startDiscovery(); manager.connectPeer(peerId(manager)); QTRY_COMPARE(nm.additions, 1);
        QCOMPARE(nm.device.path(), p2pPath); QCOMPARE(nm.selected.path(), peerPath);
        QCOMPARE(nm.settings[QStringLiteral("connection")][QStringLiteral("type")].toString(), QStringLiteral("wifi-p2p"));
        QVERIFY(!nm.settings.contains(QStringLiteral("802-11-wireless")));
        QCOMPARE(nm.options[QStringLiteral("persist")].toString(), QStringLiteral("volatile"));
        QCOMPARE(nm.options[QStringLiteral("bind-activation")].toString(), QStringLiteral("dbus-client"));
        QVERIFY(!nm.settings[QStringLiteral("connection")][QStringLiteral("autoconnect")].toBool());
        QVERIFY(nm.settings[QStringLiteral("ipv4")][QStringLiteral("never-default")].toBool());
        QVERIFY(nm.settings[QStringLiteral("ipv4")][QStringLiteral("ignore-auto-dns")].toBool());
        QCOMPARE(nm.settings[QStringLiteral("wifi-p2p")][QStringLiteral("wps-method")].toUInt(), 4u);
        QCOMPARE(sta->state, 100u); QVERIFY(nm.deactivated.isEmpty());
        manager.disconnectSession(); QTRY_COMPARE(nm.deactivated.size(), 1); QCOMPARE(nm.deactivated.first(), activePath);
    }
    void cancelsLateActivation()
    {
        nm.delay = 0;
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        manager.startDiscovery(); manager.connectPeer(peerId(manager)); manager.disconnectSession();
        QTRY_COMPARE(nm.deactivated.size(), 1); QCOMPARE(manager.state(), QStringLiteral("ready"));
        QVERIFY(manager.connectedDeviceId().isEmpty());
    }
    void denialAndTimeout()
    {
        nm.deny = true;
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe(), {60000, 80, 30000, 10});
        manager.startDiscovery(); manager.connectPeer(peerId(manager)); QTRY_COMPARE(manager.state(), QStringLiteral("ready"));
        nm.deny = false; manager.startDiscovery(); manager.connectPeer(peerId(manager));
        QTRY_COMPARE(nm.deactivated.size(), 1); QCOMPARE(manager.state(), QStringLiteral("ready"));
    }
    void preservesForeignSession()
    {
        p2p->active = QDBusObjectPath(root + QStringLiteral("/ActiveConnection/Foreign"));
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        QVERIFY(!manager.available()); manager.disconnectSession(); QCOMPARE(nm.deactivated.size(), 0);
    }
    void refusesAfterStaLoss()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        manager.startDiscovery(); sta->state = 30; manager.connectPeer(peerId(manager));
        QCOMPARE(nm.additions, 0); QVERIFY(manager.status().contains(QStringLiteral("Ordinary Wi-Fi")));
    }
    void wifiOffAndRemovedAdapter()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe(), {60000, 90000, 30000, 10});
        manager.startDiscovery(); nm.wireless = false; QTRY_COMPARE(manager.state(), QStringLiteral("unavailable"));
        nm.wireless = true; manager.startDiscovery(); nm.DeviceRemoved(QDBusObjectPath(p2pPath));
        QTRY_VERIFY(!manager.m_findRequested); QCOMPARE(nm.additions, 0);
    }
    void waitsForIpThenTimesOutWithoutKdeConnect()
    {
        QString name;
        for (const auto &iface : QNetworkInterface::allInterfaces()) {
            for (const auto &entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()
                    && iface.flags().testFlag(QNetworkInterface::IsUp)) { name = iface.name(); break; }
            }
            if (!name.isEmpty()) break;
        }
        if (name.isEmpty()) QSKIP("This IP readiness test needs an up IPv4 interface.");
        p2p->ipInterface = name;
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe(), {60000, 90000, 80, 10});
        QSignalSpy ready(&manager, &WifiDirectManager::interfaceReady);
        manager.startDiscovery(); manager.connectPeer(peerId(manager));
        QTRY_COMPARE(nm.additions, 1);
        QCOMPARE(ready.count(), 0);
        active.state = 2;
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(manager.connectedDeviceId().isEmpty());
        QTRY_COMPARE(nm.deactivated.size(), 1);
        QCOMPARE(manager.state(), QStringLiteral("ready"));
        QVERIFY(manager.status().contains(QStringLiteral("30 seconds")));
        p2p->ipInterface.clear();
    }
    void lostActiveSessionEndsWithoutReconnect()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe(), {60000, 90000, 30000, 10});
        manager.startDiscovery(); manager.connectPeer(peerId(manager)); QTRY_COMPARE(nm.additions, 1);
        active.state = 4;
        QTRY_COMPARE(nm.deactivated.size(), 1);
        QTest::qWait(60);
        QCOMPARE(nm.additions, 1); QCOMPARE(find->starts, 1);
    }
    void separatesTlsAndPairing()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        manager.startDiscovery(); manager.connectPeer(peerId(manager)); QTRY_COMPARE(nm.additions, 1);
        manager.m_interfaceReady = true;
        manager.setState(QStringLiteral("verifying"), QStringLiteral("TLS"));
        manager.authenticatedDevice(QStringLiteral("verified-id"), false);
        QCOMPARE(manager.state(), QStringLiteral("awaiting-pairing")); QCOMPARE(manager.connectedDeviceId(), QStringLiteral("verified-id"));
        manager.authenticatedDevice(QStringLiteral("different-id"), true); QCOMPARE(manager.connectedDeviceId(), QStringLiteral("verified-id"));
        manager.devicePaired(QStringLiteral("verified-id"), true); QCOMPARE(manager.state(), QStringLiteral("connected"));
        manager.disconnectSession(); QVERIFY(manager.connectedDeviceId().isEmpty());
    }
    void restartDoesNotAdoptOrReconnect()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus(), probe());
        manager.startDiscovery(); manager.connectPeer(peerId(manager)); QTRY_COMPARE(nm.additions, 1);
        auto bus = QDBusConnection::sessionBus(); bus.unregisterService(service);
        QTRY_VERIFY(!manager.m_session); QVERIFY(bus.registerService(service));
        QTest::qWait(80); QCOMPARE(nm.additions, 1); QCOMPARE(find->starts, 1);
    }
    void cleanupTestCase() { QDBusConnection::sessionBus().unregisterObject(root, QDBusConnection::UnregisterTree); QDBusConnection::sessionBus().unregisterService(service); }
private:
    WifiDirectCapabilities::Result probeInvalidInterface() { return WifiDirectCapabilities::probe(QStringLiteral("nonexistent-kdeconnect-interface")); }
};
QTEST_GUILESS_MAIN(WifiDirectTest)
#include "wifidirecttest.moc"
