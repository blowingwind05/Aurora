// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedStatusClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

using namespace aurora::tailscale;

namespace {

constexpr quint64 generation = 243;
const QHostAddress peerAddress(QStringLiteral("100.76.1.20"));
const QHostAddress peerV6(QStringLiteral("fd7a:115c:a1e0::20"));

QString fixtureExecutable()
{
    auto file = QCoreApplication::applicationDirPath() + "/AuroraTailnetFixture";
#ifdef Q_OS_WIN
    file += ".exe";
#endif
    return file;
}

QByteArray packet(char kind, char nonce = 'n')
{
    QByteArray data(192, '\0');
    data.replace(0, 17, "AURORA/PRESENCE/1");
    data[17] = kind;
    data[18] = nonce;
    return data;
}

struct Session
{
    QTemporaryDir temp;
    EmbeddedStatusClient client;
    QSignalSpy statuses;
    QSignalSpy hosts;
    QSignalSpy publishes;
    QSignalSpy presences;
    QSignalSpy opens;
    QSignalSpy closes;

    explicit Session(const QString &scenario)
        : client(fixtureExecutable(), temp.filePath(scenario)), statuses(&client, &EmbeddedStatusClient::statusFinished),
          hosts(&client, &EmbeddedStatusClient::hostFinished), publishes(&client, &EmbeddedStatusClient::publishFinished),
          presences(&client, &EmbeddedStatusClient::presenceFinished), opens(&client, &EmbeddedStatusClient::openFinished),
          closes(&client, &EmbeddedStatusClient::closeFinished)
    {
    }

    StatusResult latest() const { return statuses.constLast().at(1).value<StatusResult>(); }
    quint64 probe(char nonce = 'n') { return client.requestPresence(generation, "peer", peerV6, packet(1, nonce)); }
    quint64 open() { return client.requestOpen(generation, "peer", peerAddress); }
    OpenResult takeOpen() { return opens.takeFirst().at(2).value<OpenResult>(); }
};

} // namespace

class RpcTests : public QObject
{
    Q_OBJECT

private slots:
    void accountAndPublish()
    {
        Session session("rpc-session");
        QVERIFY(session.temp.isValid());
        QVERIFY(!session.client.requestHost(generation, true));
        QVERIFY(!session.client.requestHost(generation, false));
        QVERIFY(!session.client.requestPublish(generation, false));
        QCOMPARE(session.probe(), quint64(0));
        QCOMPARE(session.open(), quint64(0));

        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        const auto status = session.latest();
        QCOMPARE(status.error, StatusError::None);
        QVERIFY(status.status && status.status->account);
        const auto &account = *status.status->account;
        QCOMPARE(account.ownerId, (std::numeric_limits<qint64>::max)());

        QVERIFY(!session.client.requestHost(generation + 1, true));
        QVERIFY(!session.client.requestPublish(generation + 1, true, account, packet(2)));
        QVERIFY(!session.client.requestPublish(generation, true, account, packet(1)));
        auto other = account;
        --other.ownerId;
        QVERIFY(!session.client.requestPublish(generation, true, other, packet(2)));
        const QHostAddress outside("192.0.2.10");
        QCOMPARE(session.client.requestOpen(generation, "peer", peerV6), quint64(0));
        QCOMPARE(session.client.requestOpen(generation, "peer", outside), quint64(0));
        QCOMPARE(session.client.requestOpen(generation, "other", peerAddress), quint64(0));
        QCOMPARE(session.client.requestPresence(generation, "peer", outside, packet(1)), quint64(0));
        QCOMPARE(session.client.requestPresence(generation, "peer", peerAddress, packet(2)), quint64(0));
        QCOMPARE(session.client.requestPresence(generation + 1, "peer", peerAddress, packet(1)), quint64(0));

        QVERIFY(session.client.requestHost(generation, true));
        QVERIFY(!session.client.requestPublish(generation, true, account, packet(2)));
        QTRY_COMPARE_WITH_TIMEOUT(session.hosts.size(), 1, 2000);
        QCOMPARE(session.hosts.first().at(1).value<StatusError>(), StatusError::None);
        QVERIFY(session.client.requestPublish(generation, true, account, packet(2)));
        QTRY_COMPARE_WITH_TIMEOUT(session.publishes.size(), 1, 2000);
        QCOMPARE(session.publishes.first().at(1).value<StatusError>(), StatusError::None);
        QVERIFY(session.client.requestPublish(generation, false));
        QTRY_COMPARE_WITH_TIMEOUT(session.publishes.size(), 2, 2000);
        QCOMPARE(session.publishes.last().at(1).value<StatusError>(), StatusError::None);
        QVERIFY(session.client.requestHost(generation, false));
        QTRY_COMPARE_WITH_TIMEOUT(session.hosts.size(), 2, 2000);
        QCOMPARE(session.hosts.last().at(1).value<StatusError>(), StatusError::None);
        QVERIFY(!QFile::exists(session.temp.filePath("rpc-session")));
    }

    void parallelProbesAndMutations()
    {
        Session session("rpc-batch");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        QVector<quint64> ids;
        for (int index = 0; index < 8; ++index) {
            const auto id = session.probe(char(index));
            QVERIFY(id);
            ids.append(id);
        }
        QCOMPARE(session.probe(), quint64(0));
        QVERIFY(session.client.requestHost(generation, true));
        QTRY_COMPARE_WITH_TIMEOUT(session.presences.size(), 8, 800);
        QTRY_COMPARE_WITH_TIMEOUT(session.hosts.size(), 1, 1000);
        QCOMPARE(session.hosts.first().at(1).value<StatusError>(), StatusError::None);
        for (int index = 0; index < ids.size(); ++index) {
            const auto &reply = session.presences.at(index);
            QCOMPARE(reply.at(0).toULongLong(), generation);
            QCOMPARE(reply.at(1).toULongLong(), ids.at(7 - index));
            const auto result = reply.at(2).value<PresenceResult>();
            QCOMPARE(result.error, StatusError::None);
            QCOMPARE(result.packet, packet(2, char(7 - index)));
        }
    }

    void expiredProbesStillOccupySlotsUntilDrained()
    {
        Session session("rpc-late");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        for (int index = 0; index < 8; ++index)
            QVERIFY(session.probe(char(index)));
        QTRY_COMPARE_WITH_TIMEOUT(session.presences.size(), 8, 1800);
        for (const auto &completion : session.presences)
            QCOMPARE(completion.at(2).value<PresenceResult>().error, StatusError::Timeout);
        QCOMPARE(session.probe(), quint64(0));

        QVERIFY(session.client.requestHost(generation, true)); // tells the fixture to flush the late replies
        QTRY_COMPARE_WITH_TIMEOUT(session.hosts.size(), 1, 2000);
        quint64 next = 0;
        QTRY_VERIFY_WITH_TIMEOUT(next || (next = session.probe()), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(session.presences.size(), 9, 800);
        QCOMPARE(session.presences.last().at(2).value<PresenceResult>().error, StatusError::None);
    }

    void cancelledAllocationIsClosed()
    {
        Session session("rpc-cancel");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);

        const auto unsent = session.open();
        QVERIFY(unsent && session.client.cancelOpen(unsent));
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        QCOMPARE(session.takeOpen().error, StatusError::Cancelled);
        QVERIFY(session.closes.isEmpty());

        QVERIFY(session.probe());
        const auto sent = session.open();
        QVERIFY(sent);
        // The fixture only releases this probe's reply after it has read the open.
        QTRY_COMPARE_WITH_TIMEOUT(session.presences.size(), 1, 800);
        QCOMPARE(session.presences.first().at(2).value<PresenceResult>().error, StatusError::None);
        QVERIFY(session.client.cancelOpen(sent));
        QVERIFY(session.probe());
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        QCOMPARE(session.opens.first().at(1).toULongLong(), sent);
        const auto cancelled = session.takeOpen();
        QCOMPARE(cancelled.error, StatusError::Cancelled);
        QVERIFY(!cancelled.lease);
        QTRY_COMPARE_WITH_TIMEOUT(session.closes.size(), 1, 2000);
        QCOMPARE(session.closes.first().at(1).toULongLong(), sent);
        QCOMPARE(session.closes.first().at(2).value<StatusError>(), StatusError::None);
        QVERIFY(!session.client.cancelOpen(sent));

        QVERIFY(session.open());
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        QCOMPARE(session.takeOpen().error, StatusError::None);
    }

    void liveLeaseIsLoopbackAndClosesOnlyOnce()
    {
        Session session("rpc-session");
        Session other("rpc-session");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        const auto id = session.open();
        QVERIFY(id);
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        auto opened = session.takeOpen();
        QCOMPARE(opened.error, StatusError::None);
        QVERIFY(opened.lease && opened.lease->isValid());
        QCOMPARE(opened.lease->address(), QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(opened.lease->port(), quint16(55011));
        QVERIFY(!other.client.requestClose(*opened.lease));
        QVERIFY(opened.lease->close());
        QVERIFY(!opened.lease->isValid());
        QVERIFY(!opened.lease->close());
        QTRY_COMPARE_WITH_TIMEOUT(session.closes.size(), 1, 2000);
        QCOMPARE(session.closes.first().at(1).toULongLong(), id);
        QCOMPARE(session.closes.first().at(2).value<StatusError>(), StatusError::None);

        const auto owned = session.open();
        QVERIFY(owned);
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        opened = session.takeOpen();
        QVERIFY(opened.lease->isValid());
        opened.lease.reset();
        QTRY_COMPARE_WITH_TIMEOUT(session.closes.size(), 2, 2000);
        QCOMPARE(session.closes.last().at(1).toULongLong(), owned);
        QCOMPARE(session.closes.last().at(2).value<StatusError>(), StatusError::None);
    }

    void invalidOpenNeverPublishesEndpoint()
    {
        Session session("rpc-bad-open");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        QVERIFY(session.open());
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        const auto result = session.takeOpen();
        QCOMPARE(result.error, StatusError::InvalidResponse);
        QVERIFY(!result.lease);
        QCOMPARE(session.open(), quint64(0));
    }

    void childFailureInvalidatesOtherLeases()
    {
        Session session("rpc-close-fault");
        QSignalSpy invalidated(&session.client, &EmbeddedStatusClient::leasesInvalidated);
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        QVERIFY(session.open());
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        const auto first = session.takeOpen();
        QVERIFY(first.lease && first.lease->isValid());
        QVERIFY(session.open());
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        const auto second = session.takeOpen();
        QVERIFY(second.lease && second.lease->isValid());
        QVERIFY(first.lease->close());
        QTRY_COMPARE_WITH_TIMEOUT(invalidated.size(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(session.closes.size(), 1, 2000);
        QCOMPARE(session.closes.first().at(2).value<StatusError>(), StatusError::Transport);
        QVERIFY(!second.lease->isValid());
        QVERIFY(!second.lease->close());
        QCOMPARE(session.open(), quint64(0));
    }
};

QTEST_GUILESS_MAIN(RpcTests)
#include "RpcTests.moc"
