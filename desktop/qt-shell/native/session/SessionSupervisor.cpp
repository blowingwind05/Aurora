// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"
#include "ControlAdapter.h"
#include "MoonlightProcess.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QThread>

#include <algorithm>
#include <utility>

namespace Aurora::Session {
namespace {

constexpr int runtimeDeadlineMs = 4000;
constexpr int stopGraceMs = 6000;
constexpr int maxProbeBytes = 256;
constexpr int maxLineBytes = 160;

bool parseSample(const QByteArray &line, QVariantMap &metrics, bool &poor)
{
    const QByteArray prefix("aurora-connection/1,");
    if (!line.startsWith(prefix))
        return false;
    const auto fields = line.mid(prefix.size()).split(',');
    if (fields.size() != 9)
        return false;

    int values[9];
    for (int index = 0; index < fields.size(); ++index) {
        const auto &field = fields.at(index);
        auto digits = field;
        if (digits.startsWith('-') || digits.startsWith('+'))
            digits.remove(0, 1);
        if (digits.isEmpty() || !std::all_of(digits.cbegin(), digits.cend(), [](char ch) { return ch >= '0' && ch <= '9'; }))
            return false;
        bool parsed = false;
        values[index] = field.toInt(&parsed);
        if (!parsed)
            return false;
    }

    if (values[0] > 0 && values[1] > 0)
        metrics.insert("resolution", QStringLiteral("%1 \u00d7 %2").arg(values[0]).arg(values[1]));
    const QStringList codecs{QStringLiteral("H.264"), QStringLiteral("HEVC"), QStringLiteral("AV1")};
    if (values[2] >= 1 && values[2] <= codecs.size())
        metrics.insert("codec", codecs.at(values[2] - 1));
    if (values[3] > 0)
        metrics.insert("frameRate", QString::number(values[3] / 100.0, 'f', 1) + " FPS");
    if (values[4] > 0)
        metrics.insert("bitrate", QString::number(values[4] / 1000.0, 'f', 1) + " Mbps");
    if (values[5] >= 0 && values[5] <= 10000)
        metrics.insert("networkLoss", QString::number(values[5] / 100.0, 'f', 1) + '%');
    if (values[6] >= 0)
        metrics.insert("decodeTime", QString::number(values[6] / 1000.0, 'f', 1) + " ms");
    if (values[7] > 0)
        metrics.insert("latency", QString::number(values[7]) + " ms");
    poor = values[8] == 1;
    return true;
}

} // namespace

SessionSupervisor::SessionSupervisor(QString bundledExecutable, aurora::tailscale::EmbeddedStatusClient *tailnet,
                                     QObject *parent)
    : QObject(parent), executable_(std::move(bundledExecutable)), platform_(Stream::currentPlatform()), tailnet_(tailnet)
{
    qRegisterMetaType<PreparedSession>();
    qRegisterMetaType<SessionResult>();
    if (platform_ == Stream::RuntimePlatform::Windows) {
        privacyControl_ = std::make_unique<Stream::Control::Adapter>();
        connect(privacyControl_.get(), &Stream::Control::Adapter::finished,
                this, &SessionSupervisor::privacyFinished, Qt::QueuedConnection);
    }
    startupDeadline_.setSingleShot(true);
    stopDeadline_.setSingleShot(true);
    startupDeadline_.setTimerType(Qt::PreciseTimer);
    stopDeadline_.setTimerType(Qt::PreciseTimer);
    connect(&startupDeadline_, &QTimer::timeout, this, [this] {
        if (worker_)
            beginStop(probing_ ? SessionEnd::RuntimeProbeFailed : SessionEnd::WorkerStartFailed);
    });
    connect(&stopDeadline_, &QTimer::timeout, this, [this] {
        if (worker_ && worker_->state() != QProcess::NotRunning) {
            forcedStop_ = true;
            worker_->cancel();
        }
    });
    connect(&surface_, &StreamSurface::fullscreenRequested, this, [this] { setFullScreen(!fullScreen()); });
    connect(&surface_, &StreamSurface::clientPixelSizeChanged, this, &SessionSupervisor::clientPixelSizeChanged);
    connect(&surface_, &StreamSurface::surfaceLost, this, [this] { beginStop(SessionEnd::SurfaceLost); });
    connect(&surface_, &StreamSurface::stateChanged, this, [this] {
        if (surface_.videoReady() && phase_ == Phase::Connecting) {
            setPhase(Phase::Streaming);
            if (view_.startFullScreen)
                setFullScreen(true);
        } else if (surface_.attachmentFailed() && worker_ && worker_->state() != QProcess::NotRunning) {
            beginStop(SessionEnd::AttachmentFailed);
        }
    });

    if (!tailnet_)
        return;
    connect(tailnet_, &aurora::tailscale::EmbeddedStatusClient::openFinished, this,
            [this](quint64 generation, quint64 id, const aurora::tailscale::OpenResult &result) {
        if (!pendingOpen_ || pendingOpen_ != id || candidate_.generation != generation)
            return;
        pendingOpen_ = 0;
        if (phase_ == Phase::Stopping) {
            if (result.lease)
                result.lease->close();
            finish({stopReason_});
        } else if (!result.lease || !result.lease->isValid()) {
            finish({SessionEnd::RelayUnavailable, {}, false, Stream::CommandError::None, result.error});
        } else {
            lease_ = result.lease;
            resolved_ = {lease_->address(), lease_->port(), candidate_.hostId, true};
            publishPrepared();
        }
    });
    connect(tailnet_, &aurora::tailscale::EmbeddedStatusClient::leasesInvalidated, this, [this] {
        if (lease_ && !lease_->isValid())
            beginStop(SessionEnd::RelayLost);
    });
    connect(tailnet_, &QObject::destroyed, this, [this] {
        if (pendingOpen_) {
            pendingOpen_ = 0;
            finish({phase_ == Phase::Stopping ? stopReason_ : SessionEnd::RelayUnavailable});
        } else if (lease_) {
            beginStop(SessionEnd::RelayLost);
        }
    });
}

SessionSupervisor::~SessionSupervisor()
{
    privacyControl_.reset();
    disconnect(&surface_, nullptr, this, nullptr);
    if (tailnet_) {
        disconnect(tailnet_, nullptr, this, nullptr);
        if (pendingOpen_)
            tailnet_->cancelOpen(pendingOpen_);
    }
    if (worker_) {
        disconnect(worker_, nullptr, this, nullptr);
        surface_.requestStop();
        delete worker_; // MoonlightProcess reaps the child before the relay is released.
    }
    if (lease_)
        lease_->close();
}

QUuid SessionSupervisor::prepare(SessionCandidate candidate)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (phase_ != Phase::Idle)
        return {};
    const auto token = QUuid::createUuid();
    attempt_ = token;
    candidate_ = std::move(candidate);
    setPhase(Phase::Probing);
    QTimer::singleShot(0, this, [this, token] {
        if (attempt_ == token && phase_ == Phase::Probing)
            beginPreparation();
    });
    return token;
}

void SessionSupervisor::beginPreparation()
{
    if (candidate_.hostId.trimmed().isEmpty())
        return finish({SessionEnd::InvalidCandidate});

    const QFileInfo program(executable_);
    if (platform_ == Stream::RuntimePlatform::Unsupported || !program.isAbsolute() || !program.isFile()
        || !program.isExecutable() || !Stream::isBundledMoonlight(executable_)) {
        return finish({SessionEnd::RuntimeUnavailable});
    }

    if (const auto *embedded = std::get_if<EmbeddedRoute>(&candidate_.route)) {
        if (embedded->peerId.isEmpty() || embedded->address.isNull())
            return finish({SessionEnd::InvalidCandidate});
        if (!tailnet_ || tailnet_->thread() != thread())
            return finish({SessionEnd::RelayUnavailable});
    } else {
        const auto &direct = std::get<DirectRoute>(candidate_.route);
        if (direct.address.isNull() || !direct.port)
            return finish({SessionEnd::InvalidCandidate});
    }

    probes_.enqueue(platform_ == Stream::RuntimePlatform::Windows ? Stream::Probe::EmbeddedWindow : Stream::Probe::Runtime);
    if (std::holds_alternative<EmbeddedRoute>(candidate_.route))
        probes_.enqueue(Stream::Probe::GameStreamRelay);
    nextProbe();
}

void SessionSupervisor::nextProbe()
{
    if (probes_.isEmpty())
        return resolveRoute();
    currentProbe_ = probes_.dequeue();
    const auto probe = Stream::makeProbeCommand(executable_, currentProbe_, platform_);
    if (!probe.command)
        return finish({SessionEnd::RuntimeProbeFailed, {}, false, probe.error});
    launchWorker(*probe.command, true, false);
}

void SessionSupervisor::resolveRoute()
{
    if (const auto *direct = std::get_if<DirectRoute>(&candidate_.route)) {
        resolved_ = {direct->address, direct->port, candidate_.hostId, false};
        return publishPrepared();
    }
    const auto &embedded = std::get<EmbeddedRoute>(candidate_.route);
    pendingOpen_ = tailnet_ ? tailnet_->requestOpen(candidate_.generation, embedded.peerId, embedded.address) : 0;
    if (!pendingOpen_)
        return finish({SessionEnd::RelayUnavailable});
    setPhase(Phase::OpeningRelay);
}

void SessionSupervisor::publishPrepared()
{
    const PreparedSession session{attempt_, candidate_.generation, executable_, resolved_};
    setPhase(Phase::AwaitingAuthorization);
    if (attempt_ == session.attempt && phase_ == Phase::AwaitingAuthorization)
        emit prepared(session);
}

bool SessionSupervisor::start(const SessionAuthorization &authorization, SessionView view)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (phase_ != Phase::AwaitingAuthorization || attempt_ != authorization.attempt
        || candidate_.generation != authorization.generation) {
        return false;
    }

    view_ = std::move(view);
    if (platform_ == Stream::RuntimePlatform::MacOS)
        view_.owner.clear();
    if (privacyControl_ && authorization.privacy && matchesPrivacyRoute(*authorization.privacy))
        privacyRoute_ = authorization.privacy;
    const auto token = attempt_;
    setPhase(Phase::Starting);
    QTimer::singleShot(0, this, [this, token] {
        if (attempt_ == token && phase_ == Phase::Starting)
            launchPlayback();
    });
    return true;
}

void SessionSupervisor::launchPlayback()
{
    if (resolved_.viaEmbeddedTailnet && (!lease_ || !lease_->isValid()))
        return finish({SessionEnd::RelayLost});

    const bool embed = platform_ == Stream::RuntimePlatform::Windows;
    Stream::Request request;
    request.route = resolved_;
    request.deviceName = candidate_.deviceName;
    request.preferenceArguments = view_.preferenceArguments;
    request.sessionPreferences = view_.sessionPreferences;
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!environment.contains("AURORA_QT_SESSION"))
        environment.insert("AURORA_QT_SESSION", "1");

    if (embed) {
        if (!view_.owner || view_.owner->thread() != thread())
            return finish({SessionEnd::AttachmentFailed});

        const auto token = attempt_;
        previousWindowState_ = view_.owner->windowState() == Qt::WindowMaximized ? Qt::WindowMaximized : Qt::WindowNoState;
        view_.owner->installEventFilter(this);
        ownerDestroyed_ = connect(view_.owner, &QObject::destroyed, this, [this] { beginStop(SessionEnd::SurfaceLost); });
        ownerStateChanged_ = connect(view_.owner, &QWindow::windowStateChanged, this, [this](Qt::WindowState state) {
            if (state == Qt::WindowNoState || state == Qt::WindowMaximized)
                previousWindowState_ = state;
            emit fullScreenChanged();
        });
        request.embeddedInvitation = surface_.prepare(view_.owner, view_.viewport);
        if (attempt_ != token || phase_ != Phase::Starting)
            return;
        if (request.embeddedInvitation.isEmpty())
            return finish({SessionEnd::AttachmentFailed});
        surface_.setViewport(view_.viewport, view_.suppressSurface);
        if (attempt_ != token || phase_ != Phase::Starting)
            return;
        request.readConnectionDetails = true;
    }

    const auto token = attempt_;
    const auto command = Stream::makeCommand(executable_, request, platform_, environment);
    if (attempt_ != token || phase_ != Phase::Starting)
        return;
    if (resolved_.viaEmbeddedTailnet && (!lease_ || !lease_->isValid()))
        return finish({SessionEnd::RelayLost});
    if (!command.command)
        return finish({SessionEnd::InvalidLaunch, {}, false, command.error});
    launchWorker(*command.command, false, embed);
}

void SessionSupervisor::launchWorker(const Stream::Command &command, bool probing, bool bindSurface)
{
    probing_ = probing;
    output_.clear();
    discardLine_ = false;
    auto *child = new Stream::MoonlightProcess(this);
    worker_ = child;
    if (bindSurface && !surface_.bindWorker(child))
        return finish({SessionEnd::AttachmentFailed});

    connect(child, &QProcess::readyReadStandardOutput, this, &SessionSupervisor::drainOutput);
    connect(child, &QProcess::started, this, [this] {
        if (probing_ || phase_ != Phase::Starting)
            return;
        startupDeadline_.stop();
        setPhase(platform_ == Stream::RuntimePlatform::Windows ? Phase::Connecting : Phase::Standalone);
        // The renderer can become ready during StreamSurface's started handler.
        if (phase_ == Phase::Connecting && surface_.videoReady()) {
            setPhase(Phase::Streaming);
            if (view_.startFullScreen)
                setFullScreen(true);
        }
    });
    connect(child, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({phase_ == Phase::Stopping ? stopReason_ :
                    probing_ ? SessionEnd::RuntimeProbeFailed : SessionEnd::WorkerStartFailed});
    });
    connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SessionSupervisor::workerFinished);

    startupDeadline_.start(runtimeDeadlineMs);
    if (!child->launch(command))
        finish({probing_ ? SessionEnd::RuntimeProbeFailed : SessionEnd::WorkerStartFailed});
}

void SessionSupervisor::drainOutput()
{
    auto *source = worker_;
    while (source == worker_ && source && source->bytesAvailable()) {
        const auto chunk = source->readLine(4096);
        if (chunk.isEmpty())
            break;
        if (probing_) {
            if (output_.size() + chunk.size() > maxProbeBytes) {
                beginStop(SessionEnd::RuntimeProbeFailed);
                return;
            }
            output_ += chunk;
            continue;
        }

        if (!discardLine_ && output_.size() + chunk.size() <= maxLineBytes + 2)
            output_ += chunk;
        else
            discardLine_ = true;
        if (!chunk.endsWith('\n'))
            continue;
        const bool accepted = !std::exchange(discardLine_, false);
        auto line = std::exchange(output_, {});
        if (accepted)
            handleLine(std::move(line));
    }
}

void SessionSupervisor::handleLine(QByteArray line)
{
    if (line.endsWith('\n'))
        line.chop(1);
    if (line.endsWith('\r'))
        line.chop(1);
    if (line.size() > maxLineBytes || line.contains('\0') || phase_ == Phase::Stopping)
        return;

    if (platform_ == Stream::RuntimePlatform::Windows) {
        if (surface_.handleWorkerLine(worker_, line))
            return;
        QVariantMap metrics;
        bool poor = false;
        if (parseSample(line, metrics, poor) && (metrics != metrics_ || poor != networkPoor_)) {
            metrics_ = std::move(metrics);
            networkPoor_ = poor;
            emit connectionChanged();
        }
    } else {
        const QByteArray prefix("aurora-stream-setting/1\t");
        if (line.size() > 128 || !line.startsWith(prefix))
            return;
        const auto fields = line.mid(prefix.size()).split('\t');
        static const QList<QByteArray> options{
            "resolution", "fps", "bitrate", "video-codec", "hdr", "performance-overlay", "audio-config",
            "audio-on-host", "mute-on-focus-loss", "absolute-mouse", "mouse-buttons-swap", "reverse-scroll-direction"
        };
        if (fields.size() == 2 && options.contains(fields.first()) && !fields.last().isEmpty())
            emit preferenceChangeRequested(QString::fromLatin1(fields.first()), QString::fromUtf8(fields.last()));
    }
}

void SessionSupervisor::workerFinished(int code, QProcess::ExitStatus status)
{
    auto *old = worker_;
    drainOutput();
    if (old != worker_)
        return;
    if (phase_ == Phase::Stopping)
        return finish({stopReason_, code, forcedStop_});

    if (probing_) {
        const bool supported = status == QProcess::NormalExit && Stream::probeSucceeded(currentProbe_, code, output_);
        retireWorker();
        if (!supported)
            return finish({SessionEnd::RuntimeProbeFailed, code});
        return nextProbe();
    }
    if (surface_.attachmentFailed())
        return finish({SessionEnd::AttachmentFailed, code});
    finish({status == QProcess::NormalExit ? SessionEnd::Exited : SessionEnd::WorkerCrashed, code});
}

bool SessionSupervisor::cancel(const QUuid &attempt)
{
    if (attempt.isNull() || attempt != attempt_)
        return false;
    requestStop();
    return true;
}

void SessionSupervisor::requestStop()
{
    beginStop(SessionEnd::Cancelled);
}

void SessionSupervisor::beginStop(SessionEnd reason)
{
    if (phase_ == Phase::Idle || phase_ == Phase::Stopping || pendingFinish_)
        return;
    stopReason_ = reason;
    startupDeadline_.stop();
    setPhase(Phase::Stopping);
    revokePrivacy();

    if (pendingOpen_ && tailnet_) {
        tailnet_->cancelOpen(pendingOpen_);
    } else if (!worker_ || worker_->state() == QProcess::NotRunning) {
        finish({reason});
    } else if (probing_) {
        worker_->cancel();
    } else {
        stopDeadline_.start(stopGraceMs);
        if (platform_ == Stream::RuntimePlatform::Windows)
            surface_.requestStop();
        else
            worker_->terminate();
    }
}

void SessionSupervisor::retireWorker()
{
    startupDeadline_.stop();
    stopDeadline_.stop();
    if (auto *child = std::exchange(worker_, nullptr)) {
        disconnect(child, nullptr, this, nullptr);
        child->deleteLater();
    }
}

void SessionSupervisor::finish(SessionResult result)
{
    if (pendingFinish_)
        return;
    pendingFinish_ = std::move(result);
    retireWorker();
    if (privacyControl_ && privacyControl_->busy())
        setPhase(Phase::Stopping);
    revokePrivacy();
    if (pendingFinish_ && (!privacyControl_ || !privacyControl_->busy()))
        completeFinish();
}

void SessionSupervisor::completeFinish()
{
    const auto token = attempt_;
    const auto generation = candidate_.generation;
    const auto result = *pendingFinish_;
    privacyRequest_ = {};
    surface_.requestStop();
    if (lease_) {
        lease_->close();
        lease_.reset();
    }
    probes_.clear();
    output_.clear();
    if (view_.owner)
        view_.owner->removeEventFilter(this);
    disconnect(ownerStateChanged_);
    disconnect(ownerDestroyed_);
    view_ = {};
    previousWindowState_ = Qt::WindowNoState;
    attempt_ = {};
    pendingOpen_ = 0;
    candidate_ = {};
    resolved_ = {};
    forcedStop_ = false;
    metrics_.clear();
    networkPoor_ = false;
    pendingFinish_.reset();
    setPhase(Phase::Idle);
    emit fullScreenChanged();
    emit connectionChanged();
    emit finished(token, generation, result);
}

void SessionSupervisor::setPhase(Phase phase)
{
    if (phase_ == phase)
        return;
    phase_ = phase;
    emit phaseChanged();
    if (phase_ == Phase::Streaming)
        refreshPrivacyScreen();
}

void SessionSupervisor::setViewport(const QRect &viewport, bool suppressed)
{
    view_.viewport = viewport;
    view_.suppressSurface = suppressed;
    surface_.setViewport(viewport, suppressed);
}

bool SessionSupervisor::fullScreen() const
{
    return view_.owner && view_.owner->windowState() == Qt::WindowFullScreen;
}

void SessionSupervisor::setFullScreen(bool enabled)
{
    if (phase_ != Phase::Streaming || !view_.owner || fullScreen() == enabled)
        return;
    view_.owner->setWindowState(enabled ? Qt::WindowFullScreen : previousWindowState_);
}

bool SessionSupervisor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == view_.owner && event->type() == QEvent::Close && phase_ != Phase::Idle) {
        static_cast<QCloseEvent *>(event)->ignore();
        requestStop();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace Aurora::Session
