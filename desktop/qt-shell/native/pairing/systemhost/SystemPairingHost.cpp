// SPDX-License-Identifier: GPL-3.0-only
#include "SystemPairingHost.h"

#include "PairingWire.h"
#include "WindowsTransport.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <utility>

namespace Aurora::Pairing {
namespace {

using namespace aurora::tailscale;
using namespace aurora::tailscale::system;
using namespace SystemHostDetail;

constexpr int startupMs = 10000;
constexpr int statusMs = 8000;
constexpr int helloMs = 3000;
constexpr int connectionMs = 31000;
constexpr int proxyHeaderBytes = 16;
constexpr int proxyPayloadLimit = 512;
constexpr int outputLimit = 64 * 1024;
constexpr int maxConnections = 4;

const auto protectedSocket = QStringLiteral(R"(--socket=\\.\pipe\protectedprefix\administrators\tailscale\tailscaled)");
const auto loopback = QHostAddress(QHostAddress::LocalHost);

bool validAccount(const SystemHostAccount &account)
{
    const auto &identity = account.identity;
    const auto &address = account.localAddress;
    return identity.ownerId > 0 && !identity.nodeId.trimmed().isEmpty() && !identity.tailnetId.trimmed().isEmpty() &&
        !address.isNull() && !address.isLoopback() && address.scopeId().isEmpty() &&
        address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6;
}

bool sameAccount(const std::optional<SystemHostAccount> &a, const std::optional<SystemHostAccount> &b)
{
    return a && b ? a->identity == b->identity && a->localAddress == b->localAddress : !a && !b;
}

std::optional<Status> matchingStatus(const QByteArray &bytes, const SystemHostAccount &account)
{
    auto reply = decodeSystemStatus(bytes);
    if (reply.error != StatusError::None || !reply.status || reply.status->state != BackendState::Connected ||
        !reply.status->account || !(*reply.status->account == account.identity) ||
        reply.status->localAddress != account.localAddress)
        return std::nullopt;
    return std::move(reply.status);
}

const Peer *uniquePeer(const Status &status, const QHostAddress &address)
{
    if (address == status.localAddress)
        return nullptr;
    const Peer *match = nullptr;
    for (const auto &peer : status.peers) {
        if (address != peer.address && address != peer.alternateAddress)
            continue;
        if (match)
            return nullptr;
        match = &peer;
    }
    return match;
}

enum class PortState { Free, Occupied, Invalid };

PortState portState(const QJsonObject &config, const QString &port, int depth = 0)
{
    if (depth == 32)
        return PortState::Invalid;
    const auto tcp = config.value(QLatin1String("TCP"));
    const auto funnel = config.value(QLatin1String("AllowFunnel"));
    const auto foreground = config.value(QLatin1String("Foreground"));
    for (const auto &map : {tcp, funnel, foreground}) {
        if (!map.isUndefined() && !map.isNull() && !map.isObject())
            return PortState::Invalid;
    }
    if (tcp.toObject().contains(port))
        return PortState::Occupied;

    const auto allows = funnel.toObject();
    const auto suffix = QLatin1Char(':') + port;
    for (auto entry = allows.constBegin(); entry != allows.constEnd(); ++entry) {
        if (!entry.key().endsWith(suffix))
            continue;
        if (!entry.value().isBool())
            return PortState::Invalid;
        if (entry.value().toBool())
            return PortState::Occupied;
    }

    const auto sessions = foreground.toObject();
    for (const auto &session : sessions) {
        if (!session.isObject())
            return PortState::Invalid;
        const auto result = portState(session.toObject(), port, depth + 1);
        if (result != PortState::Free)
            return result;
    }
    return PortState::Free;
}

PortState portState(const QByteArray &bytes)
{
    if (bytes.trimmed() == "null")
        return PortState::Free;
    const auto config = QJsonDocument::fromJson(bytes);
    return config.isObject() ? portState(config.object(), QString::number(Port)) : PortState::Invalid;
}

class Command final : public QProcess
{
public:
    using Complete = std::function<void(bool, const QByteArray &)>;
    using Ready = std::function<void()>;

    Command(QObject *parent, int limit, Complete complete, Ready ready = {})
        : QProcess(parent), limit_(limit), complete_(std::move(complete)), ready_(std::move(ready)),
          relay_(bool(ready_))
    {
        alarm_.setSingleShot(true);
        alarm_.setTimerType(Qt::PreciseTimer);
        connect(&alarm_, &QTimer::timeout, this, [this] { finish(false); });
        connect(this, &QProcess::started, this, &QProcess::closeWriteChannel);
        connect(this, &QProcess::readyReadStandardOutput, this, [this] { collectOutput(); });
        connect(this, &QProcess::readyReadStandardError, this, [this] { collectError(); });
        connect(this, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                collectError();
                collectOutput();
                finish(!relay_ && status == QProcess::NormalExit && code == 0);
            });
        connect(this, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { finish(false); });
    }

    void run(const QString &program, const QStringList &args, int timeout)
    {
        if (timeout <= 0 || !prepareProcess(*this, program) || (relay_ && !bindToParentJob(*this))) {
            finish(false);
            return;
        }
        setArguments(QStringList{protectedSocket} + args);
        deadline_ = QDeadlineTimer(timeout, Qt::PreciseTimer);
        alarm_.start(timeout);
        QProcess::start();
    }

    void cancel()
    {
        complete_ = {};
        ready_ = {};
        alarm_.stop();
        if (state() != NotRunning)
            kill();
        deleteLater();
    }

private:
    void collectOutput()
    {
        if (!complete_)
            return;
        const auto bytes = readAllStandardOutput();
        stdoutBytes_ += bytes.size();
        if (stdoutBytes_ > limit_ || (!relayReady_ && deadline_.hasExpired())) {
            finish(false);
            return;
        }
        if (relayReady_)
            return;
        output_ += bytes;
        if (!relay_)
            return;
        const auto newline = output_.indexOf('\n');
        if ((newline < 0 && output_.size() > 4096) || newline > 4096) {
            finish(false);
        } else if (newline >= 0) {
            if (output_.left(newline).trimmed() != "Available within your tailnet:" || state() != Running) {
                finish(false);
                return;
            }
            relayReady_ = true;
            alarm_.stop();
            output_.clear();
            auto callback = std::exchange(ready_, {});
            callback();
        }
    }

    void collectError()
    {
        if (!complete_)
            return;
        stderrBytes_ += readAllStandardError().size();
        if (stderrBytes_ > outputLimit)
            finish(false);
    }

    void finish(bool ok)
    {
        auto complete = std::exchange(complete_, {});
        if (!complete)
            return;
        const auto output = std::exchange(output_, {});
        const bool success = ok && !deadline_.hasExpired();
        cancel();
        complete(success, output);
    }

    int limit_;
    Complete complete_;
    Ready ready_;
    const bool relay_;
    QTimer alarm_;
    QDeadlineTimer deadline_;
    QByteArray output_;
    qint64 stdoutBytes_ = 0;
    qint64 stderrBytes_ = 0;
    bool relayReady_ = false;
};

struct Attempt final : QObject
{
    enum class Phase { Proxy, Request, Checking, Authorized, Rechecking, Rejecting };

    Attempt(QObject *parent, QTcpSocket *socket, quint32 pid)
        : QObject(parent), channel(socket), daemonPid(pid)
    {
        socket->setParent(this);
        timer.setSingleShot(true);
        timer.setTimerType(Qt::PreciseTimer);
    }

    QTcpSocket *const channel;
    const quint32 daemonPid;
    const QDeadlineTimer deadline{connectionMs, Qt::PreciseTimer};
    const std::shared_ptr<std::atomic_bool> current = std::make_shared<std::atomic_bool>(true);
    const QDeadlineTimer hello{helloMs, Qt::PreciseTimer};
    QTimer timer;
    Phase phase = Phase::Proxy;
    ProxySource source;
    QString nodeId;
    bool revalidated = false;
    Command *command = nullptr;

    bool exposed() const { return phase == Phase::Authorized || phase == Phase::Rechecking; }
};

} // namespace

struct SystemPairingHost::Private final : QObject
{
    Private(SystemPairingHost *owner, Locate locate, FindOwner findOwner)
        : QObject(owner), owner(owner), locate(std::move(locate)), findOwner(std::move(findOwner))
    {
        startTimer.setSingleShot(true);
        startTimer.setTimerType(Qt::PreciseTimer);
        listener.setMaxPendingConnections(8);
        connect(&startTimer, &QTimer::timeout, this, [this] { shutdown(SystemHostStop::StartupTimeout); });
        connect(&listener, &QTcpServer::newConnection, this, &Private::accept);
        connect(&listener, &QTcpServer::acceptError, this, [this] { shutdown(SystemHostStop::ListenFailed); });
    }

    ~Private() override { shutdown(SystemHostStop::Cancelled, false); }

    void checkInstallationAndAccount()
    {
        program = runningProgram(locate());
        if (program.isEmpty()) {
            shutdown(SystemHostStop::InstallationUnavailable);
            return;
        }
        startup = new Command(this, maxSystemStatusBytes, [this](bool ok, const QByteArray &bytes) {
            startup = nullptr;
            if (!ok || !matchingStatus(bytes, *account)) {
                shutdown(startDeadline.hasExpired() ? SystemHostStop::StartupTimeout : SystemHostStop::IdentityMismatch);
                return;
            }
            checkPort();
        });
        startup->run(program, {QStringLiteral("status"), QStringLiteral("--json")}, startupRemaining());
    }

    void checkPort()
    {
        if (!sameInstallation()) {
            shutdown(SystemHostStop::InstallationUnavailable);
            return;
        }
        startup = new Command(this, outputLimit, [this](bool ok, const QByteArray &bytes) {
            startup = nullptr;
            if (startDeadline.hasExpired()) {
                shutdown(SystemHostStop::StartupTimeout);
                return;
            }
            const auto state = ok ? portState(bytes) : PortState::Invalid;
            if (state != PortState::Free) {
                shutdown(state == PortState::Occupied ? SystemHostStop::PortOccupied : SystemHostStop::PortCheckFailed);
                return;
            }
            serve();
        });
        startup->run(program, {QStringLiteral("serve"), QStringLiteral("status"), QStringLiteral("--json")}, startupRemaining());
    }

    void serve()
    {
        if (!sameInstallation()) {
            shutdown(SystemHostStop::InstallationUnavailable);
            return;
        }
        if (!listenLoopback(listener)) {
            shutdown(SystemHostStop::ListenFailed);
            return;
        }
        relay = new Command(this, outputLimit, [this](bool, const QByteArray &) {
            relay = nullptr;
            shutdown(!active && startDeadline.hasExpired() ? SystemHostStop::StartupTimeout : SystemHostStop::RelayFailed);
        }, [this] {
            if (startDeadline.hasExpired()) {
                shutdown(SystemHostStop::StartupTimeout);
                return;
            }
            if (!sameInstallation()) {
                shutdown(SystemHostStop::InstallationUnavailable);
                return;
            }
            active = true;
            startTimer.stop();
            emit owner->listening(host, generation);
        });
        relay->run(program, {QStringLiteral("serve"), QStringLiteral("--tcp=%1").arg(Port),
            QStringLiteral("--proxy-protocol=2"), QStringLiteral("tcp://127.0.0.1:%1").arg(listener.serverPort())},
            startupRemaining());
    }

    int startupRemaining() const { return int(startDeadline.remainingTime()); }

    bool sameInstallation() const
    {
        return !program.isEmpty() && runningProgram(locate()).compare(program, Qt::CaseInsensitive) == 0;
    }

    bool serving() const { return active && relay && relay->state() == QProcess::Running; }

    std::optional<quint32> socketOwner(const QTcpSocket *socket) const
    {
        if (socket->state() != QAbstractSocket::ConnectedState || socket->peerAddress() != loopback ||
            socket->localAddress() != loopback || !socket->peerPort() || socket->localPort() != listener.serverPort())
            return std::nullopt;
        return findOwner(socket->peerPort(), socket->localPort());
    }

    void accept()
    {
        while (auto *socket = listener.nextPendingConnection()) {
            const auto pid = serving() && attempts.size() < maxConnections ? socketOwner(socket) : std::nullopt;
            if (!pid || !*pid) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            socket->setReadBufferSize(proxyHeaderBytes + proxyPayloadLimit + requestFrame().size() + 1);
            const auto id = ++nextConnection;
            auto *attempt = new Attempt(this, socket, *pid);
            attempts.insert(id, attempt);
            connect(socket, &QTcpSocket::disconnected, this, [this, id] { drop(id); });
            connect(socket, &QTcpSocket::readyRead, this, [this, id] { receive(id); });
            connect(&attempt->timer, &QTimer::timeout, this, [this, id] { drop(id); });
            attempt->timer.start(helloMs);
            receive(id);
        }
    }

    void receive(quint64 id)
    {
        auto *attempt = attempts.value(id);
        if (!attempt)
            return;
        auto *socket = attempt->channel;
        if (!serving() || attempt->deadline.hasExpired()) {
            drop(id);
            return;
        }
        if (attempt->phase == Attempt::Phase::Proxy || attempt->phase == Attempt::Phase::Request) {
            if (attempt->hello.hasExpired()) {
                drop(id);
                return;
            }
        }
        if (attempt->phase == Attempt::Phase::Proxy) {
            const auto header = socket->peek(proxyHeaderBytes);
            if (header.size() != proxyHeaderBytes)
                return;
            const int payload = qFromBigEndian<quint16>(header.constData() + 14);
            // Validate the framing prefix before waiting for a claimed payload.
            const int family = header[13] == 0x11 ? 4 : header[13] == 0x21 ? 16 : 0;
            if (!header.startsWith(QByteArrayLiteral("\r\n\r\n\0\r\nQUIT\n")) || header[12] != 0x21 ||
                !family || payload < 2 * family + 4 || payload > proxyPayloadLimit) {
                drop(id);
                return;
            }
            if (socket->bytesAvailable() < proxyHeaderBytes + payload)
                return;
            const auto source = parseProxySource(socket->read(proxyHeaderBytes + payload));
            if (!source || !source->port || source->address.isNull() || source->address.isLoopback()) {
                drop(id);
                return;
            }
            attempt->source = *source;
            attempt->phase = Attempt::Phase::Request;
        }
        if (attempt->phase == Attempt::Phase::Request) {
            const auto expected = requestFrame();
            if (socket->bytesAvailable() < expected.size())
                return;
            if (socket->read(expected.size()) != expected || socket->bytesAvailable()) {
                drop(id);
                return;
            }
            socket->setReadBufferSize(1);
            attempt->timer.start(int(attempt->deadline.remainingTime()));
            checkPeer(id, *attempt, false);
        } else if (socket->bytesAvailable()) {
            drop(id);
        }
    }

    bool checkPeer(quint64 id, Attempt &attempt, bool again)
    {
        if (!serving() || attempt.deadline.hasExpired() || attempt.channel->bytesAvailable() ||
            socketOwner(attempt.channel) != attempt.daemonPid || !sameInstallation()) {
            drop(id);
            return false;
        }
        attempt.revalidated = false;
        attempt.phase = again ? Attempt::Phase::Rechecking : Attempt::Phase::Checking;
        attempt.command = new Command(this, maxSystemStatusBytes, [this, id, again](bool ok, const QByteArray &bytes) {
            auto *attempt = attempts.value(id);
            attempt->command = nullptr;
            peerStatus(id, *attempt, again, ok, bytes);
        });
        attempt.command->run(program, {QStringLiteral("status"), QStringLiteral("--json")},
            std::min(statusMs, int(attempt.deadline.remainingTime())));
        return true;
    }

    void peerStatus(quint64 id, Attempt &attempt, bool again, bool ok, const QByteArray &bytes)
    {
        if (!ok || !serving() || attempt.deadline.hasExpired() || attempt.channel->bytesAvailable() ||
            socketOwner(attempt.channel) != attempt.daemonPid || !sameInstallation()) {
            drop(id);
            return;
        }
        const auto status = matchingStatus(bytes, *account);
        if (!status) {
            drop(id);
            return;
        }
        const auto *peer = uniquePeer(*status, attempt.source.address);
        if (again) {
            if (!peer || peer->nodeId != attempt.nodeId) {
                drop(id);
                return;
            }
            attempt.phase = Attempt::Phase::Authorized;
            attempt.revalidated = true;
            emit owner->revalidated(host, generation, id);
            return;
        }
        if (!peer || offered) {
            attempt.phase = Attempt::Phase::Rejecting;
            attempt.timer.start(250);
            attempt.channel->write(QByteArray(1, '\0'));
            attempt.channel->disconnectFromHost();
            return;
        }
        offered = id;
        attempt.nodeId = peer->nodeId;
        attempt.phase = Attempt::Phase::Authorized;
        const SystemHostPeer info{peer->nodeId, peer->name, attempt.source.address};
        emit owner->authenticated(host, generation, id, info, attempt.channel);
    }

    void drop(quint64 id, bool notify = true)
    {
        auto *attempt = attempts.take(id);
        if (!attempt)
            return;
        attempt->current->store(false);
        if (attempt->command)
            attempt->command->cancel();
        attempt->timer.stop();
        disconnect(attempt->channel, nullptr, this, nullptr);
        attempt->channel->abort();
        if (offered == id)
            offered = 0;
        attempt->deleteLater();
        if (notify && attempt->exposed())
            emit owner->connectionClosed(host, id);
    }

    void shutdown(SystemHostStop reason, bool notify = true)
    {
        if (!host)
            return;
        const auto id = std::exchange(host, 0);
        const auto epoch = hostGeneration;
        const auto connection = offered;
        if (auto *attempt = attempts.value(connection))
            attempt->current->store(false);
        active = false;
        startTimer.stop();
        while (listener.hasPendingConnections()) {
            auto *socket = listener.nextPendingConnection();
            socket->abort();
            socket->deleteLater();
        }
        listener.close();
        for (const auto attempt : attempts.keys())
            drop(attempt, false);
        if (auto *command = std::exchange(startup, nullptr))
            command->cancel();
        if (auto *command = std::exchange(relay, nullptr))
            command->cancel();
        if (notify) {
            if (connection)
                emit owner->connectionClosed(id, connection);
            emit owner->stopped(id, epoch, reason);
        }
    }

    SystemPairingHost *const owner;
    const Locate locate;
    const FindOwner findOwner;
    std::optional<SystemHostAccount> account;
    quint64 generation = 0;
    quint64 host = 0;
    quint64 hostGeneration = 0;
    quint64 nextHost = 0;
    quint64 nextConnection = 0;
    quint64 offered = 0;
    bool active = false;
    QString program;
    QDeadlineTimer startDeadline;
    QTimer startTimer;
    QTcpServer listener;
    QHash<quint64, Attempt *> attempts;
    Command *startup = nullptr;
    Command *relay = nullptr;
};

SystemPairingHost::SystemPairingHost(QObject *parent)
    : SystemPairingHost(inspectWindowsInstallation, loopbackOwner, parent)
{
}

SystemPairingHost::SystemPairingHost(Locate locate, FindOwner findOwner, QObject *parent)
    : QObject(parent), d_(std::make_unique<Private>(this, std::move(locate), std::move(findOwner)))
{
}

SystemPairingHost::~SystemPairingHost() = default;

void SystemPairingHost::setAuthorization(quint64 generation, std::optional<SystemHostAccount> account)
{
    if (generation < d_->generation)
        return;
    if (account && !validAccount(*account))
        account.reset();
    if (generation == d_->generation && sameAccount(account, d_->account))
        return;
    d_->generation = generation;
    d_->account = std::move(account);
    d_->shutdown(SystemHostStop::AuthorizationChanged);
}

quint64 SystemPairingHost::start(quint64 generation)
{
    if (!d_->account || !d_->locate || !d_->findOwner || d_->host || generation != d_->generation)
        return 0;
    const auto id = ++d_->nextHost;
    d_->host = id;
    d_->hostGeneration = generation;
    d_->startDeadline = QDeadlineTimer(startupMs, Qt::PreciseTimer);
    d_->startTimer.start(startupMs);
    QTimer::singleShot(0, d_.get(), [state = d_.get(), id] {
        if (state->host == id)
            state->checkInstallationAndAccount();
    });
    return id;
}

void SystemPairingHost::stop(quint64 host)
{
    if (host && host == d_->host)
        d_->shutdown(SystemHostStop::Cancelled);
}

bool SystemPairingHost::revalidate(quint64 host, quint64 connection)
{
    auto *attempt = d_->attempts.value(connection);
    return host && host == d_->host && attempt && attempt->phase == Attempt::Phase::Authorized &&
        d_->checkPeer(connection, *attempt, true);
}

SystemPairingHost::StillCurrent SystemPairingHost::approvalGuard(quint64 host, quint64 connection) const
{
    const auto *attempt = d_->attempts.value(connection);
    if (!host || host != d_->host || !d_->serving() || !attempt || attempt->phase != Attempt::Phase::Authorized ||
        !attempt->revalidated || attempt->channel->state() != QAbstractSocket::ConnectedState || attempt->channel->bytesAvailable() ||
        attempt->deadline.hasExpired())
        return {};
    return [current = attempt->current, deadline = attempt->deadline] {
        return current->load() && !deadline.hasExpired();
    };
}

void SystemPairingHost::closeConnection(quint64 host, quint64 connection)
{
    if (host && host == d_->host)
        d_->drop(connection);
}

} // namespace Aurora::Pairing
