// SPDX-License-Identifier: GPL-3.0-only
#include "ControlAdapter.h"
#include "EmbeddedStatusClient.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using namespace Aurora::Stream::Control;
using Aurora::Discovery::Endpoint;
using Aurora::Discovery::StreamHost;
using CheckStatus = Aurora::Discovery::ServerInfo::Status;
using aurora::tailscale::EmbeddedStatusClient;
using aurora::tailscale::OpenResult;
using aurora::tailscale::StreamLease;

namespace {

const QString hostId = QStringLiteral("f43a3fe0-27f6-46b3-8c38-758349e23db1");
const QHostAddress fakePeer(QStringLiteral("100.76.1.20"));

Authorization grant(Operation operation)
{
    StreamHost selected{{QHostAddress(QStringLiteral("192.0.2.30")), 48989}, hostId, "Fixture", false, false, true, true};
    auto current = selected;
    current.id = current.id.toUpper();
    return {QUuid::createUuid(), operation, QString::fromUtf8(CONTROL_WORKER), selected,
            {CheckStatus::Complete, current}, {}, {}, [](const QUuid &) { return true; }};
}

Authorization viaLease(Operation operation, const std::shared_ptr<StreamLease> &lease)
{
    auto request = grant(operation);
    request.selected.endpoint.address = fakePeer;
    request.selected.viaEmbeddedTailnet = true;
    request.revalidated.current = request.selected;
    request.checkedRelay = Endpoint{lease->address(), lease->port()};
    request.lease = lease;
    return request;
}

QList<QJsonObject> records(const QString &path)
{
    QList<QJsonObject> result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return result;
    for (const auto &line : file.readAll().split('\n')) {
        const auto item = QJsonDocument::fromJson(line);
        if (item.isObject())
            result.append(item.object());
    }
    return result;
}

std::optional<Result> run(Adapter &adapter, Authorization request)
{
    const auto token = request.token;
    QSignalSpy finished(&adapter, &Adapter::finished);
    if (!adapter.start(std::move(request)) || !finished.wait(3000) || adapter.busy() ||
        finished.size() != 1 || finished.first().at(0).toUuid() != token)
        return std::nullopt;
    return finished.first().at(1).value<Result>();
}

struct FakeTailnet
{
    QTemporaryDir directory;
    EmbeddedStatusClient client{QString::fromUtf8(CONTROL_TAILNET), directory.filePath("rpc-control")};

    std::shared_ptr<StreamLease> open()
    {
        QSignalSpy statuses(&client, &EmbeddedStatusClient::statusFinished);
        QSignalSpy opens(&client, &EmbeddedStatusClient::openFinished);
        if (!directory.isValid() || !client.requestStatus(1) || !statuses.wait(2500) ||
            !client.requestOpen(1, QStringLiteral("peer"), fakePeer) || !opens.wait(2500))
            return {};
        return opens.first().at(2).value<OpenResult>().lease;
    }
};

} // namespace

class ControlTest final : public QObject
{
    Q_OBJECT

    QTemporaryDir directory_;
    const QString log_ = directory_.filePath("worker.jsonl");

private slots:
    void initTestCase()
    {
        if (Aurora::Stream::currentPlatform() == Aurora::Stream::RuntimePlatform::Unsupported)
            QSKIP("The bundled command is available on Windows and macOS.");
        QVERIFY(directory_.isValid());
    }

    void init()
    {
        if (QFile::exists(log_))
            QVERIFY(QFile::remove(log_));
        QVERIFY(qputenv("AURORA_CONTROL_FIXTURE_LOG", QFile::encodeName(log_)));
        qunsetenv("AURORA_CONTROL_FIXTURE_MODE");
        qputenv("AURORA_GAMESTREAM_RELAY_BASE", "9999");
        qputenv("AURORA_GAMESTREAM_HOST_UUID", "ambient");
    }

    void cleanup()
    {
        qunsetenv("AURORA_CONTROL_FIXTURE_LOG");
        qunsetenv("AURORA_CONTROL_FIXTURE_MODE");
        qunsetenv("AURORA_GAMESTREAM_RELAY_BASE");
        qunsetenv("AURORA_GAMESTREAM_HOST_UUID");
    }

    void protocol()
    {
        Adapter adapter;
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "split");
        auto reply = run(adapter, grant(Operation::Restart));
        QVERIFY(reply && reply->status == Status::Accepted && !reply->privacy);
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "stderr");
        reply = run(adapter, grant(Operation::Shutdown));
        QVERIFY(reply && reply->status == Status::Accepted && !reply->privacy);
        qunsetenv("AURORA_CONTROL_FIXTURE_MODE");

        for (const auto item : {std::pair{Operation::PrivacyStatus, PrivacyState::Unavailable},
                                std::pair{Operation::PrivacyOn, PrivacyState::On},
                                std::pair{Operation::PrivacyOff, PrivacyState::Off}}) {
            auto request = grant(item.first);
            request.selected.endpoint.address = QHostAddress(QStringLiteral("2001:db8::5"));
            request.revalidated.current->endpoint = request.selected.endpoint;
            reply = run(adapter, std::move(request));
            QVERIFY(reply && reply->status == Status::PrivacyReported && reply->privacy == item.second);
        }
        const auto launched = records(log_);
        QCOMPARE(launched.size(), 5);
        const QStringList actions{"restart", "shutdown", "status", "on", "off"};
        for (int i = 0; i < actions.size(); ++i) {
            const auto &record = launched.at(i);
            const QJsonArray expected{"--aurora-runtime", i < 2 ? "power" : "privacy",
                                      i < 2 ? "192.0.2.30:48989" : "[2001:db8::5]:48989", hostId, actions.at(i)};
            QCOMPARE(record.value("args").toArray(), expected);
            QCOMPARE(record.value("base").toString(), QString());
            QCOMPARE(record.value("host").toString(), QString());
        }
    }

    void responseFailures()
    {
        Adapter adapter;
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "missing-pairing");
        auto reply = run(adapter, grant(Operation::Shutdown));
        QVERIFY(reply && reply->status == Status::PairingRequired);
        reply = run(adapter, grant(Operation::PrivacyStatus));
        QVERIFY(reply && reply->status == Status::Unconfirmed && !reply->privacy);
        for (const auto *mode : {"extra-line", "nul-suffix", "rejected"}) {
            qputenv("AURORA_CONTROL_FIXTURE_MODE", mode);
            reply = run(adapter, grant(Operation::Restart));
            QVERIFY(reply && reply->status == Status::Unconfirmed);
        }
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "overflow");
        reply = run(adapter, grant(Operation::Restart));
        QVERIFY(reply && reply->status == Status::OutputLimitExceeded);
        auto missing = grant(Operation::Restart);
        missing.executable = directory_.filePath(QFileInfo(missing.executable).fileName());
        reply = run(adapter, std::move(missing));
        QVERIFY(reply && reply->status == Status::StartFailed);
    }

    void rejectsBeforeLaunch()
    {
        Adapter adapter;
        const auto reject = [&adapter](Authorization request, Status expected) {
            const auto reply = run(adapter, std::move(request));
            return reply && reply->status == expected;
        };
        auto request = grant(Operation::Restart);
        request.stillCurrent = {};
        QVERIFY(reject(request, Status::InvalidAuthorization));
        request = grant(Operation::Restart);
        request.stillCurrent = [](const QUuid &) { return false; };
        QVERIFY(reject(request, Status::Cancelled));
        request = grant(static_cast<Operation>(99));
        QVERIFY(reject(request, Status::InvalidAuthorization));
        request = grant(Operation::Restart);
        request.revalidated.status = CheckStatus::AccessChanged;
        QVERIFY(reject(request, Status::RouteChanged));
        request = grant(Operation::Restart);
        request.revalidated.current->id = QStringLiteral("d23d7a1d-4b2f-4c10-8ee2-1906f795e608");
        QVERIFY(reject(request, Status::RouteChanged));
        request = grant(Operation::Restart);
        request.revalidated.current->endpoint.address = QHostAddress(QStringLiteral("192.0.2.31"));
        QVERIFY(reject(request, Status::RouteChanged));
        request = grant(Operation::Restart);
        ++request.revalidated.current->endpoint.port;
        QVERIFY(reject(request, Status::RouteChanged));
        request = grant(Operation::Restart);
        request.revalidated.current->viaEmbeddedTailnet = true;
        QVERIFY(reject(request, Status::RouteChanged));
        request = grant(Operation::Restart);
        request.revalidated.current->supportsPower = false;
        QVERIFY(reject(request, Status::CapabilityUnavailable));
        request = grant(Operation::PrivacyStatus);
        request.revalidated.current->supportsSessionPrivacy = false;
        QVERIFY(reject(request, Status::CapabilityUnavailable));
        request = grant(Operation::PrivacyOff);
        request.executable = directory_.filePath("Moonlight.exe");
        QVERIFY(reject(request, Status::RuntimeUnavailable));
        QVERIFY(records(log_).isEmpty());
    }

    void relay()
    {
        FakeTailnet tailnet;
        auto lease = tailnet.open();
        QVERIFY(lease && lease->isValid());
        Adapter adapter;
        auto request = viaLease(Operation::Restart, lease);
        ++request.checkedRelay->port;
        auto reply = run(adapter, std::move(request));
        QVERIFY(reply && reply->status == Status::RouteChanged && records(log_).isEmpty());

        qputenv("AURORA_CONTROL_FIXTURE_MODE", "probe-bad");
        reply = run(adapter, viaLease(Operation::Restart, lease));
        QVERIFY(reply && reply->status == Status::RelayProbeFailed);
        QCOMPARE(records(log_).size(), 1);
        qunsetenv("AURORA_CONTROL_FIXTURE_MODE");

        reply = run(adapter, viaLease(Operation::Restart, lease));
        QVERIFY(reply && reply->status == Status::Accepted);
        reply = run(adapter, viaLease(Operation::PrivacyOn, lease));
        QVERIFY(reply && reply->status == Status::PrivacyReported && reply->privacy == PrivacyState::On);
        const auto launched = records(log_);
        QCOMPARE(launched.size(), 4);
        const QJsonArray probe{"--aurora-gamestream-relay-info"};
        QCOMPARE(launched.at(1).value("args").toArray(), probe);
        QCOMPARE(launched.at(1).value("base").toString(), QString());
        for (const auto index : {2, 3}) {
            const auto port = QString::number(lease->port());
            QCOMPARE(launched.at(index).value("base").toString(), port);
            QCOMPARE(launched.at(index).value("host").toString(), hostId);
            QCOMPARE(launched.at(index).value("args").toArray().at(2).toString(), "127.0.0.1:" + port);
        }
        QVERIFY(lease->isValid());
    }

    void cancellation()
    {
        Adapter adapter;
        QSignalSpy finished(&adapter, &Adapter::finished);
        auto request = grant(Operation::Restart);
        const auto token = request.token;
        QVERIFY(adapter.start(request));
        QVERIFY(!adapter.start(grant(Operation::Shutdown)));
        QVERIFY(!adapter.cancel(QUuid::createUuid()));
        QVERIFY(adapter.cancel(token));
        QVERIFY(finished.wait(1500));
        QCOMPARE(finished.takeFirst().at(1).value<Result>().status, Status::Cancelled);
        QVERIFY(records(log_).isEmpty());

        qputenv("AURORA_CONTROL_FIXTURE_MODE", "hold");
        bool authorized = true;
        request.stillCurrent = [&](const QUuid &received) { return received == token && authorized; };
        QVERIFY(adapter.start(std::move(request)));
        QTRY_COMPARE_WITH_TIMEOUT(records(log_).size(), 1, 2000);
        authorized = false;
        QVERIFY(finished.wait(1500));
        const auto completed = finished.takeFirst();
        QCOMPARE(completed.at(0).toUuid(), token);
        QCOMPARE(completed.at(1).value<Result>().status, Status::Cancelled);
        QVERIFY(!adapter.busy());
    }

    void relayProbeTimeoutAndRevocation()
    {
        FakeTailnet tailnet;
        auto lease = tailnet.open();
        QVERIFY(lease && lease->isValid());
        Adapter adapter;
        QSignalSpy finished(&adapter, &Adapter::finished);
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "probe-hold");
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(adapter.start(viaLease(Operation::Shutdown, lease)));
        QVERIFY(finished.wait(6000));
        QVERIFY(elapsed.elapsed() >= 4000);
        QCOMPARE(finished.takeFirst().at(1).value<Result>().status, Status::RelayProbeFailed);
        QCOMPARE(records(log_).size(), 1);

        auto request = viaLease(Operation::Restart, lease);
        const auto token = request.token;
        QVERIFY(adapter.start(std::move(request)));
        QTRY_COMPARE_WITH_TIMEOUT(records(log_).size(), 2, 2000);
        QVERIFY(adapter.cancel(token));
        QVERIFY(finished.wait(1500));
        QCOMPARE(finished.takeFirst().at(1).value<Result>().status, Status::Cancelled);
        const QJsonArray probe{"--aurora-gamestream-relay-info"};
        for (const auto &record : records(log_))
            QCOMPARE(record.value("args").toArray(), probe);
        QVERIFY(lease->isValid());
    }

    void retainsAndMonitorsLease()
    {
        FakeTailnet tailnet;
        auto lease = tailnet.open();
        QVERIFY(lease && lease->isValid());
        const std::weak_ptr<StreamLease> retained = lease;
        Adapter adapter;
        QSignalSpy finished(&adapter, &Adapter::finished);
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "hold");
        QVERIFY(adapter.start(viaLease(Operation::PrivacyOff, lease)));
        lease.reset();
        QTRY_COMPARE_WITH_TIMEOUT(records(log_).size(), 1, 2000);
        QVERIFY(!retained.expired() && retained.lock()->isValid());
        QVERIFY(retained.lock()->close());
        QVERIFY(finished.wait(1500));
        QCOMPARE(finished.takeFirst().at(1).value<Result>().status, Status::RouteChanged);
        QVERIFY(retained.expired() && !adapter.busy());
    }

    void deadlines()
    {
        Adapter adapter;
        QSignalSpy finished(&adapter, &Adapter::finished);
        qputenv("AURORA_CONTROL_FIXTURE_MODE", "hold");
        for (const auto item : {std::pair{Operation::PrivacyStatus, 8000}, std::pair{Operation::Shutdown, 12000}}) {
            QElapsedTimer elapsed;
            elapsed.start();
            QVERIFY(adapter.start(grant(item.first)));
            QVERIFY(finished.wait(item.second + 2500));
            QVERIFY(elapsed.elapsed() >= item.second);
            QCOMPARE(finished.takeFirst().at(1).value<Result>().status, Status::TimedOut);
            QVERIFY(!adapter.busy());
        }
    }
};

QTEST_GUILESS_MAIN(ControlTest)
#include "ControlTest.moc"
