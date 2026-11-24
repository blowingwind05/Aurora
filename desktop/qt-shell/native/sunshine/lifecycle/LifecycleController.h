// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledAdmin.h"
#include "BundledHost.h"
#include "PortPreflight.h"

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
    // Completion belongs to the worker thread. Destruction cancels a pending read.
    virtual bool readConfig(BundledAdmin::Completion completion) = 0;
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

signals:
    void stateChanged();
    void shutdownFinished();

private:
    friend class LifecycleWorker;
    enum class Action { Start, Restart, Stop, Shutdown };

    bool acceptsRequests() const;
    void submit(Action action, Phase phase);
    void enqueue(Action action, quint64 request);
    void setState(State state);

    std::shared_ptr<std::atomic<quint64>> latest_;
    LifecycleWorker *worker_ = nullptr;
    State state_;
    bool closing_ = false;
};

} // namespace Aurora::Sunshine
