// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedPairingHost.h"
#include "LifecycleController.h"
#include "PairingWire.h"
#include "../../../sunshine/lifecycle/tests/LoopbackFixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdlib>
#include <utility>

namespace Aurora::Sunshine {

struct BundledAdminFixture
{
    static auto open(const QString &profile, const QByteArray &frame, quint16 port, QString &error)
    {
        return BundledAdmin::openAtPort(profile, frame, port, error);
    }
};

} // namespace Aurora::Sunshine

using namespace Aurora::Pairing;
using namespace aurora::tailscale;

namespace {

using Phase = EmbeddedPairingHost::Phase;
using Admin = HostSunshine::Admin;
using Aurora::Sunshine::AdminIdentity;
using Aurora::Sunshine::LifecycleController;

constexpr quint16 livePort = 51007;
constexpr quint64 initialGeneration = 7;
constexpr quint64 refreshedGeneration = 8;

void require(bool ok, const QString &message)
{
    if (!ok) {
        QTextStream(stderr) << message << '\n';
        std::exit(1);
    }
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Check>
void await(Check check, const QString &message, int timeoutMs = 2700)
{
    QElapsedTimer timer;
    timer.start();
    while (!check()) {
        require(timer.elapsed() < timeoutMs, message);
        spin(5);
    }
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

void touch(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), QStringLiteral("Could not write a synthetic fixture marker."));
}

struct FakeSunshine
{
    const QString session = QStringLiteral("verified-fixture-session");
    const AdminIdentity identity{48990, QByteArray(32, 'c')};
    QList<Aurora::Sunshine::PendingPairing> rows;
    int withRows = 0;
    int approvals = 0;
    bool changeOnConfirmation = false;
    bool changePortOnConfirmation = false;
    std::function<void()> beforeFirstRow;
    Admin::ApprovalCompletion completion;
    Admin::ApprovalStillCurrent stillCurrent;

    HostSunshine api()
    {
        return {
            [this](const QString &expected) -> std::optional<AdminIdentity> {
                return expected == session ? std::optional<AdminIdentity>(identity) : std::nullopt;
            },
            [this](const QString &expected, Admin::PendingCompletion done) {
                require(expected == session, QStringLiteral("Wrong pinned pending session."));
                auto candidate = rows;
                if (!rows.isEmpty()) {
                    ++withRows;
                    if (withRows == 1 && beforeFirstRow)
                        beforeFirstRow();
                    if (withRows >= 2 && changeOnConfirmation)
                        candidate.first().id = QStringLiteral("00000000000000000000000000000002");
                    if (withRows >= 2 && changePortOnConfirmation)
                        candidate.first().sourcePort = 51008;
                }
                done({Admin::Error::None, 200, {}, candidate});
                return true;
            },
            [this](const QString &expected, HostSunshine::Approval, Admin::ApprovalCompletion done,
                   Admin::ApprovalStillCurrent guard) {
                require(expected == session && guard && guard() && !completion,
                        QStringLiteral("Approval did not carry its bound session and live scope."));
                ++approvals;
                completion = std::move(done);
                stillCurrent = std::move(guard);
                return true;
            }
        };
    }

    void settle()
    {
        auto done = std::exchange(completion, {});
        require(bool(done), QStringLiteral("No synthetic approval to finish."));
        const bool permitted = std::exchange(stillCurrent, {})();
        done({permitted ? Admin::Error::None : Admin::Error::Cancelled, permitted ? 200 : 0, {}});
    }
};

struct Fixture
{
    QTemporaryDir directory{QDir::current().filePath(QStringLiteral("pair-host-test-XXXXXX"))};
    QDir dir{directory.path()};
    EmbeddedStatusClient helper;
    std::optional<EmbeddedHostContext> context;
    quint64 proofGeneration = 0;
    FakeSunshine sunshine;
    EmbeddedPairingHost host;

    explicit Fixture(const QString &executable)
        : helper(executable, directory.path()),
          host(helper, sunshine.api(), [this] { return context; })
    {
        verifyAccount(sunshine.session, sunshine.identity);
    }

    Fixture(const QString &executable, LifecycleController &controller)
        : helper(executable, directory.path()), host(helper, controller, [this] { return context; })
    {
        const auto state = controller.state();
        verifyAccount(state.settingsSession, state.adminIdentity);
    }

    void verifyAccount(const QString &session, const AdminIdentity &identity)
    {
        require(directory.isValid(), QStringLiteral("Temporary fake helper directory unavailable."));
        QObject::connect(&helper, &EmbeddedStatusClient::statusFinished, &helper,
            [this, session, identity](quint64 generation, const StatusResult &result) {
                if (result.error != StatusError::None || !result.status || !result.status->account)
                    return;
                if (!context) {
                    const auto account = *result.status->account;
                    context = EmbeddedHostContext{EnrollmentResult::Approved,
                        {Provider::Embedded, account.ownerId, account.nodeId, identity.port, identity.certificateSha256},
                        account, result.status->localAddress, 1, generation, session};
                } else if (generation > context->helperGeneration) {
                    context->helperGeneration = generation;
                    host.contextChanged();
                }
            });
        QObject::connect(&helper, &EmbeddedStatusClient::pairSourceFinished, &helper,
            [this](quint64 generation, quint64, const PairSourceResult &) { proofGeneration = generation; });
        require(helper.requestStatus(initialGeneration), QStringLiteral("Synthetic helper status was not accepted."));
        await([&] { return context.has_value(); }, QStringLiteral("Synthetic helper never verified its account."));
    }

    void start()
    {
        require(host.start(), QStringLiteral("Enrolled fixture host start was refused."));
        await([&] { return host.phase() == Phase::Listening; }, QStringLiteral("Pairing host never acquired a lease."));
    }

    void hello(QTcpSocket &socket, bool valid = true)
    {
        const auto bridge = QJsonDocument::fromJson(read(dir.filePath("bridge.json"))).object();
        const auto key = QByteArray::fromBase64(bridge.value("key").toString().toLatin1());
        const auto port = bridge.value("port").toInt();
        require(key.size() == BridgeKeySize && port > 0 && port <= 65535,
                QStringLiteral("Synthetic helper did not receive an ephemeral bridge."));
        socket.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        socket.connectToHost(QHostAddress::LocalHost, quint16(port));
        await([&] { return socket.bytesAvailable() >= BridgeKeySize; }, QStringLiteral("No host wire challenge."));
        const auto challenge = socket.read(BridgeKeySize);
        // Address asserted by the fake embedded helper, not an actual Tailscale interface.
        const QByteArray payload = QByteArray::fromHex("04644c01147f000001");
        auto proof = QMessageAuthenticationCode::hash(QByteArrayLiteral("AURORA/PAIR/HOST/1") + challenge + payload,
                                                       key, QCryptographicHash::Sha256);
        if (!valid)
            proof[0] = char(proof.at(0) ^ 1);
        const auto message = payload + proof + requestFrame();
        socket.write(message.left(5));
        socket.flush();
        spin(15);
        socket.write(message.mid(5));
        socket.flush();
    }

    Offer offer(QTcpSocket &socket)
    {
        await([&] { return socket.bytesAvailable() >= 37; }, QStringLiteral("No valid host pairing offer."));
        const auto result = parseOffer(socket.read(37));
        require(result.state == Offer::State::Ready, QStringLiteral("Malformed host pairing offer."));
        return result;
    }

    void pending(const Offer &offer, quint16 port = livePort)
    {
        const auto source = QHostAddress(QHostAddress::LocalHost);
        sunshine.rows = {{QStringLiteral("00000000000000000000000000000001"),
            QStringLiteral("aurora-") + QString::fromLatin1(offer.marker), source.toString(), source, port}};
    }

    QByteArray proofs() const { return read(dir.filePath("proofs")); }
};

void testApproved(const QString &executable)
{
    Fixture test(executable);
    test.context->enrollment = EnrollmentResult::NeedsConfirmation;
    require(!test.host.start(), QStringLiteral("Host accepted an unapproved enrollment."));
    test.context->enrollment = EnrollmentResult::Approved;
    test.context->scope.localNode = QStringLiteral("another-node");
    require(!test.host.start(), QStringLiteral("Host accepted approval for a different local node."));
    test.context->scope.localNode = QStringLiteral("self");
    test.start();

    QTcpSocket invalid;
    test.hello(invalid, false);
    await([&] { return invalid.state() == QAbstractSocket::UnconnectedState; },
          QStringLiteral("Invalid bridge proof was not disconnected."));
    require(!invalid.bytesAvailable(), QStringLiteral("Invalid proof got an offer."));

    QTcpSocket client;
    test.hello(client);
    const auto offer = test.offer(client);
    const auto originalBridge = read(test.dir.filePath("bridge.json"));
    await([&] { return test.helper.requestStatus(refreshedGeneration); },
          QStringLiteral("Could not request same-account refresh."));
    await([&] { return test.context->helperGeneration == refreshedGeneration; },
          QStringLiteral("Same-account status refresh did not complete."));
    require(test.host.phase() == Phase::Listening && read(test.dir.filePath("bridge.json")) == originalBridge,
            QStringLiteral("Status refresh tore down the same-account host lease."));

    QTcpSocket extra;
    test.hello(extra);
    await([&] { return extra.bytesAvailable() > 0; }, QStringLiteral("Concurrent pairing was not rejected."));
    require(parseOffer(extra.readAll()).state == Offer::State::Rejected,
            QStringLiteral("Concurrent pairing bypassed serialization."));

    test.pending(offer);
    await([&] { return test.sunshine.approvals == 1; }, QStringLiteral("Exact source proof did not authorize approval."));
    require(test.sunshine.withRows == 2 && test.proofs() == "51007\n" &&
            test.proofGeneration == refreshedGeneration && test.sunshine.stillCurrent(),
            QStringLiteral("Approval missed the confirmed pending row, exact source port, or refreshed RPC generation."));
    test.sunshine.settle();
    await([&] { return client.state() == QAbstractSocket::UnconnectedState; }, QStringLiteral("Completed pairing socket stayed open."));

    ++test.context->accountEpoch;
    test.host.contextChanged();
    require(test.host.phase() == Phase::Stopped, QStringLiteral("A new account epoch retained the old listener."));
    await([&] { return read(test.dir.filePath("closes")) == "ok\n"; },
          QStringLiteral("The owned helper lease was not closed."));
}

void testDenied(const QString &executable, const QString &reason)
{
    Fixture test(executable);
    test.start();
    QTcpSocket client;
    test.hello(client);
    const auto offer = test.offer(client);
    test.pending(offer, reason == "port" ? 51008 : livePort);
    if (reason == "changed") {
        test.sunshine.changeOnConfirmation = true;
    } else if (reason == "changed-port") {
        test.sunshine.changePortOnConfirmation = true;
    } else if (reason == "peer") {
        test.sunshine.beforeFirstRow = [&] { touch(test.dir.filePath("swap-peer")); };
    } else if (reason == "duplicate" || reason == "duplicate-id") {
        auto duplicate = test.sunshine.rows.first();
        if (reason == "duplicate")
            duplicate.sourcePort.reset();
        else
            duplicate.name = QStringLiteral("Unrelated row sharing the ID");
        test.sunshine.rows.append(duplicate);
        await([&] { return test.sunshine.withRows >= 2; }, QStringLiteral("Fixture did not poll the ambiguous rows."));
        require(test.sunshine.approvals == 0 && test.proofs().isEmpty(), QStringLiteral("Ambiguous row reached live proof."));
        client.abort();
        test.host.stop();
        return;
    } else if (reason == "address" || reason == "marker") {
        if (reason == "address")
            test.sunshine.rows.first().sourceIpv4 = QHostAddress(QStringLiteral("127.0.0.2"));
        else
            test.sunshine.rows.first().name += QLatin1Char('0');
        await([&] { return test.sunshine.withRows >= 2; }, QStringLiteral("Fixture did not poll the unrelated row."));
        require(test.sunshine.approvals == 0 && test.proofs().isEmpty(), QStringLiteral("An unrelated row reached live proof."));
        client.abort();
        test.host.stop();
        return;
    }

    await([&] { return client.state() == QAbstractSocket::UnconnectedState; }, QStringLiteral("Denied pairing socket stayed open."));
    require(test.sunshine.approvals == 0 &&
            test.proofs() == (reason == "port" ? QByteArray("51008\n") : QByteArray{}),
            QStringLiteral("A changed row or unverified source reached approval."));
}

void testDisconnect(const QString &executable)
{
    Fixture test(executable);
    test.start();
    QTcpSocket client;
    test.hello(client);
    test.pending(test.offer(client));
    await([&] { return test.sunshine.approvals == 1; }, QStringLiteral("Could not hold a synthetic approval."));
    client.abort();
    await([&] { return !test.sunshine.stillCurrent(); }, QStringLiteral("Disconnect did not revoke approval."));
    QTcpSocket concurrent;
    test.hello(concurrent);
    await([&] { return concurrent.bytesAvailable() > 0; }, QStringLiteral("Second connection did not receive a decision."));
    require(parseOffer(concurrent.readAll()).state == Offer::State::Rejected,
            QStringLiteral("A cancelled approval in flight lost the serialization fence."));
    test.sunshine.settle();
}

class FakeProcess final : public Aurora::Sunshine::HostProcess
{
public:
    bool start(const Aurora::Sunshine::HostLaunch &, QString &) override
    {
        alive_ = true;
        return true;
    }
    bool writePasswordFrame(const QByteArray &, QString &) override { return true; }
    void closeInput() override {}
    bool signalStopEvent() override { return true; }
    bool running() override { return alive_; }
    void waitForExit(int) override { alive_ = false; }
    void forceStop() override { alive_ = false; }
    std::optional<int> exitCode() const override { return 0; }

private:
    bool alive_ = false;
};

class PinnedAdmin final : public Aurora::Sunshine::ReadinessAdmin
{
public:
    explicit PinnedAdmin(std::unique_ptr<Admin> admin) : admin_(std::move(admin)) {}
    AdminIdentity identity() const override { return admin_->identity(); }
    bool readConfig(Admin::Completion done) override { return admin_->readConfig(std::move(done)); }
    bool readPendingPairings(Admin::PendingCompletion done) override
    {
        return admin_->readPendingPairings(std::move(done));
    }
    bool approvePairing(HostSunshine::Approval approval, Admin::ApprovalCompletion done,
                        Admin::ApprovalStillCurrent guard) override
    {
        return admin_->approvePairing(std::move(approval), std::move(done), std::move(guard));
    }

private:
    std::unique_ptr<Admin> admin_;
};

void testPinnedSunshine(const QString &executable, const QDir &identities)
{
    using namespace Aurora::Sunshine;
    const auto cert = identities.filePath(QStringLiteral("pinned-cert.pem"));
    LoopbackFixture server(cert, identities.filePath(QStringLiteral("pinned-key.pem")));
    const QTemporaryDir root(QDir::current().filePath(QStringLiteral("pair-host-admin-test-XXXXXX")));
    require(server.isListening() && root.isValid(), QStringLiteral("Could not start synthetic Sunshine TLS."));
    const QDir dir(root.path());
    const auto layout = HostLayout::forDirectories(HostPlatform::Windows, dir.filePath("app"), dir.filePath("data"));
    require(layout.has_value(), QStringLiteral("Could not prepare synthetic Sunshine layout."));
    const QDir payload(layout->payloadDirectory());
    for (const auto &path : {layout->executable(), payload.filePath("zlib1.dll"),
                            payload.filePath("assets/apps.json"), payload.filePath("assets/web/index.html")}) {
        require(QDir().mkpath(QFileInfo(path).absolutePath()), QStringLiteral("Could not prepare synthetic payload."));
        touch(path);
    }
    require(QDir().mkpath(QFileInfo(layout->certificateFile()).absolutePath()) && QFile::copy(cert, layout->certificateFile()),
            QStringLiteral("Could not prepare synthetic Sunshine TLS."));

    LifecycleDependencies deps{
        [] { return std::make_unique<FakeProcess>(); },
        [](HostPlatform, const PortFamily &) { return PortCheck{}; },
        [port = server.serverPort()](const QString &profile, const QByteArray &frame,
                                     QString &error) -> std::unique_ptr<ReadinessAdmin> {
            auto admin = BundledAdminFixture::open(profile, frame, port, error);
            return admin ? std::make_unique<PinnedAdmin>(std::move(admin)) : nullptr;
        }
    };
    LifecycleController controller(*layout, std::move(deps), {2500, 10, 30000});
    require(controller.start(), QStringLiteral("Could not start synthetic Sunshine lifecycle."));
    await([&] { return controller.state().phase == LifecycleController::Phase::Ready; },
          QStringLiteral("Synthetic Sunshine never became ready."));

    Fixture test(executable, controller);
    const auto approved = test.context;
    test.context->scope.certificateSha256 = QByteArray(32, 'x');
    require(!test.host.start(), QStringLiteral("Enrollment for a different certificate was accepted."));
    test.context = approved;
    ++test.context->scope.adminPort;
    require(!test.host.start(), QStringLiteral("Enrollment for a different admin port was accepted."));
    test.context = approved;
    test.context->sunshineSession = QStringLiteral("stale-launch");
    require(!test.host.start(), QStringLiteral("An old Sunshine launch session was accepted."));
    test.context = approved;
    test.start();

    const auto selected = QStringLiteral("00000000000000000000000000000001");
    const auto matchingRow = [&selected](const Offer &offer) {
        return QJsonObject{{"id", selected}, {"name", QStringLiteral("aurora-") + QString::fromLatin1(offer.marker)},
                           {"address", "127.0.0.1"}, {"sourcePort", livePort}};
    };
    QTcpSocket client;
    test.hello(client);
    const auto offer = test.offer(client);
    server.pairings.append(matchingRow(offer));
    await([&] { return server.approvals.size() == 1; }, QStringLiteral("Pinned pairing approval never reached the fixture."));
    const auto actual = server.approvals.first();
    require(actual.size() == 3 && actual.value("pairing_id") == selected &&
            actual.value("pin") == QString::fromLatin1(offer.pin) && test.proofs() == "51007\n",
            QStringLiteral("Pinned approval did not submit the correlated ID and the exact offer PIN."));
    await([&] { return client.state() == QAbstractSocket::UnconnectedState; }, QStringLiteral("Pinned approval did not finish."));

    QObject::connect(&test.helper, &EmbeddedStatusClient::pairSourceFinished, &server,
        [&server](quint64, quint64, const PairSourceResult &proof) { server.holdNextHandshake = proof.verified; });
    server.pairings = {};
    QTcpSocket revoked;
    test.hello(revoked);
    server.pairings.append(matchingRow(test.offer(revoked)));
    await([&] { return server.handshakeHeld; }, QStringLiteral("Approval never entered the worker's TLS handshake."));
    const auto priorBytes = server.receivedBytes;
    ++test.context->accountEpoch;
    test.host.contextChanged();
    require(test.host.phase() == Phase::Stopped, QStringLiteral("Revocation kept the listener active."));
    server.releaseHandshake();
    await([&] { return !controller.adminBusy(); }, QStringLiteral("Revoked worker approval did not complete."));
    require(server.receivedBytes == priorBytes && server.approvals.size() == 1 && test.proofs() == "51007\n51007\n",
            QStringLiteral("An account revoked during TLS sent approval bytes."));

    await([&] { return test.host.start(); }, QStringLiteral("Cancelled approval did not release the host."));
    await([&] { return test.host.phase() == Phase::Listening; }, QStringLiteral("Replacement lease did not open."));
    require(controller.stop() && test.host.phase() == Phase::Stopped,
            QStringLiteral("Sunshine launch revocation did not shut down the pairing host immediately."));
    await([&] { return controller.state().phase == LifecycleController::Phase::Stopped; },
          QStringLiteral("Synthetic Sunshine failed to stop."));
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    require(app.arguments().size() == 3 && QFileInfo(app.arguments().at(1)).isExecutable(),
            QStringLiteral("Expected the offline helper executable."));
    const auto helper = app.arguments().at(1);
    testApproved(helper);
    for (const auto &reason : {"port", "changed", "changed-port", "peer", "duplicate", "duplicate-id", "address", "marker"})
        testDenied(helper, QString::fromLatin1(reason));
    testDisconnect(helper);
    testPinnedSunshine(helper, QDir(app.arguments().at(2)));
    QTextStream(stdout) << "PASS: authenticated loopback host, refresh-stable lease, exact pending and live source proof, "
                          "serial approvals and cancellation\n";
}
