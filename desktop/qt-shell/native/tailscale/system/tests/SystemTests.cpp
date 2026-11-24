// SPDX-License-Identifier: GPL-3.0-only
#include "SystemTailscaleClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace aurora::tailscale;
using namespace aurora::tailscale::system;

namespace {

QString fakeCli(const QTemporaryDir &directory, const QString &scenario)
{
    const auto executable = QDir(QCoreApplication::applicationDirPath()).filePath("AuroraSystemTailnetFixture.exe");
    if (!QDir(directory.path()).mkpath("Synthetic install"))
        qFatal("Could not make the offline CLI directory.");
    const auto target = directory.filePath("Synthetic install/" + scenario + ".exe");
    if (!QFile::copy(executable, target))
        qFatal("Could not stage the offline CLI fixture.");
    return target;
}

} // namespace

class SystemTests final : public QObject
{
    Q_OBJECT

private slots:
    void imagePath_data()
    {
        QTest::addColumn<QString>("image");
        QTest::addColumn<QString>("expected");
        QTest::newRow("unquoted-with-spaces") << QString(" C:\\Program Files\\Tailscale\\tailscaled.exe --verbose=1 ")
            << QString("C:\\Program Files\\Tailscale\\tailscale.exe");
        QTest::newRow("quoted-case-and-tail")
            << QString("\"D:\\Managed Apps\\Tailscale\\TAILSCALED.EXE\" --state=\"something else\"")
            << QString("D:\\Managed Apps\\Tailscale\\tailscale.exe");
        QTest::newRow("quoted-mixed-separators") << QString("\"D:/Apps//Tailscale\\tailscaled.exe\"")
            << QString("D:\\Apps\\Tailscale\\tailscale.exe");
        QTest::newRow("quoted-drive-root") << QString("\"D:/tailscaled.exe\"") << QString("D:\\tailscale.exe");
        QTest::newRow("forward-only-unquoted") << QString("D:/Apps/tailscaled.exe") << QString();
        QTest::newRow("unc") << QString("\"\\\\server\\share\\Apps\\tailscaled.exe\" --debug")
            << QString("\\\\server\\share\\Apps\\tailscale.exe");
        QTest::newRow("unc-share-as-basename") << QString("\"\\\\server\\tailscaled.exe\"") << QString();
        QTest::newRow("extended-drive") << QString("\"\\\\?\\D:\\Apps\\tailscaled.exe\"")
            << QString("\\\\?\\D:\\Apps\\tailscale.exe");
        QTest::newRow("extended-unc") << QString("\"\\\\?\\UNC\\server\\share\\tailscaled.exe\"")
            << QString("\\\\?\\UNC\\server\\share\\tailscale.exe");
        QTest::newRow("drive-relative") << QString("D:Apps\\tailscaled.exe") << QString();
        QTest::newRow("current-drive") << QString("\\Apps\\tailscaled.exe") << QString();
        QTest::newRow("relative") << QString("Apps\\tailscaled.exe") << QString();
        QTest::newRow("invalid-drive") << QString("4:\\Apps\\tailscaled.exe") << QString();
        QTest::newRow("suffix-spoof") << QString("D:\\Apps\\tailscaled.exe.bak") << QString();
        QTest::newRow("suffix-directory") << QString("D:\\tailscaled.exe\\Apps\\tailscaled.exe") << QString();
        QTest::newRow("missing-quote") << QString("\"D:\\Apps\\tailscaled.exe") << QString();
        QTest::newRow("no-argument-separator") << QString("\"D:\\Apps\\tailscaled.exe\"--test") << QString();
        QTest::newRow("wrong-executable") << QString("\"D:\\Apps\\run.exe\" tailscaled.exe") << QString();
        QTest::newRow("old-preview") << QString("\"c:\\PROGRAM FILES\\AuroraTailnet\\old\\tailscaled.exe\"") << QString();
        QTest::newRow("old-preview-separators") << QString("\"C:/Program Files/AuroraTailnet//old/tailscaled.exe\"") << QString();
        QTest::newRow("only-grandparent-matches")
            << QString("C:\\Program Files\\AuroraTailnet\\old\\bin\\tailscaled.exe")
            << QString("C:\\Program Files\\AuroraTailnet\\old\\bin\\tailscale.exe");
        QTest::newRow("different-program-files")
            << QString("D:\\Program Files\\AuroraTailnet\\old\\tailscaled.exe")
            << QString("D:\\Program Files\\AuroraTailnet\\old\\tailscale.exe");
    }

    void imagePath()
    {
        QFETCH(QString, image);
        QFETCH(QString, expected);
        const auto expand = [](const QString &) -> QString {
            qFatal("A REG_SZ fixture requested environment expansion.");
            return {};
        };
        QCOMPARE(cliFromServiceImagePath({image}, "C:\\Program Files", expand), expected);
    }

    void registryExpansion()
    {
        const auto expand = [](const QString &text) {
            auto expanded = text;
            return expanded.replace("%FixtureProgramFiles%", "F:\\Synthetic Program Files");
        };
        QCOMPARE(cliFromServiceImagePath(
            {"\"%FixtureProgramFiles%\\Tailscale\\tailscaled.exe\" --debug", ImagePathType::ExpandString},
            "F:\\Synthetic Program Files", expand), QString("F:\\Synthetic Program Files\\Tailscale\\tailscale.exe"));
        QVERIFY(cliFromServiceImagePath(
            {"\"%FixtureProgramFiles%\\AuroraTailnet\\v0\\tailscaled.exe\"", ImagePathType::ExpandString},
            "F:\\Synthetic Program Files", expand).isEmpty());
        QVERIFY(cliFromServiceImagePath(
            {"\"%FixtureProgramFiles%\\Tailscale\\tailscaled.exe\"", ImagePathType::String},
            "F:\\Synthetic Program Files", expand).isEmpty());
        const auto lookalike = QString("D:\\Apps\\tail") + QChar(0x017f) + "caled.exe";
        QVERIFY(cliFromServiceImagePath({lookalike}, "C:\\Program Files", expand).isEmpty());
        QVERIFY(cliFromServiceImagePath({'"' + lookalike + '"'}, "C:\\Program Files", expand).isEmpty());
        auto embeddedNull = QString("C:\\Program Files\\");
        embeddedNull.insert(4, QChar(0));
        QVERIFY(cliFromServiceImagePath({embeddedNull + "tailscaled.exe"}, "C:\\Program Files", expand).isEmpty());
    }

    void rawStatusValidation()
    {
        const auto login = decodeSystemStatus("{\"BackendState\":\"NeedsLogin\",\"AuthURL\":\"https://login.tailscale.com/a/fixture\"}");
        QVERIFY(login.status);
        QCOMPARE(login.status->loginUrl, QUrl("https://login.tailscale.com/a/fixture"));
        const auto hostile = decodeSystemStatus("{\"BackendState\":\"NeedsLogin\",\"AuthURL\":\"https://login.tailscale.com.invalid/a/fixture\"}");
        QVERIFY(hostile.status && hostile.status->loginUrl.isEmpty());
        const auto oldClient = decodeSystemStatus("{\"BackendState\":\"Running\",\"Self\":{\"UserID\":1234,\"ID\":\"self\"},\n"
            "            \"CurrentTailnet\":{\"Name\":\"fixture.example.invalid\"},\"TailscaleIPs\":[\"100.76.1.1\"]}");
        QVERIFY(oldClient.status && !oldClient.status->account);
        QCOMPARE(oldClient.status->state, BackendState::Connected);
        QCOMPARE(decodeSystemStatus("[]").error, StatusError::InvalidResponse);
        QCOMPARE(decodeSystemStatus("null,\"result\":{\"BackendState\":\"Running\"}").error, StatusError::InvalidResponse);
        QCOMPARE(decodeSystemStatus("{\"BackendState\":true}").error, StatusError::InvalidResponse);
        QCOMPARE(decodeSystemStatus(QByteArray(maxSystemStatusBytes + 1, ' ')).error, StatusError::ResponseTooLarge);
    }

    void profileValidation()
    {
        const auto json = decodeProfileList("[{\"id\":\"abc\",\"account\":\"one@example.invalid\",\"selected\":true},\n"
            "            {\"id\":\"abc\",\"account\":\"duplicate@example.invalid\"},{\"id\":\"--bad\",\"account\":\"two@example.invalid\"},\n"
            "            {\"id\":\"\\u00e1bc\",\"account\":\"three@example.invalid\"},{\"id\":\"Abc\",\"account\":\"four@example.invalid\",\"selected\":\"true\"},\n"
            "            {\"id\":\"empty\",\"account\":\" \"},{\"id\":5,\"account\":\"five@example.invalid\"}]");
        QCOMPARE(json.error, StatusError::None);
        QCOMPARE(json.profiles.size(), 2);
        QCOMPARE(json.profiles.first().id, QStringLiteral("abc"));
        QVERIFY(json.profiles.first().isCurrent);
        QCOMPARE(json.profiles.last().id, QStringLiteral("Abc"));
        QVERIFY(!json.profiles.last().isCurrent);

        const auto legacy = decodeLegacyProfileList("ID    Tailnet         Account\r\n"
            "home  home.test       Fixture personal device*\r"
            "work  work.test       work@example.invalid\n"
            "work  dup.test        duplicate@example.invalid\n");
        QCOMPARE(legacy.error, StatusError::None);
        QCOMPARE(legacy.profiles.size(), 2);
        QCOMPARE(legacy.profiles.first().loginName, QStringLiteral("Fixture personal device"));
        QVERIFY(legacy.profiles.first().isCurrent);
        QCOMPARE(legacy.profiles.last().tailnet, QStringLiteral("work.test"));
        QCOMPARE(decodeLegacyProfileList("bad header").error, StatusError::InvalidResponse);

        QVERIFY(cliLacksJsonProfileList("flag provided but not defined: -json", " \n"));
        QVERIFY(cliLacksJsonProfileList("", "flag provided but not defined: -json"));
        QVERIFY(!cliLacksJsonProfileList("flag provided but not defined: -json", "other error"));
        QVERIFY(!cliLacksJsonProfileList("", "flag provided but not defined: -list"));
    }

    void mockedServiceStates()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto executable = fakeCli(fixture, "status");
        auto service = ServiceState::Stopped;
        SystemTailscaleClient client([&] { return Installation{executable, service}; });
        QSignalSpy statuses(&client, &SystemTailscaleClient::statusFinished);
        QSignalSpy profiles(&client, &SystemTailscaleClient::profilesFinished);

        QVERIFY(client.requestStatus(17));
        QVERIFY(!client.requestProfiles(18));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 1000);
        const auto stopped = statuses.first().at(1).value<StatusResult>();
        QVERIFY(stopped.status);
        QCOMPARE(stopped.status->state, BackendState::Stopped);
        QCOMPARE(statuses.first().first().toULongLong(), quint64(17));

        service = ServiceState::Starting;
        QVERIFY(client.requestStatus(19));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 2, 1000);
        const auto starting = statuses.last().at(1).value<StatusResult>();
        QVERIFY(starting.status);
        QCOMPARE(starting.status->state, BackendState::Starting);

        QVERIFY(client.requestProfiles(20));
        QTRY_COMPARE_WITH_TIMEOUT(profiles.size(), 1, 1000);
        const auto empty = profiles.first().at(1).value<ProfilesResult>();
        QCOMPARE(empty.error, StatusError::None);
        QVERIFY(empty.profiles.isEmpty());
        QCOMPARE(profiles.first().first().toULongLong(), quint64(20));

        service = ServiceState::Missing;
        QVERIFY(client.requestStatus(21));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 3, 1000);
        QCOMPARE(statuses.last().at(1).value<StatusResult>().error, StatusError::Unavailable);
    }

    void typedStatusCommand()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto executable = fakeCli(fixture, "status");
        SystemTailscaleClient client([=] { return Installation{executable, ServiceState::Running}; });
        QSignalSpy statuses(&client, &SystemTailscaleClient::statusFinished);
        QVERIFY(client.requestStatus(0xfedcba9876543210ULL));
        QVERIFY(statuses.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 2500);
        QCOMPARE(statuses.first().first().toULongLong(), quint64(0xfedcba9876543210ULL));
        const auto value = statuses.first().at(1).value<StatusResult>();
        QCOMPARE(value.error, StatusError::None);
        QVERIFY(value.status && value.status->account);
        QCOMPARE(value.status->account->ownerId, qint64(9223372036854775807LL));
        QCOMPARE(value.status->account->nodeId, QStringLiteral("system-local"));
        QCOMPARE(value.status->displayName, QStringLiteral("Fixture owner"));
        QCOMPARE(value.status->peers.size(), 1);
        QCOMPARE(value.status->peers.first().nodeId, QStringLiteral("same-owner"));
    }

    void cliProfileReads_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("error");
        QTest::addColumn<int>("count");
        QTest::newRow("modern") << QStringLiteral("profiles") << int(StatusError::None) << 2;
        QTest::newRow("legacy") << QStringLiteral("legacy") << int(StatusError::None) << 2;
        QTest::newRow("bad-json-no-fallback") << QStringLiteral("bad-profiles") << int(StatusError::InvalidResponse) << 0;
        QTest::newRow("denied-no-fallback") << QStringLiteral("profile-denied") << int(StatusError::HelperRejected) << 0;
        QTest::newRow("stderr-limit") << QStringLiteral("large-stderr") << int(StatusError::ResponseTooLarge) << 0;
        QTest::newRow("combined-time-budget") << QStringLiteral("legacy-deadline") << int(StatusError::Timeout) << 0;
    }

    void cliProfileReads()
    {
        QFETCH(QString, scenario);
        QFETCH(int, error);
        QFETCH(int, count);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto executable = fakeCli(fixture, scenario);
        auto inspected = 0;
        SystemTailscaleClient client([&] { ++inspected; return Installation{executable, ServiceState::Running}; });
        QSignalSpy profiles(&client, &SystemTailscaleClient::profilesFinished);
        QSignalSpy statuses(&client, &SystemTailscaleClient::statusFinished);
        QElapsedTimer elapsed;
        elapsed.start();

        QVERIFY(client.requestProfiles(88));
        QTRY_COMPARE_WITH_TIMEOUT(profiles.size(), 1, 11000);
        QCOMPARE(profiles.first().first().toULongLong(), quint64(88));
        QCOMPARE(inspected, 1);
        QVERIFY(statuses.isEmpty());
        const auto value = profiles.first().at(1).value<ProfilesResult>();
        QCOMPARE(int(value.error), error);
        QCOMPARE(value.profiles.size(), count);
        if (count) {
            QCOMPARE(value.profiles.first().id, QStringLiteral("home"));
            QVERIFY(value.profiles.first().isCurrent);
            QCOMPARE(value.profiles.last().id, QStringLiteral("work"));
        }
        if (scenario == "legacy-deadline")
            QVERIFY(elapsed.elapsed() >= 7900 && elapsed.elapsed() < 9400);
    }

    void oversizedStatus()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto executable = fakeCli(fixture, "large-status");
        SystemTailscaleClient client([=] { return Installation{executable, ServiceState::Running}; });
        QSignalSpy statuses(&client, &SystemTailscaleClient::statusFinished);
        QVERIFY(client.requestStatus(99));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 4000);
        QCOMPARE(statuses.first().at(1).value<StatusResult>().error, StatusError::ResponseTooLarge);
    }
};

QTEST_GUILESS_MAIN(SystemTests)
#include "SystemTests.moc"
