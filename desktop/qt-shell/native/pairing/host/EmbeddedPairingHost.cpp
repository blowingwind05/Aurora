// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedPairingHost.h"
#include "LifecycleController.h"
#include "PairingWire.h"

#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>

namespace Aurora::Pairing {
namespace {

using Aurora::Sunshine::AdminIdentity;
using Aurora::Sunshine::LifecycleController;
using Aurora::Sunshine::PendingPairing;
using aurora::tailscale::BackendState;
using aurora::tailscale::Peer;
using aurora::tailscale::Status;
using aurora::tailscale::StatusError;

constexpr int maxConnections = 4;
constexpr int startupTimeoutMs = 10000;
constexpr int helloTimeoutMs = 3000;
constexpr int attemptTimeoutMs = 31000;
constexpr int pendingPollMs = 250;

bool tailnetIpv4(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol &&
        (address.toIPv4Address() & 0xffc00000u) == 0x64400000u;
}

bool isEnrolled(const EmbeddedHostContext &context)
{
    return context.enrollment == EnrollmentResult::Approved && context.scope.provider == Provider::Embedded &&
        context.scope.owner == context.account.ownerId && context.scope.localNode == context.account.nodeId &&
        !approvalId(context.scope).isEmpty() &&
        !context.account.tailnetId.isEmpty() && tailnetIpv4(context.localAddress) &&
        context.accountEpoch != 0 && context.helperGeneration != 0 && !context.sunshineSession.isEmpty();
}

bool sameEnrollment(const EmbeddedHostContext &a, const EmbeddedHostContext &b)
{
    return isEnrolled(b) && a.accountEpoch == b.accountEpoch && a.account == b.account &&
        a.localAddress == b.localAddress && a.sunshineSession == b.sunshineSession &&
        a.scope.adminPort == b.scope.adminPort && a.scope.certificateSha256 == b.scope.certificateSha256;
}

bool sameAccount(const Status &status, const EmbeddedHostContext &context)
{
    return status.state == BackendState::Connected && status.account && *status.account == context.account &&
        status.localAddress == context.localAddress;
}

const Peer *uniquePeer(const Status &status, const QHostAddress &source)
{
    if (!tailnetIpv4(source) || source == status.localAddress)
        return nullptr;

    const Peer *match = nullptr;
    for (const auto &peer : status.peers) {
        if (peer.address != source && peer.alternateAddress != source)
            continue;
        if (match || peer.nodeId.isEmpty())
            return nullptr;
        match = &peer;
    }
    return match;
}

std::optional<PendingPairing> singlePending(const QList<PendingPairing> &rows, const QHostAddress &source,
                                            const QByteArray &marker)
{
    const auto name = QStringLiteral("aurora-") + QString::fromLatin1(marker);
    const PendingPairing *match = nullptr;
    for (const auto &row : rows) {
        if (row.name != name || row.sourceIpv4 != source)
            continue;
        if (match)
            return std::nullopt;
        match = &row;
    }
    if (!match || !match->sourcePort || *match->sourcePort == 0)
        return std::nullopt;
    // Sunshine approves by ID alone, so the chosen ID must identify only this row.
    if (std::count_if(rows.cbegin(), rows.cend(), [match](const PendingPairing &row) { return row.id == match->id; }) != 1)
        return std::nullopt;
    return *match;
}

bool samePending(const PendingPairing &a, const PendingPairing &b)
{
    return a.id == b.id && a.name == b.name && a.address == b.address &&
        a.sourceIpv4 == b.sourceIpv4 && a.sourcePort == b.sourcePort;
}

QString displayName(const QString &peerName)
{
    QString name;
    for (const auto ch : peerName) {
        if (ch != QLatin1Char('#') && ch.category() != QChar::Other_Control)
            name.append(ch);
        if (name.size() == 24)
            break;
    }
    if (!name.isEmpty() && name.at(name.size() - 1).isHighSurrogate())
        name.chop(1);
    return name.trimmed().isEmpty() ? QStringLiteral("Aurora") : name;
}

QByteArray randomBytes(int size)
{
    QByteArray bytes(size, '\0');
    for (int i = 0; i < size; i += int(sizeof(quint32))) {
        const auto word = QRandomGenerator::system()->generate();
        std::memcpy(bytes.data() + i, &word, size_t(std::min(size - i, int(sizeof(word)))));
    }
    return bytes;
}

QByteArray newPin()
{
    quint32 value;
    do {
        value = QRandomGenerator::system()->generate();
    } while (value >= 4294960000u);
    return QByteArray::number(value % 10000).rightJustified(4, '0');
}

template <typename Callback>
void postTo(const QPointer<EmbeddedPairingHost> &host, Callback callback)
{
    if (host)
        QMetaObject::invokeMethod(host.data(), [host, callback = std::move(callback)]() mutable {
            if (host)
                callback(*host);
        }, Qt::QueuedConnection);
}

HostSunshine lifecycleSunshine(LifecycleController &controller)
{
    const QPointer<LifecycleController> bound(&controller);
    return {
        [bound](const QString &expected) -> std::optional<AdminIdentity> {
            if (!bound || QThread::currentThread() != bound->thread())
                return std::nullopt;
            const auto state = bound->state();
            if (state.phase != LifecycleController::Phase::Ready || state.settingsSession != expected)
                return std::nullopt;
            return state.adminIdentity;
        },
        [bound](const QString &expected, HostSunshine::Admin::PendingCompletion done) {
            return bound && bound->readPendingPairings(expected, std::move(done));
        },
        [bound](const QString &expected, HostSunshine::Approval approval, HostSunshine::Admin::ApprovalCompletion done,
                HostSunshine::Admin::ApprovalStillCurrent guard) {
            return bound && bound->approvePairing(expected, std::move(approval), std::move(done), std::move(guard));
        }
    };
}

} // namespace

struct EmbeddedPairingHost::Attempt final : QObject
{
    enum class Step { Hello, Admitting, Polling, Rechecking, Confirming, Proving, Approving };

    Attempt(QTcpSocket *connection, EmbeddedPairingHost *owner)
        : QObject(owner), socket(connection), challenge(randomBytes(BridgeKeySize))
    {
        socket->setParent(this);
        elapsed.start();
    }

    QTcpSocket *socket;
    QElapsedTimer elapsed;
    Step step = Step::Hello;
    QByteArray challenge;
    QHostAddress peer;
    QHostAddress sunshineSource;
    QString peerId;
    QString label;
    QByteArray marker;
    QByteArray pin;
    std::optional<PendingPairing> selected;
    quint64 statusGeneration = 0;
    quint64 sourceRequest = 0;
    quint64 sourceGeneration = 0;
    bool readingPending = false;
    qint64 nextRead = 0;
    std::shared_ptr<std::atomic_bool> approvalGuard;
};

EmbeddedPairingHost::EmbeddedPairingHost(aurora::tailscale::EmbeddedStatusClient &client,
                                       LifecycleController &sunshine, CurrentContext currentContext, QObject *parent)
    : EmbeddedPairingHost(client, lifecycleSunshine(sunshine), std::move(currentContext), parent)
{
    connect(&sunshine, &LifecycleController::stateChanged, this, &EmbeddedPairingHost::contextChanged);
    connect(&sunshine, &QObject::destroyed, this, &EmbeddedPairingHost::stop);
}

EmbeddedPairingHost::EmbeddedPairingHost(aurora::tailscale::EmbeddedStatusClient &client, HostSunshine sunshine,
                                       CurrentContext currentContext, QObject *parent)
    : QObject(parent), client_(&client), sunshine_(std::move(sunshine)), currentContext_(std::move(currentContext))
{
    server_.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    server_.setMaxPendingConnections(8);
    tickTimer_.setInterval(50);

    connect(&tickTimer_, &QTimer::timeout, this, &EmbeddedPairingHost::tick);
    connect(&server_, &QTcpServer::newConnection, this, &EmbeddedPairingHost::acceptConnections);
    connect(&client, &aurora::tailscale::EmbeddedStatusClient::pairHostFinished, this, &EmbeddedPairingHost::leaseFinished);
    connect(&client, &aurora::tailscale::EmbeddedStatusClient::statusFinished, this, &EmbeddedPairingHost::statusFinished);
    connect(&client, &aurora::tailscale::EmbeddedStatusClient::pairSourceFinished, this, &EmbeddedPairingHost::sourceFinished);
    connect(&client, &aurora::tailscale::EmbeddedStatusClient::leasesInvalidated, this, [this] {
        if (context_)
            fail(QStringLiteral("Tailscale 自动配对入口已失效。"));
    });
    connect(&client, &QObject::destroyed, this, [this] { stop(); });
}

EmbeddedPairingHost::~EmbeddedPairingHost()
{
    reset();
}

bool EmbeddedPairingHost::start()
{
    if (QThread::currentThread() != thread() || !client_ || client_->thread() != thread() ||
        context_ || !currentContext_ || !sunshine_.identity || !sunshine_.pending || !sunshine_.approve)
        return false;
    auto context = currentContext_();
    if (!context || !isEnrolled(*context) || !adminIsCurrent(*context) ||
        (pendingApproval_ && approvalSession_ == context->sunshineSession))
        return false;

    // A retired Sunshine client can drop its completion on destruction. Its
    // revoked approval must not block a different authenticated launch.
    pendingApproval_ = 0;
    approvalSession_.clear();
    ++run_;
    rpcGeneration_ = context->helperGeneration;
    context_ = std::move(context);
    opening_.start();
    tickTimer_.start();
    setState(Phase::Checking);
    return true;
}

void EmbeddedPairingHost::contextChanged()
{
    if (context_ && !contextIsCurrent())
        stop();
}

void EmbeddedPairingHost::stop()
{
    reset();
    setState(Phase::Stopped);
}

bool EmbeddedPairingHost::adminIsCurrent(const EmbeddedHostContext &context) const
{
    const auto identity = sunshine_.identity(context.sunshineSession);
    return identity && identity->port == context.scope.adminPort &&
        identity->certificateSha256 == context.scope.certificateSha256;
}

bool EmbeddedPairingHost::contextIsCurrent() const
{
    const auto current = context_ && client_ ? currentContext_() : std::nullopt;
    return current && sameEnrollment(*context_, *current) && adminIsCurrent(*current);
}

bool EmbeddedPairingHost::listening() const
{
    return phase_ == Phase::Listening && contextIsCurrent() && lease_ && lease_->isValid();
}

bool EmbeddedPairingHost::active(const Attempt *attempt) const
{
    return attempt && pairing_ == attempt && listening() &&
        attempt->socket->state() == QAbstractSocket::ConnectedState && !attempt->socket->bytesAvailable() &&
        attempt->elapsed.elapsed() < attemptTimeoutMs;
}

void EmbeddedPairingHost::setState(Phase phase, QString error)
{
    if (phase_ == phase && error_ == error)
        return;
    phase_ = phase;
    error_ = std::move(error);
    emit stateChanged();
}

void EmbeddedPairingHost::reset()
{
    ++run_;
    if (pairing_ && pairing_->approvalGuard)
        pairing_->approvalGuard->store(false);
    tickTimer_.stop();
    server_.close();
    while (server_.hasPendingConnections()) {
        auto *socket = server_.nextPendingConnection();
        socket->abort();
        socket->deleteLater();
    }

    const auto attempts = attempts_;
    for (auto *attempt : attempts)
        finish(attempt);

    if (client_ && openingRequest_)
        client_->cancelPairing(openingRequest_);
    openingRequest_ = 0;
    pendingCompatibility_ = false;
    if (lease_)
        lease_->close();
    lease_.reset();
    key_.fill('\0');
    key_.clear();
    context_.reset();
    rpcGeneration_ = 0;
}

void EmbeddedPairingHost::fail(QString error)
{
    reset();
    setState(Phase::Failed, std::move(error));
}

void EmbeddedPairingHost::tick()
{
    if (!contextIsCurrent()) {
        stop();
        return;
    }

    if (phase_ == Phase::Checking || phase_ == Phase::Opening) {
        if (opening_.elapsed() >= startupTimeoutMs)
            fail(QStringLiteral("启动 Tailscale 自动配对入口超时。"));
        else if (phase_ == Phase::Checking && !pendingCompatibility_)
            checkSunshine();
        else if (phase_ == Phase::Opening && !openingRequest_)
            openLease();
        return;
    }
    if (!listening()) {
        fail(QStringLiteral("Tailscale 自动配对入口已失效。"));
        return;
    }

    const auto attempts = attempts_;
    for (auto *attempt : attempts) {
        const auto limit = attempt->step == Attempt::Step::Hello ? helloTimeoutMs : attemptTimeoutMs;
        if (attempt->elapsed.elapsed() >= limit)
            finish(attempt);
    }
    auto *attempt = pairing_;
    if (!attempt)
        return;
    if (!active(attempt)) {
        finish(attempt);
        return;
    }

    switch (attempt->step) {
    case Attempt::Step::Admitting:
    case Attempt::Step::Rechecking:
        if (!attempt->statusGeneration)
            requestStatus(attempt);
        break;
    case Attempt::Step::Polling:
    case Attempt::Step::Confirming:
        if (!attempt->readingPending && attempt->elapsed.elapsed() >= attempt->nextRead)
            readPending(attempt);
        break;
    case Attempt::Step::Proving:
        if (!attempt->sourceRequest)
            verifySource(attempt);
        break;
    default:
        break;
    }
}

void EmbeddedPairingHost::checkSunshine()
{
    const QPointer<EmbeddedPairingHost> self(this);
    const auto run = run_;
    pendingCompatibility_ = sunshine_.pending(context_->sunshineSession, [self, run](HostSunshine::Admin::PendingResult result) {
        postTo(self, [run, result = std::move(result)](EmbeddedPairingHost &host) {
            if (host.run_ != run || host.phase_ != Phase::Checking || !host.contextIsCurrent())
                return;
            host.pendingCompatibility_ = false;
            if (!result) {
                host.fail(QStringLiteral("内置 Sunshine 未提供自动配对所需的待确认请求。"));
                return;
            }
            if (!host.server_.listen(QHostAddress::LocalHost, 0)) {
                host.fail(QStringLiteral("无法打开本机自动配对入口。"));
                return;
            }
            host.key_ = randomBytes(BridgeKeySize);
            host.setState(Phase::Opening);
        });
    });
}

void EmbeddedPairingHost::openLease()
{
    openingRequest_ = client_->requestPairHost(rpcGeneration_, context_->account, server_.serverPort(), key_);
}

void EmbeddedPairingHost::leaseFinished(quint64, quint64 request, const aurora::tailscale::PairingLeaseResult &result)
{
    if (!openingRequest_ || request != openingRequest_)
        return;
    openingRequest_ = 0;
    if (!contextIsCurrent()) {
        if (result.lease)
            result.lease->close();
        stop();
        return;
    }
    if (result.error == StatusError::Cancelled)
        return; // A refreshed helper generation can supersede an allocation before its reply.
    if (result.error != StatusError::None || !result.lease || !result.lease->isValid() || result.lease->localPort()) {
        if (result.lease)
            result.lease->close();
        fail(QStringLiteral("无法注册 Tailscale 自动配对入口。"));
        return;
    }
    lease_ = result.lease;
    setState(Phase::Listening);
    acceptConnections();
}

void EmbeddedPairingHost::acceptConnections()
{
    if (phase_ != Phase::Listening)
        return;
    while (server_.hasPendingConnections()) {
        auto *socket = server_.nextPendingConnection();
        if (!listening() || attempts_.size() >= maxConnections ||
            socket->localAddress() != QHostAddress(QHostAddress::LocalHost) ||
            socket->peerAddress().protocol() != QAbstractSocket::IPv4Protocol || !socket->peerAddress().isLoopback()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }

        auto *attempt = new Attempt(socket, this);
        attempts_.insert(attempt);
        // Largest hello plus one extra byte to detect unexpected input.
        socket->setReadBufferSize(1 + 16 + 4 + BridgeKeySize + requestFrame().size() + 1);
        connect(socket, &QIODevice::readyRead, this, [this, attempt] { receive(attempt); });
        connect(socket, &QAbstractSocket::disconnected, this, [this, attempt] { finish(attempt); });
        if (socket->write(attempt->challenge) != BridgeKeySize)
            finish(attempt);
        else if (socket->bytesAvailable())
            receive(attempt);
    }
}

void EmbeddedPairingHost::receive(Attempt *attempt)
{
    if (!attempts_.contains(attempt))
        return;
    if (attempt->step != Attempt::Step::Hello || !listening() ||
        attempt->elapsed.elapsed() >= helloTimeoutMs) {
        finish(attempt);
        return;
    }

    auto *socket = attempt->socket;
    char family = 0;
    if (socket->peek(&family, 1) != 1)
        return;
    if (family != 4 && family != 6) {
        finish(attempt);
        return;
    }

    const int proofSize = 1 + (family == 4 ? 4 : 16) + 4 + BridgeKeySize;
    const auto request = requestFrame();
    const auto length = proofSize + request.size();
    if (socket->bytesAvailable() < length)
        return;
    if (socket->bytesAvailable() != length) {
        finish(attempt);
        return;
    }
    const auto hello = socket->read(length);
    const auto source = verifyEmbeddedSources(hello.left(proofSize), key_, attempt->challenge);
    attempt->challenge.fill('\0');
    if (!source || hello.mid(proofSize) != request || !tailnetIpv4(source->peer) ||
        source->sunshineSource != QHostAddress(QHostAddress::LocalHost) || source->peer == context_->localAddress) {
        finish(attempt);
        return;
    }
    if (pairing_ || pendingApproval_) {
        finish(attempt, true);
        return;
    }
    pairing_ = attempt;
    attempt->peer = source->peer;
    attempt->sunshineSource = source->sunshineSource;
    attempt->step = Attempt::Step::Admitting;
    requestStatus(attempt);
}

void EmbeddedPairingHost::requestStatus(Attempt *attempt)
{
    if (client_->requestStatus(rpcGeneration_))
        attempt->statusGeneration = rpcGeneration_;
}

void EmbeddedPairingHost::statusFinished(quint64 generation, const aurora::tailscale::StatusResult &result)
{
    if (!context_ || generation < rpcGeneration_)
        return;
    if (!contextIsCurrent()) {
        stop();
        return;
    }
    if (result.error != StatusError::None || !result.status || !sameAccount(*result.status, *context_)) {
        fail(QStringLiteral("已验证的 Tailscale 自动配对账号发生变化。"));
        return;
    }
    // A status refresh changes the helper RPC generation, not the user's account epoch.
    rpcGeneration_ = generation;
    auto *attempt = pairing_;
    if (!attempt)
        return;
    const auto *peer = uniquePeer(*result.status, attempt->peer);
    if (!attempt->peerId.isEmpty() && (!peer || peer->nodeId != attempt->peerId)) {
        finish(attempt);
        return;
    }
    if (generation != attempt->statusGeneration)
        return;
    attempt->statusGeneration = 0;
    if (!active(attempt) || !peer) {
        finish(attempt, attempt->step == Attempt::Step::Admitting);
        return;
    }

    attempt->label = displayName(peer->name);
    if (attempt->step == Attempt::Step::Admitting) {
        attempt->peerId = peer->nodeId;
        attempt->marker = randomBytes(16).toHex();
        attempt->pin = newPin();
        const auto offer = QByteArray(1, '\1') + attempt->marker + attempt->pin;
        if (attempt->socket->write(offer) != offer.size()) {
            finish(attempt);
            return;
        }
        attempt->step = Attempt::Step::Polling;
        readPending(attempt);
    } else if (attempt->step == Attempt::Step::Rechecking) {
        attempt->step = Attempt::Step::Confirming;
        attempt->nextRead = 0;
        readPending(attempt);
    }
}

void EmbeddedPairingHost::readPending(Attempt *attempt)
{
    const QPointer<EmbeddedPairingHost> self(this);
    const QPointer<Attempt> waiting(attempt);
    attempt->readingPending = sunshine_.pending(context_->sunshineSession,
        [self, waiting](HostSunshine::Admin::PendingResult result) {
            postTo(self, [waiting, result = std::move(result)](EmbeddedPairingHost &host) mutable {
                if (waiting && host.active(waiting))
                    host.pendingFinished(waiting, std::move(result));
            });
        });
}

void EmbeddedPairingHost::pendingFinished(Attempt *attempt, HostSunshine::Admin::PendingResult result)
{
    attempt->readingPending = false;
    if (!result) {
        finish(attempt);
        return;
    }
    const auto found = singlePending(result.pairings, attempt->sunshineSource, attempt->marker);
    if (attempt->step == Attempt::Step::Polling) {
        if (!found) {
            attempt->nextRead = attempt->elapsed.elapsed() + pendingPollMs;
            return;
        }
        attempt->selected = found;
        attempt->step = Attempt::Step::Rechecking;
        requestStatus(attempt);
    } else if (attempt->step == Attempt::Step::Confirming) {
        if (!found || !samePending(*attempt->selected, *found)) {
            finish(attempt);
            return;
        }
        attempt->step = Attempt::Step::Proving;
        verifySource(attempt);
    }
}

void EmbeddedPairingHost::verifySource(Attempt *attempt)
{
    attempt->sourceGeneration = rpcGeneration_;
    attempt->sourceRequest = client_->requestPairSource(rpcGeneration_, context_->account, attempt->peerId,
                                                       attempt->peer, *attempt->selected->sourcePort);
}

void EmbeddedPairingHost::sourceFinished(quint64 generation, quint64 request, const aurora::tailscale::PairSourceResult &result)
{
    auto *attempt = pairing_;
    if (!attempt || attempt->step != Attempt::Step::Proving || request != attempt->sourceRequest)
        return;
    attempt->sourceRequest = 0;
    if (!active(attempt) || generation != attempt->sourceGeneration || generation != rpcGeneration_ ||
        result.error != StatusError::None || !result.verified) {
        finish(attempt);
        return;
    }
    approve(attempt);
}

void EmbeddedPairingHost::approve(Attempt *attempt)
{
    attempt->step = Attempt::Step::Approving;
    attempt->approvalGuard = std::make_shared<std::atomic_bool>(true);
    const auto id = ++nextApproval_;
    pendingApproval_ = id;
    approvalSession_ = context_->sunshineSession;
    const QPointer<EmbeddedPairingHost> self(this);
    const QPointer<Attempt> waiting(attempt);
    const auto expires = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(attemptTimeoutMs - attempt->elapsed.elapsed());
    const bool submitted = sunshine_.approve(context_->sunshineSession,
        HostSunshine::Approval::authorized(attempt->selected->id, QString::fromLatin1(attempt->pin), attempt->label),
        [self, waiting, id](HostSunshine::Admin::ApprovalResult) {
            postTo(self, [waiting, id](EmbeddedPairingHost &host) {
                if (host.pendingApproval_ != id)
                    return;
                host.pendingApproval_ = 0;
                host.approvalSession_.clear();
                if (waiting && host.pairing_ == waiting)
                    host.finish(waiting);
            });
        }, [guard = attempt->approvalGuard, expires] {
            return guard->load() && std::chrono::steady_clock::now() < expires;
        });
    attempt->pin.fill('\0');
    if (!submitted) {
        pendingApproval_ = 0;
        approvalSession_.clear();
        finish(attempt);
    }
}

void EmbeddedPairingHost::finish(Attempt *attempt, bool reject)
{
    if (!attempts_.remove(attempt))
        return;
    if (attempt->approvalGuard)
        attempt->approvalGuard->store(false);
    if (pairing_ == attempt)
        pairing_ = nullptr;
    if (client_ && attempt->sourceRequest)
        client_->cancelPairing(attempt->sourceRequest);
    attempt->challenge.fill('\0');
    attempt->pin.fill('\0');

    auto *socket = attempt->socket;
    disconnect(socket, nullptr, this, nullptr);
    if (reject && socket->state() == QAbstractSocket::ConnectedState && socket->write(QByteArray(1, '\0')) == 1) {
        connect(socket, &QAbstractSocket::disconnected, attempt, &QObject::deleteLater);
        QTimer::singleShot(1000, attempt, &QObject::deleteLater);
        socket->disconnectFromHost();
        return;
    }
    socket->abort();
    attempt->deleteLater();
}

} // namespace Aurora::Pairing
