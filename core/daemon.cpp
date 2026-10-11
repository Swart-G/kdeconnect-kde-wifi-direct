/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "daemon.h"

#include <QDBusMetaType>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>

#include "core_debug.h"
#include "dbushelper.h"
#include "kdeconnectconfig.h"
#include "networkpacket.h"
#include "notificationserverinfo.h"
#include "wifidirectmanager.h"

#include "backends/devicelink.h"
#include "backends/lan/lanlinkprovider.h"
#include "backends/linkprovider.h"
#include "backends/loopback/loopbacklinkprovider.h"
#include "device.h"

static Daemon *s_instance = nullptr;

struct DaemonPrivate {
    // Different ways to find devices and connect to them
    QSet<LinkProvider *> m_linkProviders;

    // Every known device
    QMap<QString, Device *> m_devices;

    bool m_testMode;
    WifiDirectManager *m_wifiDirect = nullptr;
};

Daemon *Daemon::instance()
{
    Q_ASSERT(s_instance != nullptr);
    return s_instance;
}

Daemon::Daemon(QObject *parent, bool testMode)
    : QObject(parent)
    , d(new DaemonPrivate)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    d->m_testMode = testMode;

    // HACK init may call pure virtual functions from this class so it can't be called directly from the ctor
    QTimer::singleShot(0, this, &Daemon::init);
}

void Daemon::init()
{
    qCDebug(KDECONNECT_CORE) << "Daemon starting";

    // Register on DBus
    // This must happen as early as possible in the process startup to ensure
    // the absolute minimum amount of blocking on logon/autostart

    qDBusRegisterMetaType<QMap<QString, QString>>();
    qDBusRegisterMetaType<QByteArrayList>();
    QDBusConnection::sessionBus().registerService(QStringLiteral("org.kde.kdeconnect"));
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/modules/kdeconnect"), this, QDBusConnection::ExportScriptableContents);

    qCDebug(KDECONNECT_CORE) << "DBus registration complete";

    auto configInstance = KdeConnectConfig::instance();
    const auto disabledLinkProviders = configInstance.disabledLinkProviders();

    qCDebug(KDECONNECT_CORE) << "Constructing Backends";
    // Load backends
    if (d->m_testMode) {
        d->m_linkProviders.insert(new LoopbackLinkProvider(false));
        qCDebug(KDECONNECT_CORE) << "Constructed LoopbackLink Backend";
    } else {
        auto *lan = new LanLinkProvider(false, disabledLinkProviders.contains(QStringLiteral("lan")));
        d->m_linkProviders.insert(lan);
        qCDebug(KDECONNECT_CORE) << "Constructed LanLinkProvider Backend";
        d->m_wifiDirect = new WifiDirectManager(this);
        connect(d->m_wifiDirect, &WifiDirectManager::changed, this, &Daemon::wifiDirectChanged);
        connect(d->m_wifiDirect, &WifiDirectManager::interfaceReady, lan, &LanLinkProvider::setWifiDirectInterface);
        connect(d->m_wifiDirect, &WifiDirectManager::sessionEnded, lan, [lan] { lan->setWifiDirectInterface({}); });
        connect(lan, &LanLinkProvider::wifiDirectLinkAuthenticated, this, [this](const QString &id) {
            auto *device = getDevice(id);
            d->m_wifiDirect->authenticatedDevice(id, device && device->isPaired());
        });
        connect(lan, &LanLinkProvider::wifiDirectLinkLost, d->m_wifiDirect, &WifiDirectManager::disconnectSession);
#ifdef KDECONNECT_LOOPBACK
        d->m_linkProviders.insert(new LoopbackLinkProvider(disabledLinkProviders.contains(QStringLiteral("loopback"))));
        qCDebug(KDECONNECT_CORE) << "Constructed LoopbackLinkProvider Backend";
#endif
    }
    Q_EMIT linkProvidersChanged(linkProviders());

    qCDebug(KDECONNECT_CORE) << "Backends loaded";

    // Read remembered paired devices
    const QStringList &list = KdeConnectConfig::instance().trustedDevices();
    for (const QString &id : list) {
        Device *d = new Device(this, id);
        // prune away devices with malformed certificates
        if (d->hasInvalidCertificate()) {
            qCDebug(KDECONNECT_CORE) << "Certificate for device " << id << "invalid, deleting the device";
            KdeConnectConfig::instance().removeTrustedDevice(id);
        } else {
            addDevice(d);
        }
    }

    qCDebug(KDECONNECT_CORE) << "Paired devices added";

    // Listen to new devices
    for (LinkProvider *a : std::as_const(d->m_linkProviders)) {
        connect(a, &LinkProvider::onConnectionReceived, this, &Daemon::onNewDeviceLink);
        a->onStart();
    }

    qCDebug(KDECONNECT_CORE) << "Link providers started";

    NotificationServerInfo::instance().init();

    qCDebug(KDECONNECT_CORE) << "Daemon start completed";
}

void Daemon::removeDevice(Device *device)
{
    d->m_devices.remove(device->id());
    device->deleteLater();
    Q_EMIT deviceRemoved(device->id());
    Q_EMIT deviceListChanged();
}

void Daemon::forceOnNetworkChange()
{
    qCDebug(KDECONNECT_CORE) << "Sending onNetworkChange to" << d->m_linkProviders.size() << "LinkProviders";
    for (LinkProvider *a : std::as_const(d->m_linkProviders)) {
        qCDebug(KDECONNECT_CORE) << "Sending onNetworkChange to:" << a->id();
        a->onNetworkChange();
    }
}

QString Daemon::wifiDirectStatus() const
{
    return d->m_wifiDirect ? d->m_wifiDirect->status() : QString();
}

bool Daemon::wifiDirectAvailable() const { return d->m_wifiDirect && d->m_wifiDirect->available(); }
QString Daemon::wifiDirectUnavailableReason() const { return d->m_wifiDirect ? d->m_wifiDirect->unavailableReason() : QString(); }
QString Daemon::wifiDirectState() const { return d->m_wifiDirect ? d->m_wifiDirect->state() : QStringLiteral("unavailable"); }
QVariantList Daemon::wifiDirectPeers() const { return d->m_wifiDirect ? d->m_wifiDirect->peers() : QVariantList(); }
QString Daemon::wifiDirectConnectedDeviceId() const { return d->m_wifiDirect ? d->m_wifiDirect->connectedDeviceId() : QString(); }
void Daemon::startWifiDirectDiscovery() { if (d->m_wifiDirect) d->m_wifiDirect->startDiscovery(); }
void Daemon::stopWifiDirectDiscovery() { if (d->m_wifiDirect) d->m_wifiDirect->stopDiscovery(); }
void Daemon::connectWifiDirectPeer(const QString &id) { if (d->m_wifiDirect) d->m_wifiDirect->connectPeer(id); }
void Daemon::disconnectWifiDirectSession() { if (d->m_wifiDirect) d->m_wifiDirect->disconnectSession(); }

Device *Daemon::getDevice(const QString &deviceId)
{
    for (Device *device : std::as_const(d->m_devices)) {
        if (device->id() == deviceId) {
            return device;
        }
    }
    return nullptr;
}

QSet<LinkProvider *> Daemon::getLinkProviders() const
{
    return d->m_linkProviders;
}

QStringList Daemon::linkProviders() const
{
    auto configInstance = KdeConnectConfig::instance();
    const auto disabledLinkProviders = configInstance.disabledLinkProviders();
    QStringList returnValue;

    for (LinkProvider *a : std::as_const(d->m_linkProviders)) {
        QString line(a->displayName() + u'|' + a->id());

        if (disabledLinkProviders.contains(a->id())) {
            line += QStringLiteral("|disabled");
        } else {
            line += QStringLiteral("|enabled");
        }

        returnValue.append(line);
    }
    returnValue.sort();
    return returnValue;
}

void Daemon::setLinkProviderState(const QString &linkProviderName, bool enabled)
{
    qCDebug(KDECONNECT_CORE) << "setLinkProviderState called" << linkProviderName << enabled;

    KdeConnectConfig configInstance = KdeConnectConfig::instance();
    LinkProvider *providerByName = nullptr;
    const auto allLinkProviders = getLinkProviders();
    for (LinkProvider *provider : allLinkProviders) {
        if (provider->id().toLower() == linkProviderName.toLower() || provider->displayName().toLower() == linkProviderName.toLower()) {
            providerByName = provider;
            break;
        }
    }
    if (!providerByName) return;

    auto disabledLinkProviders = configInstance.disabledLinkProviders();
    if (!enabled && !disabledLinkProviders.contains(providerByName->id())) {
        qCDebug(KDECONNECT_CORE) << "disabling" << providerByName->id();
        disabledLinkProviders.append(providerByName->id());
        providerByName->disable();
    } else if (enabled && disabledLinkProviders.contains(providerByName->id())) {
        qCDebug(KDECONNECT_CORE) << "enabling " << providerByName->id();
        disabledLinkProviders.removeAll(providerByName->id());
        providerByName->enable();
    } else {
        qCDebug(KDECONNECT_CORE) << "No changes to the disabledLinkProviders have been made, returning";
        return;
    }

    configInstance.setDisabledLinkProviders(disabledLinkProviders);
    Q_EMIT linkProvidersChanged(linkProviders());
}

QStringList Daemon::devices(bool onlyReachable, bool onlyTrusted) const
{
    QStringList ret;
    for (Device *device : std::as_const(d->m_devices)) {
        if (onlyReachable && !device->isReachable())
            continue;
        if (onlyTrusted && !device->isPaired())
            continue;
        ret.append(device->id());
    }
    return ret;
}

QMap<QString, QString> Daemon::deviceNames(bool onlyReachable, bool onlyTrusted) const
{
    QMap<QString, QString> ret;
    for (Device *device : std::as_const(d->m_devices)) {
        if (onlyReachable && !device->isReachable())
            continue;
        if (onlyTrusted && !device->isPaired())
            continue;
        ret[device->id()] = device->name();
    }
    return ret;
}

void Daemon::onNewDeviceLink(DeviceLink *link)
{
    QString id = link->deviceId();
    auto it = d->m_devices.find(id);
    bool isKnown = it != d->m_devices.end();
    qCDebug(KDECONNECT_CORE) << id << "connected, name" << link->deviceInfo().name << (isKnown ? "(already known device)" : "(new device)") << "via"
                             << link->provider();
    if (isKnown) {
        Device *device = it.value();
        bool wasReachable = device->isReachable();
        device->addLink(link);
        if (!wasReachable) {
            Q_EMIT deviceVisibilityChanged(id, true);
            Q_EMIT deviceListChanged();
        }
    } else {
        Device *device = new Device(this, link);
        addDevice(device);
    }
}

void Daemon::onDeviceStatusChanged()
{
    Device *device = (Device *)sender();
    if (d->m_wifiDirect) d->m_wifiDirect->devicePaired(device->id(), device->isPaired());

    qCDebug(KDECONNECT_CORE) << "Device" << device->name() << "status changed. Reachable:" << device->isReachable() << ". Paired: " << device->isPaired();

    if (!device->isReachable() && !device->isPaired()) {
        qCDebug(KDECONNECT_CORE) << "Destroying device" << device->name();
        removeDevice(device);
    } else {
        Q_EMIT deviceVisibilityChanged(device->id(), device->isReachable());
        Q_EMIT deviceListChanged();
    }
}

void Daemon::setAnnouncedName(const QString &name)
{
    QString filteredName = DeviceInfo::filterName(name);
    qCDebug(KDECONNECT_CORE) << "Announcing name";
    KdeConnectConfig::instance().setName(filteredName);
    forceOnNetworkChange();
    Q_EMIT announcedNameChanged(filteredName);
}

void Daemon::setCustomDevices(const QStringList &addresses)
{
    auto &config = KdeConnectConfig::instance();

    auto customDevices = config.customDevices();
    if (customDevices != addresses) {
        qCDebug(KDECONNECT_CORE) << "Changed list of custom device addresses:" << addresses;
        config.setCustomDevices(addresses);
        Q_EMIT customDevicesChanged(addresses);

        forceOnNetworkChange();
    }
}

QStringList Daemon::customDevices() const
{
    return KdeConnectConfig::instance().customDevices();
}

QString Daemon::announcedName()
{
    return KdeConnectConfig::instance().name();
}

QNetworkAccessManager *Daemon::networkAccessManager()
{
    static QPointer<QNetworkAccessManager> manager;
    if (!manager) {
        manager = new QNetworkAccessManager(this);
    }
    return manager;
}

QString Daemon::deviceIdByName(const QString &name) const
{
    for (Device *device : std::as_const(d->m_devices)) {
        if (device->name() == name) {
            return device->id();
        }
    }
    return {};
}

void Daemon::addDevice(Device *device)
{
    const QString id = device->id();
    connect(device, &Device::reachableChanged, this, &Daemon::onDeviceStatusChanged);
    connect(device, &Device::pairStateChanged, this, &Daemon::onDeviceStatusChanged);
    connect(device, &Device::pairStateChanged, this, &Daemon::pairingRequestsChanged);
    connect(device, &Device::pairStateChanged, this, [this, device](int pairStateAsInt) {
        PairState pairState = (PairState)pairStateAsInt;
        if (pairState == PairState::RequestedByPeer)
            askPairingConfirmation(device);
    });
    d->m_devices[id] = device;

    Q_EMIT deviceAdded(id);
    Q_EMIT deviceListChanged();
}

QStringList Daemon::pairingRequests() const
{
    QStringList ret;
    for (Device *dev : std::as_const(d->m_devices)) {
        if (dev->isPairRequestedByPeer())
            ret += dev->id();
    }
    return ret;
}

Daemon::~Daemon()
{
}

QString Daemon::selfId() const
{
    return KdeConnectConfig::instance().deviceId();
}

#include "moc_daemon.cpp"
