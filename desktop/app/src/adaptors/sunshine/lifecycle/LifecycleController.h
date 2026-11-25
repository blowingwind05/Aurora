// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledAdmin.h"
#include "BundledHost.h"
#include "PortPreflight.h"
#include "SunshineSettings.h"

#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

namespace Aurora::Sunshine {

class LifecycleWorker;

class ReadinessAdmin
{
public:
    virtual ~ReadinessAdmin() = default;
    virtual AdminIdentity identity() const { return {}; }
    // Completions belong to the worker thread. Destruction cancels pending work.
    virtual bool readConfig(BundledAdmin::Completion completion) = 0;
    virtual bool saveConfig(BundledAdmin::PreparePayload, BundledAdmin::VerifyReadback, BundledAdmin::Completion)
    {
        return false;
    }
    virtual bool readPendingPairings(BundledAdmin::PendingCompletion) { return false; }
    virtual bool approvePairing(SupervisorPairingApproval, BundledAdmin::ApprovalCompletion,
                                BundledAdmin::ApprovalStillCurrent) { return false; }
};

struct LifecycleDependencies
{
    std::function<std::unique_ptr<HostProcess>()> makeProcess;
    std::function<PortCheck(HostPlatform, const PortFamily &)> checkPortOwnership;
    std::function<std::unique_ptr<ReadinessAdmin>(const QString &, const QByteArray &, QString &)> openAdmin;
};

struct LifecycleTiming
{
    int startupTimeoutMs = 60000;
    int pollMs = 200;
    int automaticRestartCooldownMs = 30000;
};

class LifecycleController final : public QObject
{
    Q_OBJECT

public:
    enum class Phase { Stopped, Starting, Ready, Restarting, Stopping, Failed };
    Q_ENUM(Phase)

    struct State
    {
        quint64 request = 0;
        Phase phase = Phase::Stopped;
        QString error;
        bool cleanupPending = false;
        QString settingsSession;
        AdminIdentity adminIdentity;
    };

    explicit LifecycleController(HostLayout layout, QObject *parent = nullptr);
    LifecycleController(HostLayout layout, LifecycleDependencies dependencies, LifecycleTiming timing,
                        QObject *parent = nullptr);
    ~LifecycleController() override;

    // All calls belong to the controller's Qt event-loop thread.
    State state() const { return state_; }
    bool start();
    bool restart();
    bool stop();
    bool shutdown();

    // Accepts one settings operation at a time while Ready. An accepted operation
    // completes asynchronously on this thread while the controller lives.
    bool adminBusy() const { return bool(pendingAdmin_); }
    bool settingsBusy() const { return adminBusy(); }
    bool readSettings(SettingsReadCompletion completion);
    bool saveSettings(SettingsEdit edit, SettingsSaveCompletion completion);

    // Pass the authenticated session captured by the coordinator for this request.
    bool readPendingPairings(const QString &session, BundledAdmin::PendingCompletion completion);
    // The optional guard must safely read the caller's authorization on the worker thread.
    bool approvePairing(const QString &session, SupervisorPairingApproval approval,
                        BundledAdmin::ApprovalCompletion completion, BundledAdmin::ApprovalStillCurrent stillCurrent = {});

signals:
    void stateChanged();
    void shutdownFinished();

private:
    struct PendingAdmin;
    friend class LifecycleWorker;
    enum class Action { Start, Restart, Stop, Shutdown };

    bool acceptsRequests() const;
    void submit(Action action, Phase phase);
    void enqueue(Action action, quint64 request);
    void setState(State state);

    std::shared_ptr<std::atomic<quint64>> latest_;
    LifecycleWorker *worker_ = nullptr;
    State state_;
    std::unique_ptr<PendingAdmin> pendingAdmin_;
    quint64 nextAdminOperation_ = 0;
    bool closing_ = false;
};

} // namespace Aurora::Sunshine

Q_DECLARE_METATYPE(Aurora::Sunshine::BundledAdmin::PendingResult)
Q_DECLARE_METATYPE(Aurora::Sunshine::BundledAdmin::ApprovalResult)
