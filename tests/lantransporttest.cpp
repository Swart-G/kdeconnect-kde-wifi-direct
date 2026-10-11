// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include <core/backends/lan/lanlinkprovider.h>
#include <core/kdeconnectconfig.h>
#include <core/sslhelper.h>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

class LanTransportTest : public QObject {
    Q_OBJECT
    QSslKey key;
    QSslCertificate certificate;
    const QString id = QStringLiteral("verified_id");
    QSslSocket *tlsSocket(LanLinkProvider &provider, const QString &source, const QSslCertificate &cert, const QSslKey &remoteKey)
    {
        auto *server = new Server(this);
        if (!server->listen(QHostAddress::LocalHost, 0)) return nullptr;
        QSslSocket *remote = nullptr;
        connect(server, &QTcpServer::newConnection, this, [server, &remote, cert, remoteKey] {
            remote = server->nextPendingConnection();
            remote->setLocalCertificate(cert);
            remote->setPrivateKey(remoteKey);
            remote->setPeerVerifyMode(QSslSocket::QueryPeer);
            remote->startServerEncryption();
        });
        auto *local = new QSslSocket(&provider);
        local->setLocalCertificate(KdeConnectConfig::instance().certificate());
        local->setPrivateKey(KdeConnectConfig::instance().privateKey());
        local->setPeerVerifyMode(QSslSocket::QueryPeer);
        if (!local->bind(QHostAddress(source))) return nullptr;
        local->connectToHostEncrypted(QStringLiteral("127.0.0.1"), server->serverPort());
        QElapsedTimer timer; timer.start();
        while ((!local->isEncrypted() || !remote || !remote->isEncrypted()) && timer.elapsed() < 3000) QTest::qWait(10);
        disconnect(server, nullptr, this, nullptr);
        return local->isEncrypted() && remote && remote->isEncrypted() ? local : nullptr;
    }
    DeviceInfo info(QSslSocket *socket)
    {
        auto result = KdeConnectConfig::instance().deviceInfo();
        result.id = id;
        result.certificate = socket->peerCertificate();
        return result;
    }
    void directAddress(LanLinkProvider &provider)
    {
        QNetworkAddressEntry entry;
        entry.setIp(QHostAddress(QStringLiteral("127.0.0.3")));
        entry.setPrefixLength(32);
        provider.m_directAddresses = {entry};
        provider.m_directGeneration = 7;
    }
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        key = SslHelper::generateEcPrivateKey();
        certificate = SslHelper::generateSelfSignedCertificate(key, id);
        QVERIFY(!certificate.isNull());
    }
    void prefersDirectSocketAndRetainsCertificateCheck()
    {
        LanLinkProvider provider(true);
        auto *lan = tlsSocket(provider, QStringLiteral("127.0.0.2"), certificate, key);
        QVERIFY(lan);
        provider.addLink(lan, info(lan));
        QVERIFY(provider.m_links.contains(id));
        directAddress(provider);
        auto *direct = tlsSocket(provider, QStringLiteral("127.0.0.3"), certificate, key);
        QVERIFY(direct);
        direct->setProperty("directGeneration", 7u);
        provider.addLink(direct, info(direct));
        QCOMPARE(provider.m_links[id]->localAddress(), QHostAddress(QStringLiteral("127.0.0.3")));
        auto *rediscoveredLan = tlsSocket(provider, QStringLiteral("127.0.0.2"), certificate, key);
        QVERIFY(rediscoveredLan);
        provider.addLink(rediscoveredLan, info(rediscoveredLan));
        QCOMPARE(provider.m_links[id]->localAddress(), QHostAddress(QStringLiteral("127.0.0.3")));
        QCOMPARE(rediscoveredLan->state(), QAbstractSocket::UnconnectedState);
        const auto changedKey = SslHelper::generateEcPrivateKey();
        const auto changedCert = SslHelper::generateSelfSignedCertificate(changedKey, id);
        auto *impostor = tlsSocket(provider, QStringLiteral("127.0.0.3"), changedCert, changedKey);
        QVERIFY(impostor);
        impostor->setProperty("directGeneration", 7u);
        provider.addLink(impostor, info(impostor));
        QCOMPARE(impostor->state(), QAbstractSocket::UnconnectedState);
        QVERIFY(direct->isEncrypted());
        QCOMPARE(provider.m_links[id]->deviceInfo().certificate, certificate);
    }
    void rejectsCancelledHandshake()
    {
        LanLinkProvider provider(true);
        directAddress(provider);
        auto *socket = tlsSocket(provider, QStringLiteral("127.0.0.3"), certificate, key);
        QVERIFY(socket);
        socket->setProperty("directGeneration", 6u);
        provider.addLink(socket, info(socket));
        QVERIFY(provider.m_links.isEmpty());
        QCOMPARE(socket->state(), QAbstractSocket::UnconnectedState);
    }
};
QTEST_GUILESS_MAIN(LanTransportTest)
#include "lantransporttest.moc"
