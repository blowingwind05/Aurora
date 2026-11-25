// SPDX-License-Identifier: GPL-3.0-only
#include <qt_windows.h>

#include "LifecycleController.h"
#include "PairingWire.h"
#include "SystemPairingCoordinator.h"
#include "WindowsTransport.h"
#include "../../sunshine/lifecycle/tests/LoopbackFixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <utility>

using namespace Aurora::Pairing;
using namespace Aurora::Sunshine;
using aurora::tailscale::system::Installation;
using aurora::tailscale::system::ServiceState;
using Phase = SystemPairingCoordinator::Phase;

#define CHECK(expression) \
    do { if (!(expression)) qFatal("Line %d: %s", __LINE__, #expression); } while (false)

namespace Aurora::Sunshine {

struct BundledAdminFixture
{
    static std::unique_ptr<BundledAdmin> open(const QString &profile, const QByteArray &frame, quint16 port, QString &error)
    {
        return BundledAdmin::openAtPort(profile, frame, port, error);
    }
};

} // namespace Aurora::Sunshine

namespace {

const auto pairingId = QStringLiteral("00000000000000000000000000000001");
const auto otherId = QStringLiteral("00000000000000000000000000000002");
const auto ipv4 = QStringLiteral("100.80.0.2");
const auto ipv6 = QStringLiteral("fd7a:115c:a1e0::2");

template<class Predicate>
bool waitFor(Predicate done, int timeoutMs = 4500)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

void writeFile(const QString &path, const QByteArray &value)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(value) == value.size());
}

QByteArray hello(const QString &source)
{
    const QHostAddress peer(source);
    const bool v6 = peer.protocol() == QAbstractSocket::IPv6Protocol;
    const int length = v6 ? 16 : 4;
    auto bytes = QByteArray::fromHex("0d0a0d0a000d0a515549540a21");
    bytes += v6 ? '\x21' : '\x11';
    QByteArray body(2 * length + 4, '\0');
    if (v6) {
        const auto address = peer.toIPv6Address();
        for (int i = 0; i < length; ++i)
            body[i] = char(address[i]);
    } else {
        qToBigEndian(peer.toIPv4Address(), body.data());
    }
    qToBigEndian<quint16>(41321, body.data() + 2 * length);
    qToBigEndian<quint16>(Port, body.data() + 2 * length + 2);
    char size[2];
    qToBigEndian<quint16>(quint16(body.size()), size);
    return bytes.append(size, 2) + body + requestFrame();
}

QJsonObject row(const Offer &offer, const QString &source = ipv4)
{
    // The legacy system provider correlates the Sunshine source address.
    // Sunshine's GameStream source port is independent of the Serve socket.
    return {{"id", pairingId}, {"name", "aurora-" + QString::fromLatin1(offer.marker)}, {"address", source}};
}

bool disconnected(const QTcpSocket &socket)
{
    return socket.state() == QAbstractSocket::UnconnectedState;
}

class FakeProcess final : public HostProcess
{
public:
    bool start(const HostLaunch &, QString &) override { alive_ = true; return true; }
    bool writePasswordFrame(const QByteArray &, QString &) override { return true; }
    void closeInput() override {}
    bool signalStopEvent() override { alive_ = false; return true; }
    bool running() override { return alive_; }
    void waitForExit(int) override { alive_ = false; }
    void forceStop() override { alive_ = false; }
    std::optional<int> exitCode() const override { return 0; }

private:
    bool alive_ = false;
};

struct Probe
{
    std::atomic_bool changeConfirmation{false};
    std::atomic_bool incompatible{false};
    std::atomic_bool holdUntilExpiry{false};
    std::atomic_bool workerEntered{false};
    std::atomic_bool workerSawExpiry{false};
    std::atomic_int nonemptyReads{0};
};

class PinnedAdmin final : public ReadinessAdmin
{
public:
    PinnedAdmin(std::unique_ptr<BundledAdmin> admin, Probe &probe) : admin_(std::move(admin)), probe_(probe) {}

    AdminIdentity identity() const override { return admin_->identity(); }
    bool readConfig(BundledAdmin::Completion done) override { return admin_->readConfig(std::move(done)); }

    bool readPendingPairings(BundledAdmin::PendingCompletion done) override
    {
        if (probe_.incompatible)
            return false;
        return admin_->readPendingPairings([this, done = std::move(done)](BundledAdmin::PendingResult result) {
            if (probe_.changeConfirmation && !result.pairings.isEmpty() && ++probe_.nonemptyReads == 2)
                result.pairings.first().id = otherId;
            done(std::move(result));
        });
    }

    bool approvePairing(SupervisorPairingApproval approval, BundledAdmin::ApprovalCompletion done,
        BundledAdmin::ApprovalStillCurrent guard) override
    {
        if (!probe_.holdUntilExpiry)
            return admin_->approvePairing(std::move(approval), std::move(done), std::move(guard));

        // Hold synthetic work on the actual lifecycle worker. The desktop test
        // thread deliberately stops dispatching events during the expiry check.
        CHECK(guard && guard());
        probe_.workerEntered = true;
        QElapsedTimer elapsed;
        elapsed.start();
        while (guard() && elapsed.elapsed() < 34000)
            QThread::msleep(10);
        probe_.workerSawExpiry = !guard();
        done({BundledAdmin::Error::Cancelled});
        return true;
    }

private:
    std::unique_ptr<BundledAdmin> admin_;
    Probe &probe_;
};

struct Fixture
{
    QTemporaryDir directory;
    LoopbackFixture server;
    Probe probe;
    std::unique_ptr<LifecycleController> lifecycle;
    std::optional<SystemPairingContext> context;
    std::unique_ptr<SystemPairingCoordinator> coordinator;
    bool trustedSocket = true;

    Fixture(const QString &cli, const QDir &tls)
        : server(tls.filePath("pinned-cert.pem"), tls.filePath("pinned-key.pem"))
    {
        CHECK(directory.isValid() && server.isListening());
        CHECK(QFile::copy(cli, path("fake-cli.exe")));
        put("serve.json", "null");
        setStatus();

        const auto layout = HostLayout::forDirectories(HostPlatform::Windows, path("app"), path("data"));
        CHECK(layout);
        const QDir payload(layout->payloadDirectory());
        for (const auto &file : {layout->executable(), payload.filePath("zlib1.dll"),
                 payload.filePath("assets/apps.json"), payload.filePath("assets/web/index.html")}) {
            CHECK(QDir().mkpath(QFileInfo(file).absolutePath()));
            writeFile(file, "synthetic fixture");
        }
        CHECK(QDir().mkpath(QFileInfo(layout->certificateFile()).absolutePath()));
        CHECK(QFile::copy(tls.filePath("pinned-cert.pem"), layout->certificateFile()));
        LifecycleDependencies dependencies{
            [] { return std::make_unique<FakeProcess>(); },
            [](HostPlatform, const PortFamily &) { return PortCheck{}; },
            [port = server.serverPort(), control = &probe](const QString &profile, const QByteArray &frame,
                QString &error) -> std::unique_ptr<ReadinessAdmin> {
                auto admin = BundledAdminFixture::open(profile, frame, port, error);
                return admin ? std::make_unique<PinnedAdmin>(std::move(admin), *control) : nullptr;
            }
        };
        lifecycle = std::make_unique<LifecycleController>(*layout, std::move(dependencies), LifecycleTiming{3000, 10, 30000});
        CHECK(lifecycle->start());
        CHECK(waitFor([&] { return lifecycle->state().phase == LifecycleController::Phase::Ready; }));
        const auto state = lifecycle->state();
        context = SystemPairingContext{EnrollmentResult::Approved,
            {Provider::System, 12, QStringLiteral("self"), state.adminIdentity.port, state.adminIdentity.certificateSha256},
            {12, QStringLiteral("self"), QStringLiteral("tailnet")}, QHostAddress(QStringLiteral("100.80.0.1")),
            1, state.settingsSession};
        coordinator = std::make_unique<SystemPairingCoordinator>(*lifecycle, [this] { return context; },
            [this] { return Installation{path("fake-cli.exe"), ServiceState::Running}; },
            [this](quint16 client, quint16 listener) -> std::optional<quint32> {
                const auto pid = GetCurrentProcessId();
                if (trustedSocket && SystemHostDetail::tcpOwnedByProcess(pid, client, listener))
                    return pid;
                return {};
            });
    }

    QString path(const QString &name) const { return directory.filePath(name); }

    void put(const QString &name, const QByteArray &value = "1")
    {
        writeFile(path(name), value);
    }

    void setStatus(const QByteArray &peerId = "peer")
    {
        put("status.json", R"({"BackendState":"Running","Self":{"ID":"self","UserID":12},)"
            R"("CurrentTailnet":{"StableID":"tailnet"},"TailscaleIPs":["100.80.0.1"],"Peer":{"a":{"ID":")" +
            peerId + R"(","UserID":12,"HostName":"host#\tname","TailscaleIPs":["100.80.0.2","fd7a:115c:a1e0::2"]}}})");
    }

    void start()
    {
        CHECK(coordinator->start());
        CHECK(waitFor([&] { return coordinator->phase() == Phase::Listening; }));
    }

    void dial(QTcpSocket &socket, const QString &source = ipv4)
    {
        QFile log(path("relay-port"));
        CHECK(log.open(QIODevice::ReadOnly));
        bool ok = false;
        const auto port = log.readAll().toUShort(&ok);
        CHECK(ok && port);
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.connectToHost(QHostAddress::LocalHost, port);
        CHECK(socket.waitForConnected(2000) && socket.write(hello(source)) > 0);
    }

    Offer offer(QTcpSocket &socket, Offer::State expected = Offer::State::Ready)
    {
        const auto size = expected == Offer::State::Ready ? 37 : 1;
        CHECK(waitFor([&] { return socket.bytesAvailable() >= size; }));
        auto result = parseOffer(socket.readAll());
        CHECK(result.state == expected);
        return result;
    }

    void holdApproval(const Offer &offer)
    {
        put("delay-status");
        server.pairings = {row(offer)};
        CHECK(waitFor([&] { return QFile::exists(path("status-started")); }));
        server.holdNextHandshake = true;
        CHECK(QFile::remove(path("delay-status")));
        CHECK(waitFor([&] { return server.handshakeHeld; }));
    }
};

void approved(const QString &cli, const QDir &tls)
{
    Fixture test(cli, tls);
    const auto valid = test.context;
    test.context->enrollment = EnrollmentResult::NeedsConfirmation;
    CHECK(!test.coordinator->start());
    test.context = valid;
    test.context->scope.provider = Provider::Embedded;
    CHECK(!test.coordinator->start());
    test.context = valid;
    test.context->scope.certificateSha256.fill('x');
    CHECK(!test.coordinator->start());
    test.context = valid;
    ++test.context->scope.adminPort;
    CHECK(!test.coordinator->start());
    test.context = valid;
    test.context->sunshineSession += "-old";
    CHECK(!test.coordinator->start());
    test.context = valid;
    test.probe.incompatible = true;
    CHECK(test.coordinator->start());
    CHECK(waitFor([&] { return test.coordinator->phase() == Phase::Failed; }));
    CHECK(!QFile::exists(test.path("relay-port")));
    test.probe.incompatible = false;
    test.start();

    for (const auto &source : {ipv4, ipv6}) {
        test.server.pairings = {};
        QTcpSocket client;
        test.dial(client, source);
        const auto offer = test.offer(client);
        const auto oldPosts = test.server.approvals.size();
        test.server.pairings = {row(offer, source == ipv4 ? QStringLiteral("::ffff:100.80.0.2") : source)};
        CHECK(waitFor([&] { return test.server.approvals.size() == oldPosts + 1 && disconnected(client); }));
        const auto sent = test.server.approvals.last();
        CHECK(sent.value("pairing_id") == pairingId && sent.value("pin") == QString::fromLatin1(offer.pin));
        CHECK(sent.value("name") == QStringLiteral("hostname"));
    }
}

void denied(const QString &cli, const QDir &tls)
{
    {
        Fixture test(cli, tls);
        test.start();
        test.trustedSocket = false;
        QTcpSocket local;
        test.dial(local);
        CHECK(waitFor([&] { return disconnected(local); }) && local.readAll().isEmpty());
        test.trustedSocket = true;
        QTcpSocket client;
        test.dial(client);
        const auto offer = test.offer(client);
        auto wrongMarker = row(offer);
        wrongMarker["id"] = otherId;
        wrongMarker["name"] = wrongMarker.value("name").toString() + "-extra";
        const auto match = row(offer);
        auto duplicateId = match;
        duplicateId["name"] = "unrelated";

        const QList<QJsonArray> invalid = {
            {wrongMarker, row(offer, QStringLiteral("100.80.0.3"))}, {match, match}, {match, duplicateId}};
        for (const auto &rows : invalid) {
            const auto reads = test.server.receivedRequests;
            test.server.pairings = rows;
            CHECK(waitFor([&] { return test.server.receivedRequests >= reads + 2; }));
            CHECK(test.server.approvals.isEmpty() && !disconnected(client));
        }

        test.put("delay-status");
        test.server.pairings = {match};
        CHECK(waitFor([&] { return QFile::exists(test.path("status-started")); }));
        test.setStatus("reassigned");
        CHECK(QFile::remove(test.path("delay-status")));
        CHECK(waitFor([&] { return disconnected(client); }));
        CHECK(test.server.approvals.isEmpty());
    }

    Fixture changed(cli, tls);
    changed.start();
    changed.probe.changeConfirmation = true;
    QTcpSocket client;
    changed.dial(client);
    changed.server.pairings = {row(changed.offer(client))};
    CHECK(waitFor([&] { return disconnected(client); }));
    CHECK(changed.probe.nonemptyReads >= 2 && changed.server.approvals.isEmpty());
}

void revoked(const QString &cli, const QDir &tls)
{
    for (const auto action : {"disconnect", "extra-data", "epoch", "lifecycle"}) {
        Fixture test(cli, tls);
        test.start();
        QTcpSocket client;
        test.dial(client);
        test.holdApproval(test.offer(client));
        const auto before = test.server.receivedBytes;
        if (qstrcmp(action, "disconnect") == 0) {
            client.abort();
            // Keep the POST in TLS until the next peer has reached admission.
            QTcpSocket concurrent;
            test.dial(concurrent);
            test.offer(concurrent, Offer::State::Rejected);
            CHECK(test.lifecycle->adminBusy());
        } else if (qstrcmp(action, "extra-data") == 0) {
            CHECK(client.write("extra", 5) == 5);
            CHECK(waitFor([&] { return disconnected(client); }));
        } else if (qstrcmp(action, "epoch") == 0) {
            ++test.context->accountEpoch;
            test.coordinator->contextChanged();
            CHECK(test.coordinator->phase() == Phase::Stopped && !test.coordinator->start());
        } else {
            CHECK(test.lifecycle->stop() && test.coordinator->phase() == Phase::Stopped);
        }
        test.server.releaseHandshake();
        CHECK(waitFor([&] { return !test.lifecycle->adminBusy() && disconnected(client); }));
        CHECK(test.server.approvals.isEmpty() && test.server.receivedBytes == before);
    }
}

void expired(const QString &cli, const QDir &tls)
{
    Fixture test(cli, tls);
    test.probe.holdUntilExpiry = true;
    test.start();
    QTcpSocket client;
    test.dial(client);
    test.server.pairings = {row(test.offer(client))};
    CHECK(waitFor([&] { return test.probe.workerEntered.load(); }));

    QElapsedTimer elapsed;
    elapsed.start();
    while (!test.probe.workerSawExpiry && elapsed.elapsed() < 35000)
        QThread::msleep(25);
    CHECK(test.probe.workerSawExpiry);
    CHECK(waitFor([&] { return disconnected(client) && !test.lifecycle->adminBusy(); }));
    CHECK(test.server.approvals.isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CHECK(app.arguments().size() == 4);
    qputenv("AURORA_PAIR_HOST_TEST_SENTINEL", "must not be inherited");
    const auto scenario = app.arguments().at(1);
    const auto cli = app.arguments().at(2);
    const QDir tls(app.arguments().at(3));
    if (scenario == QLatin1String("approved")) approved(cli, tls);
    else if (scenario == QLatin1String("denied")) denied(cli, tls);
    else if (scenario == QLatin1String("revoked")) revoked(cli, tls);
    else if (scenario == QLatin1String("expired")) expired(cli, tls);
    else CHECK(false);
}
