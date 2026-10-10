// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include <core/wifidirectmanager.h>

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QSignalSpy>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QTest>

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

static const QString service = QStringLiteral("org.freedesktop.NetworkManager");
static const QString rootPath = QStringLiteral("/org/freedesktop/NetworkManager");
static const QString devicePath = rootPath + QStringLiteral("/Devices/1");
static const QString pointPath = rootPath + QStringLiteral("/AccessPoint/1");
static const QString profilePath = rootPath + QStringLiteral("/Settings/1");
static const QString activePath = rootPath + QStringLiteral("/ActiveConnection/1");
static const QString phoneSsid = QStringLiteral("DIRECT-KC-KDEConnect-test");

class MockProfile : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Settings.Connection")
public:
    ConnectionSettings settings;
public Q_SLOTS:
    ConnectionSettings GetSettings() { return settings; }
    void Update(const ConnectionSettings &value) { settings = value; }
};

class MockActiveConnection : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Connection.Active")
    Q_PROPERTY(uint State MEMBER state)
    Q_PROPERTY(QString Id MEMBER id)
public:
    uint state = 2;
    QString id = QStringLiteral("KDE Connect Direct %1").arg(phoneSsid);
};

class MockNetworkManager : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager")
    Q_PROPERTY(bool WirelessEnabled MEMBER wirelessEnabled)
    Q_PROPERTY(QList<QDBusObjectPath> ActiveConnections READ activeConnections)
public:
    bool wirelessEnabled = true;
    MockProfile *profile = nullptr;
    bool active = false;
    int additions = 0;
    int activations = 0;
    int deactivations = 0;
    QVariantMap options;
    QList<QDBusObjectPath> activeConnections() const { return active ? QList<QDBusObjectPath>{QDBusObjectPath(activePath)} : QList<QDBusObjectPath>{}; }
public Q_SLOTS:
    QList<QDBusObjectPath> GetDevices() { return {QDBusObjectPath(devicePath)}; }
    QDBusObjectPath AddAndActivateConnection2(const ConnectionSettings &settings, const QDBusObjectPath &, const QDBusObjectPath &,
                                            const QVariantMap &value, QDBusObjectPath &connection, QVariantMap &result)
    {
        ++additions;
        active = true;
        profile->settings = settings;
        options = value;
        connection = QDBusObjectPath(activePath);
        result = {};
        return QDBusObjectPath(profilePath);
    }
    QDBusObjectPath ActivateConnection(const QDBusObjectPath &, const QDBusObjectPath &, const QDBusObjectPath &)
    {
        ++activations;
        active = true;
        return QDBusObjectPath(activePath);
    }
    void DeactivateConnection(const QDBusObjectPath &) { ++deactivations; active = false; }
};

class MockDevice : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device")
    Q_PROPERTY(uint DeviceType READ deviceType)
    Q_PROPERTY(bool Managed READ managed)
public:
    explicit MockDevice(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    uint deviceType() const { return 2; }
    bool managed() const { return true; }
};

class MockWireless : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device.Wireless")
public:
    explicit MockWireless(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    int scans = 0;
public Q_SLOTS:
    void RequestScan(const QVariantMap &) { ++scans; }
    QList<QDBusObjectPath> GetAllAccessPoints() { return {QDBusObjectPath(pointPath)}; }
};

class MockAccessPoint : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.AccessPoint")
    Q_PROPERTY(QByteArray Ssid MEMBER ssid)
public:
    QByteArray ssid = phoneSsid.toUtf8();
};

class MockSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Settings")
public:
    MockProfile *profile = nullptr;
public Q_SLOTS:
    QList<QDBusObjectPath> ListConnections() { return profile->settings.isEmpty() ? QList<QDBusObjectPath>{} : QList<QDBusObjectPath>{QDBusObjectPath(profilePath)}; }
};

class WifiDirectTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        auto bus = QDBusConnection::sessionBus();
        qDBusRegisterMetaType<ConnectionSettings>();
        m_nm.profile = &m_profile;
        m_settings.profile = &m_profile;
        QVERIFY(bus.registerService(service));
        new MockDevice(&m_device);
        m_wireless = new MockWireless(&m_device);
        QVERIFY(bus.registerObject(rootPath, &m_nm, QDBusConnection::ExportAllContents));
        QVERIFY(bus.registerObject(devicePath, &m_device, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerObject(pointPath, &m_point, QDBusConnection::ExportAllContents));
        QVERIFY(bus.registerObject(profilePath, &m_profile, QDBusConnection::ExportAllContents));
        QVERIFY(bus.registerObject(activePath, &m_active, QDBusConnection::ExportAllContents));
        QVERIFY(bus.registerObject(rootPath + QStringLiteral("/Settings"), &m_settings, QDBusConnection::ExportAllContents));
    }

    void init()
    {
        m_nm.active = false;
        m_nm.additions = 0;
        m_nm.activations = 0;
        m_nm.deactivations = 0;
        m_profile.settings.clear();
    }

    void savesAndReusesPassword()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        QSignalSpy activated(&manager, &WifiDirectManager::networkActivated);
        manager.startDiscovery();
        manager.connectNetwork(phoneSsid, QStringLiteral("test-wifi-password"));
        QVERIFY(!manager.searching());
        QTRY_COMPARE(activated.count(), 1);
        QVERIFY(!manager.connecting());
        QCOMPARE(m_nm.additions, 1);
        QCOMPARE(m_nm.options.value(QStringLiteral("persist")).toString(), QStringLiteral("disk"));
        QVERIFY(!m_profile.settings.value(QStringLiteral("connection")).value(QStringLiteral("autoconnect")).toBool());
        QVERIFY(m_profile.settings.value(QStringLiteral("ipv4")).value(QStringLiteral("never-default")).toBool());
        QVERIFY(m_profile.settings.value(QStringLiteral("ipv4")).value(QStringLiteral("ignore-auto-dns")).toBool());
        QCOMPARE(m_profile.settings.value(QStringLiteral("802-11-wireless-security")).value(QStringLiteral("psk")).toString(), QStringLiteral("test-wifi-password"));
        manager.disconnectNetwork();
        QTRY_COMPARE(m_nm.deactivations, 1);
        manager.startDiscovery();
        manager.connectNetwork(phoneSsid, QString());
        QTRY_COMPARE(activated.count(), 2);
        QCOMPARE(m_nm.additions, 1);
        QCOMPARE(m_nm.activations, 1);
        manager.disconnectNetwork();
        QTRY_COMPARE(m_nm.deactivations, 2);
    }

    void cancelsPendingActivation()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        QSignalSpy activated(&manager, &WifiDirectManager::networkActivated);
        manager.startDiscovery();
        manager.connectNetwork(phoneSsid, QStringLiteral("test-wifi-password"));
        manager.disconnectNetwork();
        QVERIFY(!manager.connecting());
        QVERIFY(!manager.searching());
        QTRY_COMPARE(m_nm.deactivations, 1);
        QCOMPARE(activated.count(), 0);
    }

    void adoptsActiveConnectionAfterRestart()
    {
        m_nm.active = true;
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        QVERIFY(!manager.searching());
        manager.disconnectNetwork();
        QTRY_COMPARE(m_nm.deactivations, 1);
    }

    void manualSearchOnly()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        QVERIFY(!manager.searching());
        QVERIFY(!manager.connecting());
        QVERIFY(manager.networks().isEmpty());
        const int scans = m_wireless->scans;
        QTest::qWait(2100);
        QCOMPARE(m_wireless->scans, scans);
        manager.startDiscovery();
        QVERIFY(manager.searching());
        QCOMPARE(manager.networks(), QStringList{phoneSsid});
        QTRY_VERIFY(m_wireless->scans > scans);
        manager.stopDiscovery();
        QVERIFY(!manager.searching());
        const int stoppedScans = m_wireless->scans;
        QTest::qWait(2100);
        QCOMPARE(m_wireless->scans, stoppedScans);
    }

    void filtersOtherNetworks()
    {
        m_point.ssid = QByteArrayLiteral("unrelated-wifi");
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        manager.startDiscovery();
        QVERIFY(manager.networks().isEmpty());
        manager.stopDiscovery();
        m_point.ssid = phoneSsid.toUtf8();
    }

    void rejectsMissingAndInvalidPassword()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        manager.connectNetwork(phoneSsid, QStringLiteral("valid-password"));
        QVERIFY(!manager.connecting());
        manager.startDiscovery();
        manager.connectNetwork(phoneSsid, QString());
        QVERIFY(!manager.connecting());
        QVERIFY(manager.searching());
        manager.connectNetwork(phoneSsid, QStringLiteral("short"));
        QVERIFY(!manager.connecting());
        QVERIFY(manager.searching());
        manager.stopDiscovery();
    }

    void disabledWifiStopsSearch()
    {
        m_nm.wirelessEnabled = false;
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        manager.startDiscovery();
        QVERIFY(!manager.searching());
        QVERIFY(manager.networks().isEmpty());
        m_nm.wirelessEnabled = true;
    }

    void searchExpires()
    {
        WifiDirectManager manager(nullptr, QDBusConnection::sessionBus());
        manager.startDiscovery();
        QVERIFY(manager.searching());
        QTRY_VERIFY_WITH_TIMEOUT(!manager.searching(), 65000);
        QCOMPARE(manager.networks(), QStringList{phoneSsid});
        const int scans = m_wireless->scans;
        QTest::qWait(2100);
        QCOMPARE(m_wireless->scans, scans);
    }

    void cleanupTestCase()
    {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(rootPath, QDBusConnection::UnregisterTree);
        bus.unregisterService(service);
    }

private:
    MockNetworkManager m_nm;
    QObject m_device;
    MockWireless *m_wireless = nullptr;
    MockAccessPoint m_point;
    MockSettings m_settings;
    MockProfile m_profile;
    MockActiveConnection m_active;
};

QTEST_GUILESS_MAIN(WifiDirectTest)
#include "wifidirecttest.moc"
