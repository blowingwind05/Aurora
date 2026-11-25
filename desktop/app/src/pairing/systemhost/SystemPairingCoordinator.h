// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledAdmin.h"
#include "Enrollment.h"
#include "SystemPairingHost.h"

#include <QPointer>
#include <QTimer>

namespace Aurora::Sunshine {
class LifecycleController;
}

namespace Aurora::Pairing {

struct SystemPairingContext
{
    EnrollmentResult enrollment = EnrollmentResult::Unavailable;
    ApprovalScope scope{};
    aurora::tailscale::AccountIdentity account{};
    QHostAddress localAddress;
    quint64 accountEpoch = 0;
    QString sunshineSession;
};

class SystemPairingCoordinator final : public QObject
{
    Q_OBJECT

public:
    enum class Phase { Stopped, Checking, Opening, Listening, Failed };
    Q_ENUM(Phase)

    using CurrentContext = std::function<std::optional<SystemPairingContext>()>;

    SystemPairingCoordinator(Sunshine::LifecycleController &sunshine, CurrentContext currentContext,
        QObject *parent = nullptr);
    SystemPairingCoordinator(Sunshine::LifecycleController &sunshine, CurrentContext currentContext,
        SystemPairingHost::Locate locate, SystemPairingHost::FindOwner findOwner, QObject *parent = nullptr);
    ~SystemPairingCoordinator() override;

    // Use on the shared Qt event-loop thread. The parent owns enrollment and
    // must call contextChanged synchronously when its authorization changes.
    bool start();
    void contextChanged();
    void stop();

    Phase phase() const { return phase_; }
    const QString &error() const { return error_; }

signals:
    void stateChanged();

private:
    struct Attempt;
    using PendingResult = Sunshine::BundledAdmin::PendingResult;

    SystemPairingCoordinator(Sunshine::LifecycleController &sunshine, CurrentContext currentContext,
        std::unique_ptr<SystemPairingHost> transport, QObject *parent);
    bool adminIsCurrent(const SystemPairingContext &context) const;
    bool contextIsCurrent() const;
    bool checkContext();
    void setPhase(Phase phase, QString error = {});
    void reset();
    void fail(QString error);
    void pollPending();
    void pendingFinished(quint64 connection, PendingResult result);
    void openTransport();
    void authenticated(quint64 host, quint64 generation, quint64 connection,
        const SystemHostPeer &peer, QTcpSocket *channel);
    void revalidated(quint64 host, quint64 generation, quint64 connection);
    void finishAttempt();

    const QPointer<Sunshine::LifecycleController> sunshine_;
    const CurrentContext currentContext_;
    const std::unique_ptr<SystemPairingHost> transport_;
    std::optional<SystemPairingContext> context_;
    std::unique_ptr<Attempt> attempt_;
    QTimer preflightTimer_;
    QTimer pollTimer_;
    Phase phase_ = Phase::Stopped;
    QString error_;
    quint64 run_ = 0;
    quint64 host_ = 0;
    quint64 pendingApproval_ = 0;
    bool supportsClientGrant_ = false;
    QString approvalSession_;
};

} // namespace Aurora::Pairing
