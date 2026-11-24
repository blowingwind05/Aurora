// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedStatusClient.h"
#include "ProviderSelection.h"

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


    void actionReplyShapes()
    {
        const auto pending = decodeLoginReply(reply("{\"pending\":true}"), "fixture");
        QCOMPARE(pending.error, StatusError::None);
        QVERIFY(pending.pending && *pending.pending);
        const auto ready = decodeLoginReply(reply("{\"pending\":false}"), "fixture");
        QCOMPARE(ready.error, StatusError::None);
        QVERIFY(ready.pending && !*ready.pending);

        QCOMPARE(decodeLogoutReply(reply("{}"), "fixture"), StatusError::None);
        for (const auto *body : {"{}", "[]", "null", "{\"pending\":1}", "{\"pending\":\"true\"}",
                "{\"pending\":null}", "{\"pending\":true,\"AuthURL\":\"https://login.tailscale.com/a/ignored\"}"}) {
            const auto invalid = decodeLoginReply(reply(body), "fixture");
            QCOMPARE(invalid.error, StatusError::InvalidResponse);
            QVERIFY(!invalid.pending);
        }
        for (const auto *body : {"[]", "null", "true", "{\"pending\":false}", "{\"ok\":true}"})
            QCOMPARE(decodeLogoutReply(reply(body), "fixture"), StatusError::InvalidResponse);

        const QByteArray rejected("{\"id\":\"fixture\",\"ok\":false,\"error\":\"fixture refusal\",\"result\":null}");
        QCOMPARE(decodeLoginReply(rejected, "fixture").error, StatusError::HelperRejected);
        QCOMPARE(decodeLogoutReply(rejected, "fixture"), StatusError::HelperRejected);

        for (const auto *frame : {
                 "{\"id\":\"fixture\",\"ok\":true,\"result\":{}}",
                 "{\"id\":\"fixture\",\"ok\":true,\"error\":null,\"result\":{},\"extra\":true}",
                 "{\"id\":1,\"ok\":true,\"error\":null,\"result\":{}}",
                 "{\"id\":\"fixture\",\"ok\":false,\"error\":null,\"result\":null}",
                 "{\"id\":\"fixture\",\"ok\":false,\"error\":\"fixture refusal\",\"result\":{}}"}) {
            QCOMPARE(decodeLoginReply(frame, "fixture").error, StatusError::InvalidResponse);
            QCOMPARE(decodeLogoutReply(frame, "fixture"), StatusError::InvalidResponse);
        }

        auto atLimit = reply("{\"pending\":true}");
        atLimit.append(QByteArray(maxActionReplyBytes - atLimit.size(), ' '));
        QCOMPARE(decodeLoginReply(atLimit, "fixture").error, StatusError::None);
        QCOMPARE(decodeLoginReply(atLimit + ' ', "fixture").error, StatusError::ResponseTooLarge);
        QCOMPARE(decodeLogoutReply(atLimit + ' ', "fixture"), StatusError::ResponseTooLarge);
    }

    void actionsShareTheStatusProcess()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto state = temp.filePath("session");
        EmbeddedStatusClient client(fixtureExecutable(), state);
        QSignalSpy statuses(&client, &EmbeddedStatusClient::statusFinished);
        QSignalSpy logins(&client, &EmbeddedStatusClient::loginFinished);
        QSignalSpy logouts(&client, &EmbeddedStatusClient::logoutFinished);

        QVERIFY(client.requestStatus(201));
        QVERIFY(!client.requestLogin(202));
        QVERIFY(!client.requestLogout(202));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 3000);
        QCOMPARE(statuses.first().first().toULongLong(), quint64(201));
        QVERIFY(result(statuses).status && result(statuses).status->loginUrl.isEmpty());
        QCOMPARE(result(statuses).status->state, BackendState::NeedsLogin);

        QVERIFY(client.requestLogin(203));
        QVERIFY(!client.requestStatus(204));
        QVERIFY(!client.requestLogout(204));
        QVERIFY(logins.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(logins.size(), 1, 3000);
        QCOMPARE(logins.first().first().toULongLong(), quint64(203));
        const auto queued = logins.first().at(1).value<LoginResult>();
        QCOMPARE(queued.error, StatusError::None);
        QVERIFY(queued.pending && *queued.pending);
        QCOMPARE(statuses.size(), 1);
        QVERIFY(logouts.isEmpty());

        QVERIFY(client.requestStatus(205));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 2, 3000);
        const auto authorizing = result(statuses, 1);
        QVERIFY(authorizing.status);
        QCOMPARE(authorizing.status->loginUrl, QUrl("https://login.tailscale.com/a/offline-fixture"));

        QVERIFY(client.requestStatus(206));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 3, 3000);
        const auto running = result(statuses, 2);
        QVERIFY(running.status && running.status->loginUrl.isEmpty());
        QCOMPARE(running.status->state, BackendState::Connected);

        QVERIFY(client.requestLogin(207));
        QTRY_COMPARE_WITH_TIMEOUT(logins.size(), 2, 3000);
        QCOMPARE(logins.last().first().toULongLong(), quint64(207));
        const auto alreadyRunning = logins.last().at(1).value<LoginResult>();
        QCOMPARE(alreadyRunning.error, StatusError::None);
        QVERIFY(alreadyRunning.pending && !*alreadyRunning.pending);

        QVERIFY(client.requestLogout(208));
        QVERIFY(!client.requestLogin(209));
        QVERIFY(!client.requestStatus(209));
        QTRY_COMPARE_WITH_TIMEOUT(logouts.size(), 1, 3000);
        QCOMPARE(logouts.first().first().toULongLong(), quint64(208));
        QCOMPARE(logouts.first().at(1).value<StatusError>(), StatusError::None);
        QCOMPARE(statuses.size(), 3);

        QVERIFY(client.requestStatus(210));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 4, 3000);
        QCOMPARE(statuses.last().first().toULongLong(), quint64(210));
        const auto stopped = result(statuses, 3);
        QVERIFY(stopped.status && stopped.status->loginUrl.isEmpty());
        QCOMPARE(stopped.status->state, BackendState::Stopped);
        QVERIFY(!QFile::exists(state));
    }

    void failedActionRequests_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("expected");
        QTest::newRow("login-id-mismatch") << QStringLiteral("login-wrong-id") << int(StatusError::InvalidResponse);
        QTest::newRow("login-invalid-pending") << QStringLiteral("login-bad-shape") << int(StatusError::InvalidResponse);
        QTest::newRow("logout-nonempty-result") << QStringLiteral("logout-bad-shape") << int(StatusError::InvalidResponse);
        QTest::newRow("login-helper-rejects") << QStringLiteral("login-rejected") << int(StatusError::HelperRejected);
        QTest::newRow("logout-pipe-exit") << QStringLiteral("logout-exit") << int(StatusError::Transport);
        QTest::newRow("login-reply-over-limit") << QStringLiteral("login-oversize") << int(StatusError::ResponseTooLarge);
        QTest::newRow("logout-timeout-with-partial-frame") << QStringLiteral("logout-silent") << int(StatusError::Timeout);
    }

    void failedActionRequests()
    {
        QFETCH(QString, scenario);
        QFETCH(int, expected);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EmbeddedStatusClient client(fixtureExecutable(), temp.filePath(scenario));
        QSignalSpy statuses(&client, &EmbeddedStatusClient::statusFinished);
        QSignalSpy logins(&client, &EmbeddedStatusClient::loginFinished);
        QSignalSpy logouts(&client, &EmbeddedStatusClient::logoutFinished);
        const bool login = scenario.startsWith("login-");
        QElapsedTimer clock;
        clock.start();
        QVERIFY(login ? client.requestLogin(251) : client.requestLogout(251));
        const auto &completions = login ? logins : logouts;
        QTRY_COMPARE_WITH_TIMEOUT(completions.size(), 1, 15500);
        QCOMPARE(completions.first().first().toULongLong(), quint64(251));
        const auto payload = completions.first().at(1);
        if (login) {
            const auto result = payload.value<LoginResult>();
            QCOMPARE(int(result.error), expected);
            QVERIFY(!result.pending);
        } else {
            QCOMPARE(int(payload.value<StatusError>()), expected);
        }
        if (scenario == "logout-silent")
            QVERIFY(clock.elapsed() >= 11900);

        QVERIFY(statuses.isEmpty());
        bool accepted = false;
        QTRY_VERIFY_WITH_TIMEOUT(accepted || (accepted = client.requestStatus(252)), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 3000);
        QCOMPARE(statuses.first().first().toULongLong(), quint64(252));
        const auto status = result(statuses);
        QCOMPARE(status.error, StatusError::None);
        QVERIFY(status.status);
        QCOMPARE(status.status->state, expected == int(StatusError::HelperRejected) ?
            BackendState::NeedsLogin : BackendState::Starting);
        QCOMPARE(logins.size(), login ? 1 : 0);
        QCOMPARE(logouts.size(), login ? 0 : 1);
    }

    void windowsProviderFallback()
    {
        QCOMPARE(selectWindowsProvider({}, true, true), ProviderMode::System);
        QCOMPARE(selectWindowsProvider({}, false, true), ProviderMode::Embedded);
        QCOMPARE(selectWindowsProvider({}, true, false), ProviderMode::System);
        QCOMPARE(selectWindowsProvider({}, false, false), ProviderMode::System);
        QCOMPARE(selectWindowsProvider(" \tEmbedded\r\n", true, true), ProviderMode::Embedded);
        QCOMPARE(selectWindowsProvider("System", true, true), ProviderMode::System);
        QCOMPARE(selectWindowsProvider("System", false, true), ProviderMode::Embedded);
        QCOMPARE(selectWindowsProvider("Embedded", true, false), ProviderMode::System);
        QCOMPARE(selectWindowsProvider("embedded", true, true), ProviderMode::System);
        QCOMPARE(selectWindowsProvider(QChar(0xfeff) + QStringLiteral("Embedded\n"), true, true), ProviderMode::Embedded);
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
        QSignalSpy absentLogin(&absent, &EmbeddedStatusClient::loginFinished);
        QVERIFY(absent.requestLogin(3));
        QTRY_COMPARE_WITH_TIMEOUT(absentLogin.size(), 1, 1000);
        QCOMPARE(absentLogin.first().first().toULongLong(), quint64(3));
        const auto login = absentLogin.first().at(1).value<LoginResult>();
        QCOMPARE(login.error, StatusError::Unavailable);
        QVERIFY(!login.pending);
        QCOMPARE(missing.size(), 1);

        EmbeddedStatusClient invalid(fixtureExecutable(), "relative-state");
        QSignalSpy refused(&invalid, &EmbeddedStatusClient::statusFinished);
        QVERIFY(invalid.requestStatus(2));
        QTRY_COMPARE_WITH_TIMEOUT(refused.size(), 1, 1000);
        QCOMPARE(result(refused).error, StatusError::InvalidStateDirectory);
        QSignalSpy refusedLogout(&invalid, &EmbeddedStatusClient::logoutFinished);
        QVERIFY(invalid.requestLogout(4));
        QTRY_COMPARE_WITH_TIMEOUT(refusedLogout.size(), 1, 1000);
        QCOMPARE(refusedLogout.first().first().toULongLong(), quint64(4));
        QCOMPARE(refusedLogout.first().at(1).value<StatusError>(), StatusError::InvalidStateDirectory);
        QCOMPARE(refused.size(), 1);
    }
};

QTEST_GUILESS_MAIN(StatusTests)
#include "StatusTests.moc"
