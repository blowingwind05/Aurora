// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledAdmin.h"
#include "EmbeddedStatusClient.h"
#include "Enrollment.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

namespace Aurora::Sunshine {
class LifecycleController;
}

namespace Aurora::Pairing {

struct EmbeddedHostContext
{
    EnrollmentResult enrollment = EnrollmentResult::Unavailable;
    ApprovalScope scope{};
    aurora::tailscale::AccountIdentity account{};
    QHostAddress localAddress;
    quint64 accountEpoch = 0;
    quint64 helperGeneration = 0;
    QString sunshineSession;
};

struct HostSunshine
{
    using Admin = Aurora::Sunshine::BundledAdmin;
    using Approval = Aurora::Sunshine::SupervisorPairingApproval;

    // All calls and completions use the host thread. Only the approval guard
    // can run on the Sunshine worker.
    std::function<std::optional<Aurora::Sunshine::AdminIdentity>(const QString &)> identity;
    std::function<bool(const QString &, Admin::PendingCompletion)> pending;
    std::function<bool(const QString &, Approval, Admin::ApprovalCompletion, Admin::ApprovalStillCurrent)> approve;
};

class EmbeddedPairingHost final : public QObject
{
    Q_OBJECT

public:
    enum class Phase { Stopped, Checking, Opening, Listening, Failed };
    Q_ENUM(Phase)

    using CurrentContext = std::function<std::optional<EmbeddedHostContext>()>;

    EmbeddedPairingHost(aurora::tailscale::EmbeddedStatusClient &client,
                        Aurora::Sunshine::LifecycleController &sunshine,
                        CurrentContext currentContext, QObject *parent = nullptr);
    EmbeddedPairingHost(aurora::tailscale::EmbeddedStatusClient &client, HostSunshine sunshine,
                        CurrentContext currentContext, QObject *parent = nullptr);
    ~EmbeddedPairingHost() override;

    // The parent supplies the current enrolled account and authenticated Sunshine session.
    // Start is explicit. Call contextChanged synchronously on revocation or epoch change.
    bool start();
    void contextChanged();
    void stop();

    Phase phase() const { return phase_; }
    const QString &error() const { return error_; }

signals:
    void stateChanged();

private:
    struct Attempt;

    bool adminIsCurrent(const EmbeddedHostContext &context) const;
    bool contextIsCurrent() const;
    bool listening() const;
    bool active(const Attempt *attempt) const;
    void setState(Phase phase, QString error = {});
    void reset();
    void fail(QString error);
    void tick();
    void checkSunshine();
    void openLease();
    void leaseFinished(quint64 generation, quint64 request, const aurora::tailscale::PairingLeaseResult &result);
    void acceptConnections();
    void receive(Attempt *attempt);
    void requestStatus(Attempt *attempt);
    void statusFinished(quint64 generation, const aurora::tailscale::StatusResult &result);
    void readPending(Attempt *attempt);
    void pendingFinished(Attempt *attempt, HostSunshine::Admin::PendingResult result);
    void verifySource(Attempt *attempt);
    void sourceFinished(quint64 generation, quint64 request, const aurora::tailscale::PairSourceResult &result);
    void approve(Attempt *attempt);
    void finish(Attempt *attempt, bool reject = false);
    void finishWithReply(Attempt *attempt, const QByteArray &reply);

    QPointer<aurora::tailscale::EmbeddedStatusClient> client_;
    HostSunshine sunshine_;
    CurrentContext currentContext_;
    std::optional<EmbeddedHostContext> context_;
    QTcpServer server_{this};
    QTimer tickTimer_{this};
    QElapsedTimer opening_;
    Phase phase_ = Phase::Stopped;
    QString error_;
    QByteArray key_;
    quint64 run_ = 0;
    quint64 openingRequest_ = 0;
    quint64 rpcGeneration_ = 0;
    bool pendingCompatibility_ = false;
    bool supportsClientGrant_ = false;
    std::shared_ptr<aurora::tailscale::PairingLease> lease_;
    QSet<Attempt *> attempts_;
    Attempt *pairing_ = nullptr;
    quint64 nextApproval_ = 0;
    quint64 pendingApproval_ = 0;
    QString approvalSession_;
};

} // namespace Aurora::Pairing
