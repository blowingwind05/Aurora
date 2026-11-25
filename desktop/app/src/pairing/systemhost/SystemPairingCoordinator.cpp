// SPDX-License-Identifier: GPL-3.0-only
#include "SystemPairingCoordinator.h"

#include "HostPairingOffer.h"
#include "LifecycleController.h"

#include <QThread>

#include <utility>

namespace Aurora::Pairing {
namespace {

using Sunshine::BundledAdmin;
using Sunshine::LifecycleController;
using Sunshine::PendingPairing;
using Sunshine::SupervisorPairingApproval;

constexpr int preflightMs = 10000;
constexpr int pendingPollMs = 250;

bool isEnrolled(const SystemPairingContext &context)
{
    return context.enrollment == EnrollmentResult::Approved && context.scope.provider == Provider::System &&
        context.scope.owner == context.account.ownerId && context.scope.localNode == context.account.nodeId &&
        !approvalId(context.scope).isEmpty() && !context.account.tailnetId.isEmpty() &&
        !context.localAddress.isNull() && context.localAddress.scopeId().isEmpty() &&
        context.accountEpoch && !context.sunshineSession.isEmpty();
}

bool sameEnrollment(const SystemPairingContext &a, const SystemPairingContext &b)
{
    return isEnrolled(b) && a.accountEpoch == b.accountEpoch && a.account == b.account &&
        a.localAddress == b.localAddress && a.sunshineSession == b.sunshineSession &&
        a.scope.adminPort == b.scope.adminPort && a.scope.certificateSha256 == b.scope.certificateSha256;
}

bool sameSource(const PendingPairing &row, const QHostAddress &source)
{
    if (source.protocol() == QAbstractSocket::IPv4Protocol)
        return row.sourceIpv4 == source;
    // The pinned admin decoder normalizes mapped IPv4 into sourceIpv4. The
    // system provider also accepts the full IPv6 tailnet address from PROXYv2.
    const QHostAddress address(row.address);
    return address.protocol() == QAbstractSocket::IPv6Protocol &&
        address.scopeId().isEmpty() && address == source;
}

QString transportError(SystemHostStop reason)
{
    switch (reason) {
    case SystemHostStop::PortOccupied:
        return QStringLiteral("Tailscale 的自动配对端口已被占用，请检查现有转发配置。");
    case SystemHostStop::PortCheckFailed:
        return QStringLiteral("读取 Tailscale 转发配置失败。");
    case SystemHostStop::IdentityMismatch:
        return QStringLiteral("已验证的 Tailscale 自动配对账号发生变化。");
    case SystemHostStop::StartupTimeout:
        return QStringLiteral("启动 Tailscale 自动配对入口超时。");
    case SystemHostStop::ListenFailed:
        return QStringLiteral("无法打开本机自动配对入口。");
    default:
        return QStringLiteral("无法启动或保持 Tailscale 自动配对入口。");
    }
}

} // namespace

struct SystemPairingCoordinator::Attempt
{
    enum class Step { Polling, Confirming, Revalidating, Approving };

    quint64 connection;
    QHostAddress source;
    QString label;
    QByteArray marker;
    QByteArray pin;
    Step step = Step::Polling;
    std::optional<PendingPairing> selected;

    ~Attempt() { pin.fill('\0'); }
};

SystemPairingCoordinator::SystemPairingCoordinator(LifecycleController &sunshine, CurrentContext currentContext,
    QObject *parent)
    : SystemPairingCoordinator(sunshine, std::move(currentContext), std::make_unique<SystemPairingHost>(), parent)
{
}

SystemPairingCoordinator::SystemPairingCoordinator(LifecycleController &sunshine, CurrentContext currentContext,
    SystemPairingHost::Locate locate, SystemPairingHost::FindOwner findOwner, QObject *parent)
    : SystemPairingCoordinator(sunshine, std::move(currentContext),
        std::make_unique<SystemPairingHost>(std::move(locate), std::move(findOwner)), parent)
{
}

SystemPairingCoordinator::SystemPairingCoordinator(LifecycleController &sunshine, CurrentContext currentContext,
    std::unique_ptr<SystemPairingHost> transport, QObject *parent)
    : QObject(parent), sunshine_(&sunshine), currentContext_(std::move(currentContext)), transport_(std::move(transport))
{
    preflightTimer_.setSingleShot(true);
    preflightTimer_.setTimerType(Qt::PreciseTimer);
    pollTimer_.setSingleShot(true);
    connect(&preflightTimer_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("无法及时读取内置 Sunshine 的待确认配对请求。"));
    });
    connect(&pollTimer_, &QTimer::timeout, this, &SystemPairingCoordinator::pollPending);
    connect(&sunshine, &LifecycleController::stateChanged, this, &SystemPairingCoordinator::contextChanged);
    connect(&sunshine, &QObject::destroyed, this, &SystemPairingCoordinator::stop);
    connect(transport_.get(), &SystemPairingHost::listening, this, [this](quint64 host, quint64 generation) {
        if (host == host_ && checkContext() && generation == context_->accountEpoch)
            setPhase(Phase::Listening);
    });
    connect(transport_.get(), &SystemPairingHost::authenticated, this, &SystemPairingCoordinator::authenticated);
    connect(transport_.get(), &SystemPairingHost::revalidated, this, &SystemPairingCoordinator::revalidated);
    connect(transport_.get(), &SystemPairingHost::connectionClosed, this, [this](quint64 host, quint64 connection) {
        if (host == host_ && attempt_ && attempt_->connection == connection) {
            pollTimer_.stop();
            attempt_.reset();
        }
    });
    connect(transport_.get(), &SystemPairingHost::stopped, this, [this](quint64 host, quint64, SystemHostStop why) {
        if (host == host_)
            fail(transportError(why));
    });
}

SystemPairingCoordinator::~SystemPairingCoordinator()
{
    reset();
}

bool SystemPairingCoordinator::start()
{
    if (QThread::currentThread() != thread() || !sunshine_ || sunshine_->thread() != thread() ||
        context_ || !currentContext_)
        return false;

    auto context = currentContext_();
    if (!context || !isEnrolled(*context) || !adminIsCurrent(*context) ||
        (pendingApproval_ && approvalSession_ == context->sunshineSession))
        return false;

    // A retired session's completion can be discarded by the lifecycle.
    pendingApproval_ = 0;
    approvalSession_.clear();
    context_ = std::move(context);
    preflightTimer_.start(preflightMs);
    pollTimer_.start(0);
    setPhase(Phase::Checking);
    return true;
}

bool SystemPairingCoordinator::adminIsCurrent(const SystemPairingContext &context) const
{
    if (!sunshine_)
        return false;
    const auto state = sunshine_->state();
    return state.phase == LifecycleController::Phase::Ready && state.settingsSession == context.sunshineSession &&
        state.adminIdentity.port == context.scope.adminPort &&
        state.adminIdentity.certificateSha256 == context.scope.certificateSha256;
}

bool SystemPairingCoordinator::contextIsCurrent() const
{
    const auto current = context_ && sunshine_ ? currentContext_() : std::nullopt;
    return current && sameEnrollment(*context_, *current) && adminIsCurrent(*current);
}

bool SystemPairingCoordinator::checkContext()
{
    if (contextIsCurrent())
        return true;
    if (context_)
        stop();
    return false;
}

void SystemPairingCoordinator::contextChanged()
{
    checkContext();
}

void SystemPairingCoordinator::stop()
{
    reset();
    setPhase(Phase::Stopped);
}

void SystemPairingCoordinator::setPhase(Phase phase, QString error)
{
    if (phase_ == phase && error_ == error)
        return;
    phase_ = phase;
    error_ = std::move(error);
    emit stateChanged();
}

void SystemPairingCoordinator::reset()
{
    ++run_;
    host_ = 0;
    if (context_)
        transport_->setAuthorization(context_->accountEpoch, std::nullopt);
    preflightTimer_.stop();
    pollTimer_.stop();
    attempt_.reset();
    context_.reset();
}

void SystemPairingCoordinator::fail(QString error)
{
    reset();
    setPhase(Phase::Failed, std::move(error));
}

void SystemPairingCoordinator::pollPending()
{
    if (!checkContext())
        return;
    const auto connection = attempt_ ? attempt_->connection : 0;
    const QPointer<SystemPairingCoordinator> self(this);
    const bool scheduled = sunshine_->readPendingPairings(context_->sunshineSession,
        [self, run = run_, connection](PendingResult result) {
            if (self && self->run_ == run)
                self->pendingFinished(connection, std::move(result));
        });
    if (!scheduled)
        pollTimer_.start(pendingPollMs);
}

void SystemPairingCoordinator::pendingFinished(quint64 connection, PendingResult result)
{
    if (!checkContext())
        return;
    if (!connection) {
        if (phase_ != Phase::Checking)
            return;
        if (result)
            openTransport();
        else
            fail(QStringLiteral("内置 Sunshine 未提供自动配对所需的待确认请求。"));
        return;
    }
    if (!attempt_ || attempt_->connection != connection)
        return;
    if (!result) {
        finishAttempt();
        return;
    }

    const auto row = HostOffer::singlePending(result.pairings, attempt_->marker, [this](const PendingPairing &row) {
        return sameSource(row, attempt_->source);
    });
    if (attempt_->step == Attempt::Step::Polling) {
        if (!row) {
            pollTimer_.start(pendingPollMs);
            return;
        }
        attempt_->selected = row;
        attempt_->step = Attempt::Step::Confirming;
        pollPending();
    } else if (attempt_->step == Attempt::Step::Confirming) {
        if (!row || !HostOffer::samePending(*attempt_->selected, *row)) {
            finishAttempt();
            return;
        }
        attempt_->step = Attempt::Step::Revalidating;
        if (!transport_->revalidate(host_, connection))
            finishAttempt();
    }
}

void SystemPairingCoordinator::openTransport()
{
    preflightTimer_.stop();
    transport_->setAuthorization(context_->accountEpoch, SystemHostAccount{context_->account, context_->localAddress});
    host_ = transport_->start(context_->accountEpoch);
    if (host_)
        setPhase(Phase::Opening);
    else
        fail(QStringLiteral("无法启动 Tailscale 自动配对入口。"));
}

void SystemPairingCoordinator::authenticated(quint64 host, quint64 generation, quint64 connection,
    const SystemHostPeer &peer, QTcpSocket *channel)
{
    if (host != host_ || !checkContext() || generation != context_->accountEpoch)
        return;
    if (pendingApproval_ || attempt_) {
        channel->write(QByteArray(1, '\0'));
        channel->disconnectFromHost();
        return;
    }

    attempt_.reset(new Attempt{connection, peer.address, HostOffer::displayName(peer.name),
        HostOffer::randomBytes(16).toHex(), HostOffer::newPin()});
    const auto offer = HostOffer::frame(attempt_->marker, attempt_->pin);
    if (channel->write(offer) == offer.size())
        pollPending();
    else
        finishAttempt();
}

void SystemPairingCoordinator::revalidated(quint64 host, quint64 generation, quint64 connection)
{
    if (host != host_ || !checkContext() || generation != context_->accountEpoch ||
        !attempt_ || attempt_->connection != connection || attempt_->step != Attempt::Step::Revalidating)
        return;
    auto guard = transport_->approvalGuard(host, connection);
    if (!guard || !guard()) {
        finishAttempt();
        return;
    }

    attempt_->step = Attempt::Step::Approving;
    auto approval = SupervisorPairingApproval::authorized(attempt_->selected->id,
        QString::fromLatin1(attempt_->pin), attempt_->label);
    attempt_->pin.fill('\0');

    pendingApproval_ = connection;
    approvalSession_ = context_->sunshineSession;
    const QPointer<SystemPairingCoordinator> self(this);
    const bool submitted = sunshine_->approvePairing(approvalSession_, std::move(approval),
        [self, connection](BundledAdmin::ApprovalResult) {
            if (!self || self->pendingApproval_ != connection)
                return;
            self->pendingApproval_ = 0;
            self->approvalSession_.clear();
            if (self->attempt_ && self->attempt_->connection == connection)
                self->finishAttempt();
        }, std::move(guard));
    if (!submitted) {
        pendingApproval_ = 0;
        approvalSession_.clear();
        finishAttempt();
    }
}

void SystemPairingCoordinator::finishAttempt()
{
    pollTimer_.stop();
    if (auto attempt = std::move(attempt_))
        transport_->closeConnection(host_, attempt->connection);
}

} // namespace Aurora::Pairing
