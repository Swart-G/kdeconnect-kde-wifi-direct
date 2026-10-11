// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include <declarativeplugin/responsewaiter.h>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingReply>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

class PeerListService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.test.peers")
public Q_SLOTS:
    QVariantList peers() { return {QVariantMap{{QStringLiteral("id"), QStringLiteral("first")}, {QStringLiteral("name"), QStringLiteral("Phone")}, {QStringLiteral("available"), true}}}; }
};
class DBusResponseTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void decodesPeerObjectsForQml()
    {
        auto server = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("peer-list-test"));
        QThread serverThread;
        auto *service = new PeerListService;
        service->moveToThread(&serverThread);
        connect(&serverThread, &QThread::finished, service, &QObject::deleteLater);
        serverThread.start();
        QVERIFY(server.registerService(QStringLiteral("org.kde.kdeconnect.test.peers")));
        QVERIFY(server.registerObject(QStringLiteral("/peers"), service, QDBusConnection::ExportAllSlots));
        QDBusInterface remote(QStringLiteral("org.kde.kdeconnect.test.peers"), QStringLiteral("/peers"), QStringLiteral("org.kde.kdeconnect.test.peers"), QDBusConnection::sessionBus());
        DBusAsyncResponse response;
        QSignalSpy success(&response, &DBusAsyncResponse::success);
        const QDBusPendingReply<QVariantList> reply = remote.asyncCall(QStringLiteral("peers"));
        response.setPendingCall(QVariant::fromValue(reply));
        QTRY_COMPARE(success.size(), 1);
        const auto list = success[0][0].toList();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list[0].toMap()[QStringLiteral("id")].toString(), QStringLiteral("first"));
        QVERIFY(list[0].toMap()[QStringLiteral("available")].toBool());
        server.unregisterObject(QStringLiteral("/peers"));
        serverThread.quit();
        serverThread.wait();
        server.unregisterService(QStringLiteral("org.kde.kdeconnect.test.peers"));
        QDBusConnection::disconnectFromBus(QStringLiteral("peer-list-test"));
    }
};
QTEST_GUILESS_MAIN(DBusResponseTest)
#include "dbusresponsetest.moc"
