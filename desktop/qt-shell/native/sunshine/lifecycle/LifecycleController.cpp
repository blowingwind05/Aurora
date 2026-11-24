// SPDX-License-Identifier: GPL-3.0-only
#include "LifecycleController.h"
#include "SettingsSession.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace Aurora::Sunshine {
namespace {

using Phase = LifecycleController::Phase;

class BundledReadinessAdmin final : public ReadinessAdmin
{
public:
    explicit BundledReadinessAdmin(std::unique_ptr<BundledAdmin> admin) : admin_(std::move(admin)) {}

    bool readConfig(BundledAdmin::Completion completion) override
    {
        return admin_->readConfig(std::move(completion));
    }

    bool saveConfig(BundledAdmin::PreparePayload prepare, BundledAdmin::VerifyReadback verify,
                    BundledAdmin::Completion completion) override
    {
        return admin_->saveConfig(std::move(prepare), std::move(verify), std::move(completion));
    }

private:
    std::unique_ptr<BundledAdmin> admin_;
};

std::unique_ptr<ReadinessAdmin> openPinnedAdmin(const QString &profile, const QByteArray &password, QString &error)
{
    auto admin = BundledAdmin::open(profile, password, error);
    if (!admin)
        return {};
    return std::make_unique<BundledReadinessAdmin>(std::move(admin));
}

bool canRetry(BundledAdmin::Error error)
{
    using Error = BundledAdmin::Error;
    return error == Error::Network || error == Error::Tls || error == Error::Timeout;
}

bool lostAuthentication(BundledAdmin::Error error)
{
    return error == BundledAdmin::Error::Unauthorized || error == BundledAdmin::Error::Tls;
}

} // namespace

struct LifecycleController::PendingSettings
{
    quint64 operation = 0;
    QString session;
    std::optional<SettingsEdit> edit;
    SettingsReadCompletion read;
    SettingsSaveCompletion save;

    void cancel()
    {
        if (edit)
            save(rejectedSettingsEdit(*edit, SettingsStatus::Cancelled, cancelledSettingsMessage()));
        else
            read({session, SettingsStatus::Cancelled, cancelledSettingsMessage(), std::nullopt});
    }
};

class LifecycleWorker final : public QObject
{
    Q_OBJECT

public:
    LifecycleWorker(HostLayout layout, LifecycleDependencies dependencies, LifecycleTiming timing,
                    std::shared_ptr<std::atomic<quint64>> latest)
        : layout_(std::move(layout)), dependencies_(std::move(dependencies)), timing_(timing), latest_(std::move(latest))
    {
        timing_.pollMs = std::max(1, timing_.pollMs);
        timing_.startupTimeoutMs = std::max(1, timing_.startupTimeoutMs);
        timing_.automaticRestartCooldownMs = std::max(0, timing_.automaticRestartCooldownMs);
        if (!dependencies_.checkPortOwnership)
            dependencies_.checkPortOwnership = checkPorts;
        if (!dependencies_.openAdmin)
            dependencies_.openAdmin = openPinnedAdmin;
        timer_.setInterval(timing_.pollMs);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    }

    void handle(LifecycleController::Action action, quint64 request)
    {
        using Action = LifecycleController::Action;
        if (latest_->load() != request)
            return;
        request_ = request;
        lastAutomaticRestart_.invalidate();

        if (action == Action::Start || action == Action::Restart) {
            beginStart(action == Action::Start ? Phase::Starting : Phase::Restarting);
            return;
        }

        QString error;
        const bool stopped = cleanUp(error);
        publish(stopped ? Phase::Stopped : Phase::Failed, error, !stopped);
        if (action == Action::Shutdown) {
            // Dispose the backend on its own thread, including its final process/lease cleanup.
            host_.reset();
            QThread::currentThread()->quit();
        }
    }

    void readSettings(quint64 request, quint64 operation, const QString &session)
    {
        if (!settingsCurrent(request, session)) {
            emit settingsReadFinished(operation, {session, SettingsStatus::Stale, staleSettingsMessage(), std::nullopt});
            return;
        }
        settings_->read([this, request, operation, session](SettingsReadResult result, BundledAdmin::Error error) {
            QMetaObject::invokeMethod(this, [this, request, operation, session, result = std::move(result), error] {
                if (!settingsCurrent(request, session))
                    return;
                emit settingsReadFinished(operation, result);
                if (lostAuthentication(error)) {
                    publish(Phase::Stopping);
                    fail(result.error);
                }
            }, Qt::QueuedConnection);
        });
    }

    void saveSettings(quint64 request, quint64 operation, const SettingsEdit &edit)
    {
        const auto session = edit.version.session;
        if (!settingsCurrent(request, session)) {
            emit settingsSaveFinished(operation, rejectedSettingsEdit(edit, SettingsStatus::Stale, staleSettingsMessage()));
            return;
        }
        settings_->save(edit, [this, request, operation, session](SettingsSaveResult result, BundledAdmin::Error error) {
            QMetaObject::invokeMethod(this, [this, request, operation, session, result = std::move(result), error] {
                if (!settingsCurrent(request, session))
                    return;
                emit settingsSaveFinished(operation, result);
                if (lostAuthentication(error)) {
                    publish(Phase::Stopping);
                    fail(result.error);
                }
            }, Qt::QueuedConnection);
        });
    }

signals:
    void progressed(quint64 request, int phase, const QString &error, bool cleanupPending, const QString &settingsSession);
    void settingsReadFinished(quint64 operation, const Aurora::Sunshine::SettingsReadResult &result);
    void settingsSaveFinished(quint64 operation, const Aurora::Sunshine::SettingsSaveResult &result);

private:
    bool current() const { return latest_->load() == request_; }

    bool settingsCurrent(quint64 request, const QString &session)
    {
        return current() && request == request_ && phase_ == Phase::Ready && settings_ &&
            settings_->id() == session && host_->running();
    }

    void publish(Phase phase, const QString &error = {}, bool cleanupPending = false)
    {
        phase_ = phase;
        if (current())
            emit progressed(request_, int(phase), error, cleanupPending,
                phase == Phase::Ready && settings_ ? settings_->id() : QString{});
    }

    void cancelRead()
    {
        ++readId_;
        settings_.reset();
        admin_.reset();
    }

    bool cleanUp(QString &error)
    {
        timer_.stop();
        cancelRead();
        if (host_ && !host_->stop(error)) {
            if (error.isEmpty())
                error = QStringLiteral("内置 Sunshine 进程仍在退出。");
            return false;
        }
        host_.reset();
        return true;
    }

    void fail(QString error)
    {
        QString cleanupError;
        const bool stopped = cleanUp(cleanupError);
        if (!cleanupError.isEmpty())
            error += QLatin1Char(' ') + cleanupError;
        publish(Phase::Failed, error, !stopped);
    }

    void beginStart(Phase phase)
    {
        QString error;
        if (!cleanUp(error)) {
            publish(Phase::Failed, error, true);
            return;
        }
        if (!current())
            return;

        publish(phase);
        const auto ports = dependencies_.checkPortOwnership(layout_.platform(), bundledPorts());
        if (!current())
            return;
        if (ports.status != PortCheck::Status::Available) {
            publish(Phase::Failed, ports.errorMessage());
            return;
        }

        if (dependencies_.makeProcess)
            host_ = std::make_unique<BundledHost>(layout_, dependencies_.makeProcess());
        else
            host_ = std::make_unique<BundledHost>(layout_);
        if (!current())
            return;

        const bool started = host_->start(error);
        if (!current())
            return;
        if (!started) {
            fail(error.isEmpty() ? QStringLiteral("无法启动内置 Sunshine。") : error);
            return;
        }

        lastReadError_.clear();
        nextReadMs_ = 0;
        startup_.start();
        timer_.start();
        poll();
    }

    void handleExit()
    {
        const auto code = host_->exitCode();
        if (phase_ == Phase::Ready && code == BundledHost::restartRequestedExitCode) {
            if (lastAutomaticRestart_.isValid() && lastAutomaticRestart_.elapsed() < timing_.automaticRestartCooldownMs) {
                fail(QStringLiteral("内置 Sunshine 连续请求重启，请重新启用。"));
                return;
            }
            lastAutomaticRestart_.start();
            beginStart(Phase::Restarting);
            return;
        }
        fail(QStringLiteral("内置 Sunshine 已退出。日志位于 %1").arg(layout_.logFile()));
    }

    void readinessTimedOut()
    {
        auto error = QStringLiteral("内置 Sunshine 未能在启动期限内通过管理接口验证。");
        if (!lastReadError_.isEmpty())
            error += QLatin1Char(' ') + lastReadError_;
        fail(error);
    }

    void poll()
    {
        if (!current() || !host_)
            return;
        if (!host_->running()) {
            handleExit();
            return;
        }
        if (phase_ == Phase::Ready)
            return;

        const auto elapsed = startup_.elapsed();
        if (elapsed >= timing_.startupTimeoutMs) {
            readinessTimedOut();
            return;
        }
        if (!admin_ && elapsed >= nextReadMs_)
            beginRead();
    }

    void beginRead()
    {
        auto password = host_->launchPassword();
        if (password.isEmpty()) {
            handleExit();
            return;
        }
        admin_ = dependencies_.openAdmin(layout_.profileDirectory(), password, lastReadError_);
        password.fill('\0');
        if (!current())
            return;
        if (!admin_) {
            nextReadMs_ = startup_.elapsed() + timing_.pollMs;
            return;
        }
        if (startup_.elapsed() >= timing_.startupTimeoutMs) {
            readinessTimedOut();
            return;
        }

        const auto id = ++readId_;
        const auto request = request_;
        const QPointer<LifecycleWorker> worker(this);
        const bool started = admin_->readConfig([worker, request, id](BundledAdmin::Result result) {
            if (worker) {
                QMetaObject::invokeMethod(worker.data(), [worker, request, id, result = std::move(result)] {
                    if (worker)
                        worker->readFinished(request, id, result);
                }, Qt::QueuedConnection);
            }
        });
        if (!started)
            fail(QStringLiteral("无法开始验证内置 Sunshine 的管理接口。"));
    }

    void readFinished(quint64 request, quint64 id, const BundledAdmin::Result &result)
    {
        if (!current() || request != request_ || id != readId_)
            return;
        ++readId_;
        if (!host_->running()) {
            handleExit();
            return;
        }
        if (startup_.elapsed() >= timing_.startupTimeoutMs) {
            readinessTimedOut();
            return;
        }
        if (result) {
            settings_ = std::make_unique<SettingsSession>(QUuid::createUuid().toString(QUuid::WithoutBraces),
                *admin_, [this] { return current() && phase_ == Phase::Ready && host_->running(); });
            publish(Phase::Ready);
            return;
        }

        cancelRead();
        if (!canRetry(result.error)) {
            fail(result.message.isEmpty() ? QStringLiteral("内置 Sunshine 未通过管理接口验证。") : result.message);
            return;
        }

        lastReadError_ = result.message;
        nextReadMs_ = startup_.elapsed() + timing_.pollMs;
    }

    HostLayout layout_;
    LifecycleDependencies dependencies_;
    LifecycleTiming timing_;
    std::shared_ptr<std::atomic<quint64>> latest_;
    quint64 request_ = 0;
    quint64 readId_ = 0;
    Phase phase_ = Phase::Stopped;
    QTimer timer_{this};
    QElapsedTimer startup_;
    QElapsedTimer lastAutomaticRestart_;
    qint64 nextReadMs_ = 0;
    QString lastReadError_;
    std::unique_ptr<BundledHost> host_;
    std::unique_ptr<ReadinessAdmin> admin_;
    std::unique_ptr<SettingsSession> settings_;
};

LifecycleController::LifecycleController(HostLayout layout, QObject *parent)
    : LifecycleController(std::move(layout), {}, {}, parent)
{
}

LifecycleController::LifecycleController(HostLayout layout, LifecycleDependencies dependencies, LifecycleTiming timing,
                                         QObject *parent)
    : QObject(parent), latest_(std::make_shared<std::atomic<quint64>>(0)),
      worker_(new LifecycleWorker(std::move(layout), std::move(dependencies), timing, latest_))
{
    auto *workerThread = new QThread;
    workerThread->setObjectName(QStringLiteral("Bundled Sunshine lifecycle"));
    worker_->moveToThread(workerThread);
    qRegisterMetaType<SettingsReadResult>();
    qRegisterMetaType<SettingsSaveResult>();
    connect(worker_, &LifecycleWorker::progressed, this,
        [this](quint64 request, int phase, const QString &error, bool cleanupPending, const QString &settingsSession) {
            if (request == state_.request)
                setState({request, Phase(phase), error, cleanupPending, settingsSession});
        }, Qt::QueuedConnection);
    connect(worker_, &LifecycleWorker::settingsReadFinished, this,
        [this](quint64 operation, const SettingsReadResult &result) {
            if (pendingSettings_ && pendingSettings_->operation == operation) {
                auto pending = std::move(pendingSettings_);
                pending->read(result);
            }
        }, Qt::QueuedConnection);
    connect(worker_, &LifecycleWorker::settingsSaveFinished, this,
        [this](quint64 operation, const SettingsSaveResult &result) {
            if (pendingSettings_ && pendingSettings_->operation == operation) {
                auto pending = std::move(pendingSettings_);
                pending->save(result);
            }
        }, Qt::QueuedConnection);
    connect(workerThread, &QThread::finished, worker_, &QObject::deleteLater);
    connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
    connect(workerThread, &QThread::finished, this, &LifecycleController::shutdownFinished, Qt::QueuedConnection);
    workerThread->start();
}

LifecycleController::~LifecycleController()
{
    // The independent worker owns cleanup and will quit even after the controller goes away.
    if (!closing_)
        enqueue(Action::Shutdown, state_.request + 1);
}

bool LifecycleController::acceptsRequests() const
{
    return QThread::currentThread() == thread() && !closing_;
}

bool LifecycleController::start()
{
    if (!acceptsRequests() || (state_.phase != Phase::Stopped && state_.phase != Phase::Failed && state_.phase != Phase::Stopping))
        return false;
    submit(Action::Start, Phase::Starting);
    return true;
}

bool LifecycleController::restart()
{
    if (!acceptsRequests() || state_.phase != Phase::Ready)
        return false;
    submit(Action::Restart, Phase::Restarting);
    return true;
}

bool LifecycleController::stop()
{
    if (!acceptsRequests() || state_.phase == Phase::Stopped || state_.phase == Phase::Stopping)
        return false;
    submit(Action::Stop, Phase::Stopping);
    return true;
}

bool LifecycleController::shutdown()
{
    if (!acceptsRequests())
        return false;
    closing_ = true;
    submit(Action::Shutdown, Phase::Stopping);
    return true;
}

bool LifecycleController::readSettings(SettingsReadCompletion completion)
{
    if (!acceptsRequests() || state_.phase != Phase::Ready || pendingSettings_ || !completion)
        return false;

    const auto operation = ++nextSettingsOperation_;
    const auto session = state_.settingsSession;
    pendingSettings_ = std::make_unique<PendingSettings>(
        PendingSettings{operation, session, std::nullopt, std::move(completion), {}});
    auto *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker, request = state_.request, operation, session] {
        worker->readSettings(request, operation, session);
    }, Qt::QueuedConnection);
    return true;
}

bool LifecycleController::saveSettings(SettingsEdit edit, SettingsSaveCompletion completion)
{
    if (!acceptsRequests() || state_.phase != Phase::Ready || pendingSettings_ || !completion)
        return false;

    const auto operation = ++nextSettingsOperation_;
    pendingSettings_ = std::make_unique<PendingSettings>(
        PendingSettings{operation, edit.version.session, edit, {}, std::move(completion)});
    auto *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker, request = state_.request, operation, edit = std::move(edit)] {
        worker->saveSettings(request, operation, edit);
    }, Qt::QueuedConnection);
    return true;
}

void LifecycleController::submit(Action action, Phase phase)
{
    const auto request = state_.request + 1;
    enqueue(action, request);
    setState({request, phase, {}, state_.cleanupPending, {}});
}

void LifecycleController::enqueue(Action action, quint64 request)
{
    latest_->store(request);
    auto *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker, action, request] { worker->handle(action, request); }, Qt::QueuedConnection);
}

void LifecycleController::setState(State state)
{
    if (state_.request == state.request && state_.phase == state.phase &&
        state_.error == state.error && state_.cleanupPending == state.cleanupPending &&
        state_.settingsSession == state.settingsSession)
        return;

    if (pendingSettings_ && state.settingsSession != state_.settingsSession) {
        auto cancelled = std::shared_ptr<PendingSettings>(std::move(pendingSettings_));
        QMetaObject::invokeMethod(this, [cancelled] { cancelled->cancel(); }, Qt::QueuedConnection);
    }
    state_ = std::move(state);
    emit stateChanged();
}

} // namespace Aurora::Sunshine

#include "LifecycleController.moc"
