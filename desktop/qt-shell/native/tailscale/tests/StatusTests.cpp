// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedStatusClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using namespace aurora::tailscale;

namespace {

QByteArray reply(const QByteArray &result)
{
    return "{\"id\":\"fixture\",\"ok\":true,\"error\":null,\"result\":" + result + "}";
}

QByteArray connectedFixture()
{
    QFile file(QStringLiteral(TAILNET_FIXTURES "/connected.json"));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Could not read connected fixture.");
    return file.readAll();
}

QString fixtureExecutable()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        QStringLiteral("AuroraTailnetFixture.exe")
#else
        QStringLiteral("AuroraTailnetFixture")
#endif
    );
}

StatusResult result(const QSignalSpy &completions, int index = 0)
{
    return completions.at(index).at(1).value<StatusResult>();
}

} // namespace

class StatusTests final : public QObject
{
    Q_OBJECT

private slots:
    void exactIdentityAndPeers()
    {
        const auto original = decodeStatusReply(connectedFixture(), "fixture");
        QCOMPARE(original.error, StatusError::None);
        QVERIFY(original.status && original.status->account);
        const auto &status = *original.status;
        QCOMPARE(status.state, BackendState::Connected);
        QCOMPARE(status.account->ownerId, qint64(9007199254740992));
        QCOMPARE(status.account->nodeId, QStringLiteral("stable-local"));
        QCOMPARE(status.account->tailnetId, QStringLiteral("stable-tailnet"));
        QCOMPARE(status.localAddress.toString(), QStringLiteral("100.76.1.10"));
        QCOMPARE(status.displayName, QStringLiteral("Fixture owner"));
        QCOMPARE(status.loginName, QStringLiteral("owner@example.invalid"));
        QVERIFY(status.loginUrl.isEmpty());
        QCOMPARE(status.peers.size(), 2);
        QCOMPARE(status.peers.at(0).nodeId, QStringLiteral("stable-peer"));
        QCOMPARE(status.peers.at(0).address.toString(), QStringLiteral("100.76.1.20"));
        QCOMPARE(status.peers.at(0).alternateAddress.toString(), QStringLiteral("fd7a:115c:a1e0::20"));
        QCOMPARE(status.peers.at(1).nodeId, QStringLiteral("stable-v6"));
        QCOMPARE(status.peers.at(1).name, QStringLiteral("desk-v6.fixture"));
        QVERIFY(!status.peers.at(1).online);
        QVERIFY(status.peers.at(1).alternateAddress.isNull());

        auto bytes = connectedFixture();
        bytes.replace("rotatable-key", "replacement-key").replace("\"Desk\"", "\"Renamed \\\"desk\\\" 1.2\"")
            .replace("fixture.example.invalid", "renamed.example.invalid");
        const auto renamed = decodeStatusReply(bytes, "fixture");
        QVERIFY(renamed.status && renamed.status->account);
        QVERIFY(*renamed.status->account == *status.account);
        QCOMPARE(renamed.status->peers.first().nodeId, status.peers.first().nodeId);
        QCOMPARE(renamed.status->peers.first().name, QStringLiteral("Renamed \"desk\" 1.2"));
    }

    void redactsInvalidIdentity_data()
    {
        QTest::addColumn<QByteArray>("fixture");
        QTest::newRow("tagged-self") << connectedFixture().replace("\"Tags\": null", "\"Tags\": [\"tag:server\"]");
        QTest::newRow("shared-self") << connectedFixture().replace("\"Tags\": null", "\"ShareeNode\": true");
        QTest::newRow("bad-tags") << connectedFixture().replace("\"Tags\": null", "\"Tags\": true");
        QTest::newRow("missing-tailnet-id") << connectedFixture().replace("\"StableID\": \"stable-tailnet\"", "\"StableID\": null");
        QTest::newRow("bad-local-id") << connectedFixture().replace("\"stable-local\"", "\"stable local\"");
        QTest::newRow("fractional-owner") << connectedFixture().replace(
            "\"ID\": \"stable-local\", \"UserID\": 9007199254740992,", "\"ID\": \"stable-local\", \"UserID\": 9007199254740992.1,");
        QTest::newRow("int64-overflow") << connectedFixture().replace(
            "\"ID\": \"stable-local\", \"UserID\": 9007199254740992,", "\"ID\": \"stable-local\", \"UserID\": 9223372036854775808,");
        QTest::newRow("signed-out-with-stale-fields") << connectedFixture().replace("\"BackendState\": \"Running\"", "\"BackendState\": \"Stopped\"");
    }

    void redactsInvalidIdentity()
    {
        QFETCH(QByteArray, fixture);
        const auto result = decodeStatusReply(fixture, "fixture");
        QCOMPARE(result.error, StatusError::None);
        QVERIFY(result.status);
        QVERIFY(!result.status->account);
        QVERIFY(result.status->peers.isEmpty());
        QVERIFY(result.status->displayName.isEmpty());
        QVERIFY(result.status->localAddress.isNull());
    }

    void exactMaximumAndSelfAddressFallback()
    {
        const auto result = decodeStatusReply(reply(
            "{\"BackendState\":\"Running\",\"Self\":{\"ID\":\"stable\",\"UserID\":9223372036854775807,"
            "\"TailscaleIPs\":[\"100.76.1.2\"]},\"CurrentTailnet\":{\"StableID\":\"net\"},"
            "\"User\":{\"9223372036854775807\":{\"ID\":9223372036854775807,\"DisplayName\":\"Maximum\"}}}"), "fixture");
        QVERIFY(result.status && result.status->account);
        QCOMPARE(result.status->account->ownerId, qint64(9223372036854775807LL));
        QCOMPARE(result.status->displayName, QStringLiteral("Maximum"));
        QCOMPARE(result.status->localAddress.toString(), QStringLiteral("100.76.1.2"));
    }

    void backendStatesAndLoginUrls()
    {
        const std::pair<const char *, BackendState> map[] = {
            std::pair<const char *, BackendState>{"Starting", BackendState::Starting},
            {"NoState", BackendState::Starting}, {"Stopped", BackendState::Stopped},
            {"NeedsMachineAuth", BackendState::NeedsApproval}, {"NewState", BackendState::Unknown}
        };
        for (const auto &item : map) {
            const auto result = decodeStatusReply(reply(
                QByteArray("{\"BackendState\":\"") + item.first + "\"}"), "fixture");
            QVERIFY(result.status);
            QCOMPARE(result.status->state, item.second);
        }
        for (const auto *url : {"https://login.tailscale.com/a/example", "https://login.tailscale.com:443/a/example",
                               "https://login.tailscale.com.invalid/a/example", "https://x@login.tailscale.com/a/example",
                               "https://@login.tailscale.com/a/example", "http://login.tailscale.com/a/example",
                               "https://login.tailscale.com:8443/a/example", "https://login.tailscale.com/admin",
                               "https://controlplane.tailscale.com/a/example"}) {
            const auto result = decodeStatusReply(reply(
                QByteArray("{\"BackendState\":\"NeedsLogin\",\"AuthURL\":\"") + url + "\"}"), "fixture");
            QVERIFY(result.status);
            QCOMPARE(result.status->state, BackendState::NeedsLogin);
            QCOMPARE(!result.status->loginUrl.isEmpty(), QByteArray(url).startsWith("https://login.tailscale.com/a/") ||
                QByteArray(url).startsWith("https://login.tailscale.com:443/a/"));
            QVERIFY(!result.status->account);
        }
    }

    void rejectsMalformedReplies()
    {
        for (const auto &line : {QByteArray("["), QByteArray("{}"), reply("{}"), reply("[]"),
                QByteArray("{\"id\":\"other\",\"ok\":true,\"result\":{},\"error\":null}"),
                QByteArray("{\"id\":\"fixture\",\"ok\":\"true\",\"result\":{},\"error\":null}"),
                QByteArray("{\"id\":\"fixture\",\"ok\":false,\"result\":null,\"error\":true}"),
                reply("{\"BackendState\":1,\"ignored\":-1.2e3}"), reply("{\"BackendState\":\"Starting\"}") + "\n{}"}) {
            const auto result = decodeStatusReply(line, "fixture");
            QCOMPARE(result.error, StatusError::InvalidResponse);
            QVERIFY(!result.status);
        }
        auto atLimit = reply("{\"BackendState\":\"Starting\"}");
        atLimit.append(QByteArray(maxStatusReplyBytes - atLimit.size(), ' '));
        QCOMPARE(decodeStatusReply(atLimit, "fixture").error, StatusError::None);
        QCOMPARE(decodeStatusReply(atLimit + ' ', "fixture").error, StatusError::ResponseTooLarge);
    }

    void processReuseAndGeneration()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EmbeddedStatusClient client(fixtureExecutable(), temp.filePath("state with spaces"));
        QVERIFY(client.isAvailable());
        QSignalSpy completions(&client, &EmbeddedStatusClient::statusFinished);
        QVERIFY(client.requestStatus(91));
        QVERIFY(!client.requestStatus(92));
        QVERIFY(completions.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(completions.size(), 1, 3000);
        QCOMPARE(completions.first().first().toULongLong(), quint64(91));
        QVERIFY(result(completions).status);
        QCOMPARE(result(completions).status->state, BackendState::Starting);

        QVERIFY(client.requestStatus(93));
        QTRY_COMPARE_WITH_TIMEOUT(completions.size(), 2, 3000);
        QCOMPARE(completions.last().first().toULongLong(), quint64(93));
        QVERIFY(result(completions, 1).status);
        // The fixture reports NeedsLogin only after seeing a second request on the same pipe.
        QCOMPARE(result(completions, 1).status->state, BackendState::NeedsLogin);
        QVERIFY(!QFile::exists(temp.filePath("state with spaces")));
    }

    void failedRequests_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("expected");
        QTest::newRow("id-mismatch") << QStringLiteral("wrong-id") << int(StatusError::InvalidResponse);
        QTest::newRow("extra-frame") << QStringLiteral("extra-frame") << int(StatusError::InvalidResponse);
        QTest::newRow("helper-declared-error") << QStringLiteral("rejected") << int(StatusError::HelperRejected);
        QTest::newRow("pipe-exit") << QStringLiteral("exit") << int(StatusError::Transport);
        QTest::newRow("oversize") << QStringLiteral("oversize") << int(StatusError::ResponseTooLarge);
        QTest::newRow("deadline-with-partial-frame") << QStringLiteral("silent") << int(StatusError::Timeout);
    }

    void failedRequests()
    {
        QFETCH(QString, scenario);
        QFETCH(int, expected);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EmbeddedStatusClient client(fixtureExecutable(), temp.filePath(scenario));
        QSignalSpy completions(&client, &EmbeddedStatusClient::statusFinished);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(client.requestStatus(9));
        QTRY_COMPARE_WITH_TIMEOUT(completions.size(), 1, 15500);
        QCOMPARE(completions.first().first().toULongLong(), quint64(9));
        QCOMPARE(int(result(completions).error), expected);
        QVERIFY(!result(completions).status);
        if (scenario == "silent")
            QVERIFY(clock.elapsed() >= 11900);

        bool retried = false;
        QTRY_VERIFY_WITH_TIMEOUT(retried || (retried = client.requestStatus(10)), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(completions.size(), 2, 3000);
        QCOMPARE(completions.last().first().toULongLong(), quint64(10));
        const auto retry = result(completions, 1);
        QCOMPARE(retry.error, StatusError::None);
        QVERIFY(retry.status);
        QCOMPARE(retry.status->state, scenario == "rejected" ? BackendState::NeedsLogin : BackendState::Starting);
    }

    void packageAndStatePaths()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto candidate = EmbeddedStatusClient::bundledExecutable(temp.path());
#ifdef Q_OS_WIN
        QCOMPARE(candidate, temp.filePath("Components/Tailscale/AuroraTailnet.exe"));
#elif defined(Q_OS_MACOS)
        QCOMPARE(candidate, temp.filePath("AuroraTailnet"));
#endif
        QVERIFY(EmbeddedStatusClient::bundledExecutable("relative").isEmpty());
        EmbeddedStatusClient absent(temp.filePath("missing.exe"), temp.filePath("state"));
        QSignalSpy missing(&absent, &EmbeddedStatusClient::statusFinished);
        QVERIFY(!absent.isAvailable());
        QVERIFY(absent.requestStatus(1));
        QTRY_COMPARE_WITH_TIMEOUT(missing.size(), 1, 1000);
        QCOMPARE(result(missing).error, StatusError::Unavailable);

        EmbeddedStatusClient invalid(fixtureExecutable(), "relative-state");
        QSignalSpy refused(&invalid, &EmbeddedStatusClient::statusFinished);
        QVERIFY(invalid.requestStatus(2));
        QTRY_COMPARE_WITH_TIMEOUT(refused.size(), 1, 1000);
        QCOMPARE(result(refused).error, StatusError::InvalidStateDirectory);
    }
};

QTEST_GUILESS_MAIN(StatusTests)
#include "StatusTests.moc"
