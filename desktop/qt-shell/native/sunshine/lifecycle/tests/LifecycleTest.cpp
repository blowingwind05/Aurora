// SPDX-License-Identifier: GPL-3.0-only
#include "LifecycleController.h"
#include "LoopbackFixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QSemaphore>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <cstdlib>
#include <utility>
#include <vector>

namespace Aurora::Sunshine {

struct BundledAdminFixture
{
    static auto open(const QString &profile, const QByteArray &password, quint16 port, QString &error)
    {
        return BundledAdmin::openAtPort(profile, password, port, error);
    }
};

} // namespace Aurora::Sunshine

namespace {

using namespace Aurora::Sunshine;
using Phase = LifecycleController::Phase;
using Error = BundledAdmin::Error;
using PortStatus = PortCheck::Status;

void require(bool condition, const QString &message)
{
    if (!condition) {
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

template <typename Predicate>
void await(Predicate condition, const QString &failure, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs)
        spin(3);
    require(condition(), failure);
}

void awaitPhase(const LifecycleController &controller, Phase phase)
{
    await([&] { return controller.state().phase == phase; },
        QStringLiteral("Expected lifecycle phase %1.").arg(int(phase)));
}

void shutdown(LifecycleController &controller)
{
    QObject guard;
    bool finished = false;
    QObject::connect(&controller, &LifecycleController::shutdownFinished, &guard, [&] { finished = true; });
    require(controller.shutdown(), QStringLiteral("Shutdown was not accepted."));
    require(!controller.start(), QStringLiteral("A closing controller accepted a start."));
    await([&] { return finished; }, QStringLiteral("The lifecycle worker did not shut down."));
}

HostLayout layoutFor(const QDir &fixture, const QString &name)
{
    const QDir test(fixture.filePath(name));
    const auto layout = HostLayout::forDirectories(HostPlatform::Windows, test.filePath(QStringLiteral("app")),
                                                   test.filePath(QStringLiteral("data")));
    require(layout.has_value(), QStringLiteral("Could not make fixture layout."));
    const QDir payload(layout->payloadDirectory());
    const QStringList paths{layout->executable(), payload.filePath(QStringLiteral("zlib1.dll")),
                            payload.filePath(QStringLiteral("assets/apps.json")),
                            payload.filePath(QStringLiteral("assets/web/index.html"))};
    for (const auto &path : paths) {
        require(QDir().mkpath(QFileInfo(path).absolutePath()), QStringLiteral("Could not create fixture directory."));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write("Offline fixture.\n") == 17,
                QStringLiteral("Could not seed fixture payload."));
    }
    return *layout;
}

struct Replay
{
    QThread *gui = QThread::currentThread();
    std::atomic<int> portChecks{0};
    std::atomic<int> launches{0};
    std::atomic<int> stops{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> adminOpens{0};
    std::atomic<int> reads{0};
    std::atomic<int> repliesDelivered{0};
    std::atomic<int> adminDestroyed{0};
    std::atomic<int> exitLaunch{0};
    std::atomic<int> exitStatus{0};
    std::atomic<bool> portBusy{false};
    std::atomic<bool> blockStart{false};
    std::atomic<bool> blockStop{false};
    std::atomic<bool> refuseStop{false};
    std::atomic<bool> touchedGui{false};
    QSemaphore launchGate;
    QSemaphore stopGate;
    QMutex frameMutex;
    QByteArray frame;

    void onWorker()
    {
        if (QThread::currentThread() == gui)
            touchedGui = true;
    }
};

class FakeProcess final : public HostProcess
{
public:
    explicit FakeProcess(std::shared_ptr<Replay> replay) : replay_(std::move(replay)) { replay_->onWorker(); }
    ~FakeProcess() override
    {
        replay_->onWorker();
        ++replay_->destroyed;
    }

    bool start(const HostLaunch &, QString &) override
    {
        replay_->onWorker();
        launch_ = ++replay_->launches;
        alive_ = true;
        if (replay_->blockStart.exchange(false))
            require(replay_->launchGate.tryAcquire(1, 3000), QStringLiteral("Start gate timed out."));
        return true;
    }

    bool writePasswordFrame(const QByteArray &frame, QString &) override
    {
        replay_->onWorker();
        QMutexLocker locker(&replay_->frameMutex);
        replay_->frame = frame;
        return true;
    }

    void closeInput() override { replay_->onWorker(); }
    bool signalStopEvent() override
    {
        replay_->onWorker();
        ++replay_->stops;
        if (replay_->blockStop.exchange(false))
            require(replay_->stopGate.tryAcquire(1, 3000), QStringLiteral("Stop gate timed out."));
        return true;
    }

    bool running() override
    {
        replay_->onWorker();
        if (alive_ && replay_->exitLaunch == launch_) {
            alive_ = false;
            status_ = replay_->exitStatus.load();
        }
        return alive_;
    }

    void waitForExit(int) override
    {
        replay_->onWorker();
        if (!replay_->refuseStop)
            alive_ = false;
    }

    void forceStop() override { replay_->onWorker(); }
    std::optional<int> exitCode() const override { return status_; }

private:
    std::shared_ptr<Replay> replay_;
    std::optional<int> status_;
    int launch_ = 0;
    bool alive_ = false;
};

struct FakeReply
{
    int delayMs = 0;
    Error error = Error::None;
};

class FakeAdmin final : public ReadinessAdmin
{
public:
    FakeAdmin(std::shared_ptr<Replay> replay, FakeReply reply) : replay_(std::move(replay)), reply_(reply) {}
    ~FakeAdmin() override
    {
        replay_->onWorker();
        ++replay_->adminDestroyed;
    }

    bool readConfig(BundledAdmin::Completion completion) override
    {
        replay_->onWorker();
        ++replay_->reads;
        // The queued callback can outlive this fake. This exercises stale-result filtering.
        QTimer::singleShot(reply_.delayMs, [replay = replay_, completion = std::move(completion), reply = reply_]() mutable {
            ++replay->repliesDelivered;
            completion({reply.error, reply.error == Error::Unauthorized ? 401 : 0,
                        reply.error == Error::None ? QString{} : QStringLiteral("Fixture admin failure."), {}});
        });
        return true;
    }

private:
    std::shared_ptr<Replay> replay_;
    FakeReply reply_;
};

LifecycleDependencies fakeDependencies(const std::shared_ptr<Replay> &replay, std::vector<FakeReply> replies = {{}})
{
    return {
        [replay] { return std::make_unique<FakeProcess>(replay); },
        [replay](HostPlatform, const PortFamily &) {
            replay->onWorker();
            ++replay->portChecks;
            if (replay->portBusy)
                return PortCheck{PortStatus::Occupied, PortProtocol::Tcp, 48990};
            return PortCheck{};
        },
        [replay, replies = std::move(replies)](const QString &, const QByteArray &frame, QString &) {
            replay->onWorker();
            require(frame.size() == 64, QStringLiteral("Readiness did not receive the launch frame."));
            const auto index = size_t(replay->adminOpens++);
            return std::make_unique<FakeAdmin>(replay, replies.at(qMin(index, replies.size() - 1)));
        }
    };
}

void testPortOwnership()
{
    const auto ports = bundledPorts();
    require(ports.tcp == QList<quint16>{48984, 48989, 48990, 49010} &&
            ports.udp == QList<quint16>{48998, 48999, 49000, 49002},
            QStringLiteral("Legacy bundled port family differs."));

    QTcpServer tcp;
    tcp.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    QUdpSocket udp;
    udp.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    require(tcp.listen(QHostAddress::LocalHost, 0) &&
            udp.bind(QHostAddress(QHostAddress::LocalHost), 0, QUdpSocket::DontShareAddress),
            QStringLiteral("Cannot bind ephemeral loopback fixtures."));

    const auto checkOccupied = [&](HostPlatform platform) {
        const auto tcpResult = checkPorts(platform, {{tcp.serverPort()}, {}});
        const auto udpResult = checkPorts(platform, {{}, {udp.localPort()}});
        require(tcpResult.status == PortStatus::Occupied && tcpResult.protocol == PortProtocol::Tcp &&
                tcpResult.port == tcp.serverPort() && udpResult.status == PortStatus::Occupied &&
                udpResult.protocol == PortProtocol::Udp && udpResult.port == udp.localPort(),
                QStringLiteral("Port preflight missed an ephemeral listener on platform %1. TCP %2, UDP %3.")
                    .arg(int(platform)).arg(int(tcpResult.status)).arg(int(udpResult.status)));
    };
#if defined(Q_OS_WIN)
    checkOccupied(HostPlatform::Windows);
    QTcpSocket outbound;
    outbound.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    outbound.connectToHost(QHostAddress::LocalHost, tcp.serverPort());
    require(outbound.waitForConnected(1000), QStringLiteral("Cannot connect to ephemeral listener."));
    require(checkPorts(HostPlatform::Windows, {{outbound.localPort()}, {}}).status == PortStatus::Available,
            QStringLiteral("An outgoing TCP connection was classified as a listener."));
    QTcpServer tcp6;
    tcp6.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    if (tcp6.listen(QHostAddress::LocalHostIPv6, 0))
        require(checkPorts(HostPlatform::Windows, {{tcp6.serverPort()}, {}}).status == PortStatus::Occupied,
                QStringLiteral("IPv6 TCP listener was missed."));
    QUdpSocket udp6;
    udp6.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    if (udp6.bind(QHostAddress(QHostAddress::LocalHostIPv6), 0, QUdpSocket::DontShareAddress))
        require(checkPorts(HostPlatform::Windows, {{}, {udp6.localPort()}}).status == PortStatus::Occupied,
                QStringLiteral("IPv6 UDP binding was missed."));
#else
    checkOccupied(HostPlatform::MacOS);
#endif
}

void testRetryAndRestart(const QDir &fixtures)
{
    const auto replay = std::make_shared<Replay>();
    replay->portBusy = true;
    LifecycleController controller(layoutFor(fixtures, QStringLiteral("Retry and restart")),
        fakeDependencies(replay, {{0, Error::Network}, {0, Error::Unauthorized}, {0, Error::None}}),
        {800, 5, 30000});

    require(controller.start(), QStringLiteral("Port conflict start was not submitted."));
    awaitPhase(controller, Phase::Failed);
    require(replay->launches == 0 && replay->reads == 0 && controller.state().error.contains(QStringLiteral("48990")),
            QStringLiteral("Conflict must fail before launch."));

    replay->portBusy = false;
    require(controller.start(), QStringLiteral("Explicit retry was not accepted."));
    awaitPhase(controller, Phase::Failed);
    require(replay->reads == 2 && replay->stops == 1, QStringLiteral("Transport retry or auth refusal behaved incorrectly."));

    require(controller.start(), QStringLiteral("Retry after auth failure was not accepted."));
    awaitPhase(controller, Phase::Ready);
    require(!controller.start(), QStringLiteral("Start on a ready launch scheduled extra work."));
    require(controller.restart() && !controller.restart(), QStringLiteral("Explicit restart was not coalesced."));
    awaitPhase(controller, Phase::Ready);
    require(replay->launches == 3 && replay->stops == 2, QStringLiteral("Manual restart did not reap before relaunch."));

    const auto request = controller.state().request;
    replay->exitStatus = BundledHost::restartRequestedExitCode;
    replay->exitLaunch = 3;
    await([&] { return replay->launches == 4 && controller.state().phase == Phase::Ready; },
        QStringLiteral("A ready companion's restart request was not honored."));
    require(controller.state().request == request, QStringLiteral("Automatic restart lost the user request."));
    replay->exitLaunch = 4;
    awaitPhase(controller, Phase::Failed);
    require(replay->launches == 4, QStringLiteral("Repeated exit 42 triggered a restart loop."));

    shutdown(controller);
    require(!replay->touchedGui, QStringLiteral("A backend ran on the GUI thread."));
}

void testCancellation(const QDir &fixtures)
{
    const auto replay = std::make_shared<Replay>();
    LifecycleController controller(layoutFor(fixtures, QStringLiteral("Cancellation")),
        fakeDependencies(replay, {{180, Error::None}, {700, Error::None}}), {1600, 5, 30000});

    replay->blockStart = true;
    require(controller.start(), QStringLiteral("Blocking launch was not queued."));
    await([&] { return replay->launches == 1; }, QStringLiteral("The launch worker did not start."));
    // The GUI remains active while the launch backend is blocked.
    require(controller.stop(), QStringLiteral("Stop did not cancel the blocked startup."));
    replay->launchGate.release();
    awaitPhase(controller, Phase::Stopped);
    require(replay->reads == 0 && replay->stops == 1, QStringLiteral("Cancelled launch reached readiness or leaked a host."));

    require(controller.start(), QStringLiteral("Start after cancellation was refused."));
    await([&] { return replay->reads == 1; }, QStringLiteral("Old readiness read never started."));
    require(controller.stop(), QStringLiteral("Stop during readiness was not accepted."));
    awaitPhase(controller, Phase::Stopped);
    require(replay->adminDestroyed == 1, QStringLiteral("The in-flight admin client was retained."));
    require(controller.start(), QStringLiteral("Replacement start was not accepted."));
    await([&] { return replay->reads == 2; }, QStringLiteral("New readiness read never started."));
    await([&] { return replay->repliesDelivered == 1; }, QStringLiteral("The cancelled read did not replay."));
    spin(20);
    require(controller.state().phase == Phase::Starting, QStringLiteral("Old success revived a new request."));
    awaitPhase(controller, Phase::Ready);
    shutdown(controller);
    require(!replay->touchedGui, QStringLiteral("A blocked launch or cleanup ran on the GUI thread."));
}

void testReadinessDeadline(const QDir &fixtures)
{
    const auto replay = std::make_shared<Replay>();
    LifecycleController controller(layoutFor(fixtures, QStringLiteral("Readiness timeout")),
        fakeDependencies(replay, {{500, Error::None}}), {90, 5, 30000});
    require(controller.start(), QStringLiteral("Timeout fixture did not start."));
    awaitPhase(controller, Phase::Failed);
    require(replay->reads == 1 && replay->adminDestroyed == 1 && replay->stops == 1 && !controller.state().cleanupPending,
            QStringLiteral("Readiness deadline did not cancel the pending check and clean up."));
    shutdown(controller);
}

void testCleanupAndDestruction(const QDir &fixtures)
{
    const auto replay = std::make_shared<Replay>();
    auto controller = std::make_unique<LifecycleController>(layoutFor(fixtures, QStringLiteral("Cleanup")),
        fakeDependencies(replay), LifecycleTiming{800, 5, 30000});
    require(controller->start(), QStringLiteral("Start for cleanup fixture failed."));
    awaitPhase(*controller, Phase::Ready);
    replay->refuseStop = true;
    require(controller->stop(), QStringLiteral("Stop for cleanup fixture failed."));
    awaitPhase(*controller, Phase::Failed);
    require(controller->state().cleanupPending, QStringLiteral("An unreaped host was reported cleaned up."));
    require(controller->start(), QStringLiteral("Retry after stop failure was not queued."));
    awaitPhase(*controller, Phase::Failed);
    require(controller->state().cleanupPending && replay->launches == 1,
            QStringLiteral("A failed cleanup did not fence relaunch."));

    replay->refuseStop = false;
    require(controller->start(), QStringLiteral("Retry after recovery was not queued."));
    awaitPhase(*controller, Phase::Ready);
    require(replay->launches == 2 && !controller->state().cleanupPending,
            QStringLiteral("Recovered host did not come up safely."));

    replay->blockStop = true;
    const auto destroyed = replay->destroyed.load();
    const auto stops = replay->stops.load();
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    require(timer.elapsed() < 500, QStringLiteral("Controller destruction waited on the GUI thread."));
    await([&] { return replay->stops > stops; }, QStringLiteral("Controller destruction did not request cleanup."));
    replay->stopGate.release();
    await([&] { return replay->destroyed > destroyed; }, QStringLiteral("Destroyed controller left a worker-owned process."));
}

class RealAdmin final : public ReadinessAdmin
{
public:
    explicit RealAdmin(std::unique_ptr<BundledAdmin> admin) : admin_(std::move(admin)) {}
    bool readConfig(BundledAdmin::Completion done) override { return admin_->readConfig(std::move(done)); }
private:
    std::unique_ptr<BundledAdmin> admin_;
};

void testPinnedReadiness(const QDir &fixtures, const QDir &identities, bool goodPin)
{
    const auto cert = identities.filePath(QStringLiteral("pinned-cert.pem"));
    const auto identity = goodPin ? QStringLiteral("pinned") : QStringLiteral("other");
    LoopbackFixture server(identities.filePath(identity + QStringLiteral("-cert.pem")),
                           identities.filePath(identity + QStringLiteral("-key.pem")));
    require(server.isListening(), QStringLiteral("Ephemeral TLS fixture could not listen."));
    const auto layout = layoutFor(fixtures, goodPin ? QStringLiteral("Pinned readiness") : QStringLiteral("Wrong pin"));
    require(QDir().mkpath(QFileInfo(layout.certificateFile()).absolutePath()) && QFile::copy(cert, layout.certificateFile()),
            QStringLiteral("Could not stage the synthetic certificate."));

    const auto replay = std::make_shared<Replay>();
    auto deps = fakeDependencies(replay);
    deps.openAdmin = [replay, port = server.serverPort()](const QString &profile, const QByteArray &frame,
                                                         QString &error) -> std::unique_ptr<ReadinessAdmin> {
        replay->onWorker();
        ++replay->adminOpens;
        auto admin = BundledAdminFixture::open(profile, frame, port, error);
        if (!admin)
            return {};
        return std::make_unique<RealAdmin>(std::move(admin));
    };
    LifecycleController controller(layout, std::move(deps), {goodPin ? 2400 : 250, 10, 30000});
    require(controller.start(), QStringLiteral("TLS readiness fixture did not start."));
    if (goodPin) {
        await([&] { return controller.state().phase == Phase::Ready; }, QStringLiteral("Pinned readiness was refused."), 2800);
        QMutexLocker locker(&replay->frameMutex);
        const auto expected = "Basic " + (QByteArray("aurora-local:") + replay->frame).toBase64();
        require(server.receivedRequests == 1 && server.authorization == expected,
                QStringLiteral("Readiness did not authenticate with the current launch frame."));
    } else {
        awaitPhase(controller, Phase::Failed);
        require(replay->adminOpens >= 1 && replay->stops == 1 && server.connections > 0 && server.receivedBytes == 0,
                QStringLiteral("A wrong TLS peer received HTTP data or prevented cleanup."));
    }
    shutdown(controller);
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QTemporaryDir root(QDir::current().filePath(QStringLiteral("sunshine-lifecycle-test-XXXXXX")));
    require(root.isValid() && app.arguments().size() == 2 && QSslSocket::supportsSsl(),
            QStringLiteral("The fixture requires a temporary directory and a Qt TLS backend."));
    const QDir fixtures(root.path());
    const QDir identities(app.arguments().at(1));
    testPortOwnership();
    testRetryAndRestart(fixtures);
    testCancellation(fixtures);
    testReadinessDeadline(fixtures);
    testCleanupAndDestruction(fixtures);
    testPinnedReadiness(fixtures, identities, true);
    testPinnedReadiness(fixtures, identities, false);
    QTextStream(stdout) << "PASS: ephemeral port ownership and pinned admin, worker launch and cleanup, "
                          "retry, restart, cancellation and stale-result protection\n";
}
