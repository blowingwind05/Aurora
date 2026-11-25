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
    QSignalSpy pairHosts;
    QSignalSpy pairOpens;
    QSignalSpy pairSources;
    QSignalSpy pairCloses;
    const QByteArray key = QByteArray(32, 'k');

    explicit Session(const QString &scenario)
        : client(fixtureExecutable(), temp.filePath(scenario)), statuses(&client, &EmbeddedStatusClient::statusFinished),
          hosts(&client, &EmbeddedStatusClient::hostFinished), publishes(&client, &EmbeddedStatusClient::publishFinished),
          presences(&client, &EmbeddedStatusClient::presenceFinished), opens(&client, &EmbeddedStatusClient::openFinished),
          closes(&client, &EmbeddedStatusClient::closeFinished), pairHosts(&client, &EmbeddedStatusClient::pairHostFinished),
          pairOpens(&client, &EmbeddedStatusClient::pairOpenFinished), pairSources(&client, &EmbeddedStatusClient::pairSourceFinished),
          pairCloses(&client, &EmbeddedStatusClient::pairCloseFinished)
    {
    }

    StatusResult latest() const { return statuses.constLast().at(1).value<StatusResult>(); }
    quint64 probe(char nonce = 'n') { return client.requestPresence(generation, "peer", peerV6, packet(1, nonce)); }
    quint64 open() { return client.requestOpen(generation, "peer", peerAddress); }
    OpenResult takeOpen() { return opens.takeFirst().at(2).value<OpenResult>(); }
    quint64 hostPair(const AccountIdentity &account) { return client.requestPairHost(generation, account, 54801, key); }
    quint64 openPair() { return client.requestPairOpen(generation, "peer", peerAddress, key); }
    quint64 verifyPair(const AccountIdentity &account, int port = 51007)
    {
        return client.requestPairSource(generation, account, "peer", peerAddress, port);
    }
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


    void pairingIdentityAndCloseNamespaces()
    {
        Session session("rpc-pair");
        const AccountIdentity account{(std::numeric_limits<qint64>::max)(), "self", "tailnet"};
        QCOMPARE(session.hostPair(account), quint64(0));
        QCOMPARE(session.openPair(), quint64(0));
        QCOMPARE(session.verifyPair(account), quint64(0));
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        QVERIFY(session.latest().status->account);

        auto otherAccount = account;
        --otherAccount.ownerId;
        QCOMPARE(session.hostPair(otherAccount), quint64(0));
        QCOMPARE(session.verifyPair(otherAccount), quint64(0));
        otherAccount = account;
        otherAccount.nodeId = "unknown-self";
        QCOMPARE(session.hostPair(otherAccount), quint64(0));
        otherAccount = account;
        otherAccount.tailnetId = "another-tailnet";
        QCOMPARE(session.verifyPair(otherAccount), quint64(0));
        QCOMPARE(session.client.requestPairHost(generation + 1, account, 54801, session.key), quint64(0));
        for (int port : {0, -1, 48990, 65536})
            QCOMPARE(session.client.requestPairHost(generation, account, port, session.key), quint64(0));
        for (int port : {0, -1, 65536})
            QCOMPARE(session.verifyPair(account, port), quint64(0));
        QCOMPARE(session.client.requestPairHost(generation, account, 54801, {}), quint64(0));
        QCOMPARE(session.client.requestPairOpen(generation, "peer", peerAddress, "short"), quint64(0));
        QCOMPARE(session.client.requestPairOpen(generation, "peer", peerV6, session.key), quint64(0));
        QCOMPARE(session.client.requestPairOpen(generation, "other", peerAddress, session.key), quint64(0));
        QCOMPARE(session.client.requestPairOpen(generation, "peer", QHostAddress("::ffff:100.76.1.20"), session.key), quint64(0));
        QCOMPARE(session.client.requestPairSource(generation, account, "", peerAddress, 51007), quint64(0));

        const auto hosted = session.hostPair(account);
        QVERIFY(hosted);
        QCOMPARE(session.openPair(), quint64(0)); // same foreground slot
        QTRY_COMPARE_WITH_TIMEOUT(session.pairHosts.size(), 1, 2000);
        QCOMPARE(session.pairHosts.first().at(0).toULongLong(), generation);
        QCOMPARE(session.pairHosts.first().at(1).toULongLong(), hosted);
        auto host = session.pairHosts.takeFirst().at(2).value<PairingLeaseResult>();
        QCOMPARE(host.error, StatusError::None);
        QVERIFY(host.lease && host.lease->isValid() && !host.lease->localPort());

        const auto streamed = session.open();
        QVERIFY(streamed); // the fixture uses the same wire ID in the two Go lease maps
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        auto stream = session.takeOpen();
        QVERIFY(stream.lease && stream.lease->isValid());

        const auto dialed = session.openPair();
        QVERIFY(dialed);
        QTRY_COMPARE_WITH_TIMEOUT(session.pairOpens.size(), 1, 2000);
        QCOMPARE(session.pairOpens.first().at(1).toULongLong(), dialed);
        auto dial = session.pairOpens.takeFirst().at(2).value<PairingLeaseResult>();
        QCOMPARE(dial.error, StatusError::None);
        QVERIFY(dial.lease && dial.lease->isValid());
        QCOMPARE(dial.lease->address(), QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(dial.lease->localPort().value_or(0), quint16(55011));

        Session other("rpc-pair");
        QVERIFY(!other.client.requestClose(*host.lease));
        QVERIFY(host.lease->close());
        QVERIFY(!host.lease->isValid());
        QVERIFY(!host.lease->close());
        QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 1, 2000);
        QCOMPARE(session.pairCloses.first().at(0).toULongLong(), generation);
        QCOMPARE(session.pairCloses.first().at(1).toULongLong(), hosted);
        QCOMPARE(session.pairCloses.first().at(2).value<StatusError>(), StatusError::None);
        QVERIFY(stream.lease->isValid());

        dial.lease.reset(); // last reference schedules confirmed auxiliary close
        QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 2, 2000);
        QCOMPARE(session.pairCloses.last().at(1).toULongLong(), dialed);
        QCOMPARE(session.pairCloses.last().at(2).value<StatusError>(), StatusError::None);
        QVERIFY(stream.lease->close());
        QTRY_COMPARE_WITH_TIMEOUT(session.closes.size(), 1, 2000);
        QCOMPARE(session.closes.first().at(1).toULongLong(), streamed);
        QCOMPARE(session.closes.first().at(2).value<StatusError>(), StatusError::None);

        for (int port : {51007, 51008}) {
            const auto checked = session.verifyPair(account, port);
            QVERIFY(checked);
            QTRY_COMPARE_WITH_TIMEOUT(session.pairSources.size(), 1, 2000);
            const auto signal = session.pairSources.takeFirst();
            QCOMPARE(signal.at(0).toULongLong(), generation);
            QCOMPARE(signal.at(1).toULongLong(), checked);
            const auto source = signal.at(2).value<PairSourceResult>();
            QCOMPARE(source.error, StatusError::None);
            QCOMPARE(source.verified, port == 51007);
        }
    }

    void cancelsPairingBeforeAndAfterSend_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::newRow("host") << QStringLiteral("host");
        QTest::newRow("open") << QStringLiteral("open");
        QTest::newRow("source") << QStringLiteral("source");
    }

    void cancelsPairingBeforeAndAfterSend()
    {
        QFETCH(QString, operation);
        Session session("rpc-pair-cancel");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        const auto account = *session.latest().status->account;
        auto &finished = operation == "source" ? session.pairSources :
            operation == "host" ? session.pairHosts : session.pairOpens;
        const auto request = [&] {
            if (operation == "source") return session.verifyPair(account);
            if (operation == "host") return session.hostPair(account);
            return session.openPair();
        };
        const auto check = [&](quint64 id) {
            QCOMPARE(finished.first().at(0).toULongLong(), generation);
            QCOMPARE(finished.first().at(1).toULongLong(), id);
            const auto payload = finished.takeFirst().at(2);
            if (operation == "source") {
                const auto result = payload.value<PairSourceResult>();
                QCOMPARE(result.error, StatusError::Cancelled);
                QVERIFY(!result.verified);
            } else {
                const auto result = payload.value<PairingLeaseResult>();
                QCOMPARE(result.error, StatusError::Cancelled);
                QVERIFY(!result.lease);
            }
        };

        const auto unsent = request();
        QVERIFY(unsent && session.client.cancelPairing(unsent));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
        check(unsent);
        QVERIFY(session.pairCloses.isEmpty());

        QVERIFY(session.probe());
        const auto sent = request();
        QVERIFY(sent);
        // The first probe completes only after the fake child has read the pairing request.
        QTRY_COMPARE_WITH_TIMEOUT(session.presences.size(), 1, 800);
        QVERIFY(session.client.cancelPairing(sent));
        QVERIFY(session.probe());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
        check(sent);
        QVERIFY(!session.client.cancelPairing(sent));
        if (operation == "source") {
            QVERIFY(session.pairCloses.isEmpty());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 1, 2000);
            QCOMPARE(session.pairCloses.first().at(1).toULongLong(), sent);
            QCOMPARE(session.pairCloses.first().at(2).value<StatusError>(), StatusError::None);
        }
    }

    void generationOrPeerChangeRetiresLease_data()
    {
        QTest::addColumn<bool>("advanceGeneration");
        QTest::newRow("generation-changed") << true;
        QTest::newRow("peer-address-changed") << false;
    }

    void hostPairingSurvivesSameAccountStatusRefresh()
    {
        Session session("rpc-pair-host-account-change");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        const auto account = *session.latest().status->account;
        const auto id = session.hostPair(account);
        QVERIFY(id);
        QTRY_COMPARE_WITH_TIMEOUT(session.pairHosts.size(), 1, 2000);
        const auto result = session.pairHosts.takeFirst().at(2).value<PairingLeaseResult>();
        QVERIFY(result.lease && result.lease->isValid());

        QVERIFY(session.client.requestStatus(generation + 1));
        QVERIFY(result.lease->isValid());
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 2, 2000);
        QVERIFY(result.lease->isValid());
        QVERIFY(session.pairCloses.isEmpty());
        QCOMPARE(session.verifyPair(account), quint64(0));

        QVERIFY(session.client.requestStatus(generation + 2));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 3, 2000);
        QVERIFY(!result.lease->isValid());
        QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 1, 2000);
        QCOMPARE(session.pairCloses.first().at(1).toULongLong(), id);
        QCOMPARE(session.pairCloses.first().at(2).value<StatusError>(), StatusError::None);
    }

    void generationOrPeerChangeRetiresLease()
    {
        QFETCH(bool, advanceGeneration);
        Session session(advanceGeneration ? "rpc-pair" : "rpc-pair-changed");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        const auto id = session.openPair();
        QVERIFY(id);
        QTRY_COMPARE_WITH_TIMEOUT(session.pairOpens.size(), 1, 2000);
        auto opened = session.pairOpens.takeFirst().at(2).value<PairingLeaseResult>();
        QVERIFY(opened.lease && opened.lease->isValid());

        QVERIFY(session.client.requestStatus(generation + int(advanceGeneration)));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 2, 2000);
        QVERIFY(!opened.lease->isValid());
        QVERIFY(!opened.lease->close());
        QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 1, 2000);
        QCOMPARE(session.pairCloses.first().at(0).toULongLong(), generation);
        QCOMPARE(session.pairCloses.first().at(1).toULongLong(), id);
        QCOMPARE(session.pairCloses.first().at(2).value<StatusError>(), StatusError::None);
    }

    void badPairingReplies_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("bad-host-shape") << QStringLiteral("rpc-pair-bad-host");
        QTest::newRow("rounded-port") << QStringLiteral("rpc-pair-bad-port");
        QTest::newRow("nonboolean-source") << QStringLiteral("rpc-pair-bad-source");
        QTest::newRow("helper-rejects-source") << QStringLiteral("rpc-pair-reject");
    }

    void badPairingReplies()
    {
        QFETCH(QString, scenario);
        Session session(scenario);
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        const auto account = *session.latest().status->account;
        if (scenario == "rpc-pair-bad-host" || scenario == "rpc-pair-bad-port") {
            const bool hosting = scenario == "rpc-pair-bad-host";
            QVERIFY(hosting ? session.hostPair(account) : session.openPair());
            const auto &finished = hosting ? session.pairHosts : session.pairOpens;
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
            const auto result = finished.first().at(2).value<PairingLeaseResult>();
            QCOMPARE(result.error, StatusError::InvalidResponse);
            QVERIFY(!result.lease);
        } else {
            QVERIFY(session.verifyPair(account));
            QTRY_COMPARE_WITH_TIMEOUT(session.pairSources.size(), 1, 2000);
            const auto result = session.pairSources.first().at(2).value<PairSourceResult>();
            QCOMPARE(result.error, scenario == "rpc-pair-reject" ? StatusError::HelperRejected : StatusError::InvalidResponse);
            QVERIFY(!result.verified);
        }
        if (scenario == "rpc-pair-reject") {
            QVERIFY(session.client.requestStatus(generation));
            QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 2, 2000);
            QCOMPARE(session.latest().error, StatusError::None);
        } else {
            QCOMPARE(session.openPair(), quint64(0));
        }
    }

    void unconfirmedPairCloseInvalidatesAuxiliaryLeases_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("invalid-ack") << QStringLiteral("rpc-pair-close-invalid");
        QTest::newRow("no-ack") << QStringLiteral("rpc-pair-close-silent");
    }

    void unconfirmedPairCloseInvalidatesAuxiliaryLeases()
    {
        QFETCH(QString, scenario);
        Session session(scenario);
        QSignalSpy invalidated(&session.client, &EmbeddedStatusClient::leasesInvalidated);
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);

        const auto id = session.openPair();
        QVERIFY(id);
        QTRY_COMPARE_WITH_TIMEOUT(session.pairOpens.size(), 1, 2000);
        auto first = session.pairOpens.takeFirst().at(2).value<PairingLeaseResult>();
        QVERIFY(first.lease && first.lease->isValid());
        QVERIFY(session.openPair());
        QTRY_COMPARE_WITH_TIMEOUT(session.pairOpens.size(), 1, 2000);
        auto second = session.pairOpens.takeFirst().at(2).value<PairingLeaseResult>();
        QVERIFY(second.lease && second.lease->isValid());
        QVERIFY(first.lease->close());
        QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 1, 4500);
        QCOMPARE(session.pairCloses.first().at(1).toULongLong(), id);
        QCOMPARE(session.pairCloses.first().at(2).value<StatusError>(),
                 scenario == "rpc-pair-close-silent" ? StatusError::Timeout : StatusError::InvalidResponse);
        QCOMPARE(invalidated.size(), 1);
        QVERIFY(!second.lease->isValid());
        QVERIFY(!second.lease->close());
    }

    void auxiliaryLimitIsIndependentFromStreamLeases()
    {
        Session session("rpc-pair");
        QVERIFY(session.client.requestStatus(generation));
        QTRY_COMPARE_WITH_TIMEOUT(session.statuses.size(), 1, 2000);
        QVector<std::shared_ptr<PairingLease>> owned;
        for (int i = 0; i < 16; ++i) {
            QVERIFY(session.openPair());
            QTRY_COMPARE_WITH_TIMEOUT(session.pairOpens.size(), 1, 2000);
            auto result = session.pairOpens.takeFirst().at(2).value<PairingLeaseResult>();
            QVERIFY(result.lease && result.lease->isValid());
            owned.append(std::move(result.lease));
        }
        QCOMPARE(session.openPair(), quint64(0));
        QVERIFY(session.open());
        QTRY_COMPARE_WITH_TIMEOUT(session.opens.size(), 1, 2000);
        QCOMPARE(session.takeOpen().error, StatusError::None);
        QTRY_COMPARE_WITH_TIMEOUT(session.closes.size(), 1, 2000);
        owned.clear();
        QTRY_COMPARE_WITH_TIMEOUT(session.pairCloses.size(), 16, 2500);
        for (const auto &completion : session.pairCloses)
            QCOMPARE(completion.at(2).value<StatusError>(), StatusError::None);
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
