// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"
#include "ControlAdapter.h"
#include "MoonlightProcess.h"
#include "../../../runtime/stream/AuroraStreamSessionProtocol.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QPlatformSurfaceEvent>
#include <QProcessEnvironment>
#include <QThread>

#include <algorithm>
#include <utility>

namespace Aurora::Session {
namespace {

constexpr int runtimeDeadlineMs = 4000;
constexpr int videoReadyDeadlineMs = 15000;
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
        displayControl_ = std::make_unique<Stream::Control::Adapter>();
        connect(displayControl_.get(), &Stream::Control::Adapter::finished,
                this, &SessionSupervisor::displayFinished, Qt::QueuedConnection);
        bitrateControl_ = std::make_unique<Stream::Control::Adapter>();
        connect(bitrateControl_.get(), &Stream::Control::Adapter::finished,
                this, &SessionSupervisor::videoBitrateFinished, Qt::QueuedConnection);
    }
    startupDeadline_.setSingleShot(true);
    stopDeadline_.setSingleShot(true);
    startupDeadline_.setTimerType(Qt::PreciseTimer);
    stopDeadline_.setTimerType(Qt::PreciseTimer);
    connect(&startupDeadline_, &QTimer::timeout, this, [this] {
        if (!worker_)
            return;
        if (probing_)
            beginStop(SessionEnd::RuntimeProbeFailed);
        else
            playbackFailed({SessionEnd::WorkerStartFailed});
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
        if (surface_.videoReady() && phase_ == Phase::Connecting)
            playbackReady();
        else if (surface_.attachmentFailed() && worker_ && worker_->state() != QProcess::NotRunning)
            playbackFailed({SessionEnd::AttachmentFailed});
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
    displayControl_.reset();
    bitrateControl_.reset();
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
    resumeAfterRelayStop_ = false;
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

    verifiesHostIdentity_ = false;
    probes_.enqueue(Stream::Probe::HostIdentity);
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
    const PreparedSession session{attempt_, candidate_.generation, executable_, resolved_, verifiesHostIdentity_};
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

    if ((authorization.resumeOnly || !authorization.certificateSha256.isEmpty()) &&
        (!verifiesHostIdentity_ || authorization.certificateSha256.size() != 32))
        return false;

    resolved_.certificateSha256 = authorization.certificateSha256;
    resumeOnly_ = authorization.resumeOnly;
    view_ = std::move(view);
    if (platform_ == Stream::RuntimePlatform::MacOS)
        view_.owner.clear();
    if (platform_ == Stream::RuntimePlatform::Windows && authorization.controls && matchesControlRoute(*authorization.controls))
        controlRoute_ = authorization.controls;
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
    const auto token = attempt_;
    if (embed && !videoChange_) {
        const auto initial = freezePlayback({view_.preferenceArguments, view_.sessionPreferences});
        if (attempt_ != token || phase_ != Phase::Starting)
            return;
        view_.preferenceArguments = initial.preferenceArguments;
    }
    Stream::Request request;
    request.route = resolved_;
    request.resumeOnly = resumeOnly_;
    request.readConnectionDetails = true;
    request.deviceName = candidate_.deviceName;
    if (videoChange_ && videoChange_->step == VideoStep::Applying) {
        request.preferenceArguments = videoChange_->requested.preferenceArguments;
        request.sessionPreferences = videoChange_->requested.sessionPreferences;
    } else {
        request.preferenceArguments = view_.preferenceArguments;
        request.sessionPreferences = view_.sessionPreferences;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(StreamSession::SwitchChannel);
    if (platform_ == Stream::RuntimePlatform::MacOS && resolved_.certificateSha256.size() == 32)
        environment.insert(StreamSession::SwitchChannel, QStringLiteral("1"));
    if (!environment.contains("AURORA_QT_SESSION"))
        environment.insert("AURORA_QT_SESSION", "1");

    if (embed) {
        if (!view_.owner || view_.owner->thread() != thread())
            return finish({SessionEnd::SurfaceLost});

        if (!videoChange_) {
            previousWindowState_ = view_.owner->windowState() == Qt::WindowMaximized
                ? Qt::WindowMaximized : Qt::WindowNoState;
            view_.owner->installEventFilter(this);
            ownerDestroyed_ = connect(view_.owner, &QObject::destroyed, this,
                                      [this] { beginStop(SessionEnd::SurfaceLost); });
            ownerStateChanged_ = connect(view_.owner, &QWindow::windowStateChanged, this,
                                         [this](Qt::WindowState state) {
                if (state == Qt::WindowNoState || state == Qt::WindowMaximized)
                    previousWindowState_ = state;
                emit fullScreenChanged();
            });
        }
        request.embeddedInvitation = surface_.prepare(view_.owner, view_.viewport);
        if (attempt_ != token || phase_ != Phase::Starting)
            return;
        if (request.embeddedInvitation.isEmpty())
            return playbackFailed({SessionEnd::AttachmentFailed});
        surface_.setViewport(view_.viewport, view_.suppressSurface);
        if (attempt_ != token || phase_ != Phase::Starting)
            return;
    }

    auto command = Stream::makeCommand(executable_, request, platform_, environment);
    if (attempt_ != token || phase_ != Phase::Starting)
        return;
    if (resolved_.viaEmbeddedTailnet && (!lease_ || !lease_->isValid()))
        return finish({SessionEnd::RelayLost});
    if (!command.command)
        return playbackFailed({SessionEnd::InvalidLaunch, {}, false, command.error});
    // Keep the app on the host unless the Aurora session explicitly requests quit-on-stop.
    auto &arguments = command.command->arguments;
    if (!arguments.contains(QStringLiteral("--quit-after")) &&
        !arguments.contains(QStringLiteral("--no-quit-after"))) {
        arguments.append(QStringLiteral("--no-quit-after"));
    }
    launchWorker(*command.command, false, embed);
}

void SessionSupervisor::launchWorker(const Stream::Command &command, bool probing, bool bindSurface)
{
    probing_ = probing;
    pinnedControlAvailable_ = false;
    output_.clear();
    discardLine_ = false;
    auto *child = new Stream::MoonlightProcess(this);
    if (!probing && command.environment.contains(StreamSession::SwitchChannel))
        child->setInputChannelMode(QProcess::ManagedInputChannel);
    worker_ = child;
    if (bindSurface && !surface_.bindWorker(child))
        return playbackFailed({SessionEnd::AttachmentFailed});

    connect(child, &QProcess::readyReadStandardOutput, this, &SessionSupervisor::drainOutput);
    connect(child, &QProcess::started, this, [this] {
        if (probing_ || phase_ != Phase::Starting)
            return;
        startupDeadline_.stop();
        if (videoChange_)
            startupDeadline_.start(videoReadyDeadlineMs);
        setPhase(platform_ == Stream::RuntimePlatform::Windows ? Phase::Connecting : Phase::Standalone);
        // The renderer can become ready during StreamSurface's started handler.
        if (phase_ == Phase::Connecting && surface_.videoReady())
            playbackReady();
    });
    connect(child, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (phase_ == Phase::Stopping || probing_)
            finish({phase_ == Phase::Stopping ? stopReason_ : SessionEnd::RuntimeProbeFailed});
        else
            playbackFailed({SessionEnd::WorkerStartFailed});
    });
    connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SessionSupervisor::workerFinished);

    startupDeadline_.start(runtimeDeadlineMs);
    if (!child->launch(command)) {
        if (probing_)
            finish({SessionEnd::RuntimeProbeFailed});
        else
            playbackFailed({SessionEnd::WorkerStartFailed});
    }
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
    if (line.size() > maxLineBytes || line.contains('\0') || phase_ == Phase::Stopping ||
        (videoChange_ && (videoChange_->step == VideoStep::Draining || videoChange_->step == VideoStep::Discarding))) {
        return;
    }

    if (platform_ == Stream::RuntimePlatform::Windows) {
        if (line == StreamSession::PinnedControlAvailable && controlRoute_ && !controlRoute_->revalidated) {
            if (!std::exchange(pinnedControlAvailable_, true)) {
                refreshPrivacyScreen();
                requestDisplay(QStringLiteral("list"));
            }
            return;
        }
        if (surface_.handleWorkerLine(worker_, line))
            return;
    }
    if (platform_ == Stream::RuntimePlatform::MacOS) {
        if (phase_ == Phase::Standalone &&
            (line == StreamSession::PlaybackReady || line == StreamSession::SwitchReady)) {
            if (line == StreamSession::SwitchReady && worker_->inputChannelMode() == QProcess::ManagedInputChannel)
                switchState_ = SwitchState::Ready;
            playbackReady();
            return;
        }
        if (line == StreamSession::SwitchDenied && switchState_ == SwitchState::Requested) {
            switchState_ = SwitchState::Ready;
            emit routeSwitchDenied(attempt_);
            return;
        }
    }

    QVariantMap metrics;
    bool poor = false;
    if (parseSample(line, metrics, poor)) {
        if (metrics != metrics_ || poor != networkPoor_) {
            metrics_ = std::move(metrics);
            if (poor != networkPoor_) {
                if (poor)
                    networkPoorSince_.start();
                else
                    networkPoorSince_.invalidate();
            }
            networkPoor_ = poor;
            emit connectionChanged();
        }
        return;
    }

    if (platform_ == Stream::RuntimePlatform::MacOS) {
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
    if (phase_ == Phase::Stopping) {
        const auto reason = stopReason_ == SessionEnd::RouteSwitched &&
                (forcedStop_ || status != QProcess::NormalExit || code != 0)
            ? SessionEnd::WorkerStartFailed : stopReason_;
        return finish({reason, code, forcedStop_});
    }

    if (probing_) {
        const bool supported = status == QProcess::NormalExit && Stream::probeSucceeded(currentProbe_, code, output_);
        const bool legacy = currentProbe_ == Stream::Probe::HostIdentity &&
            status == QProcess::NormalExit && code == 2 && output_.trimmed().isEmpty();
        retireWorker();
        if (legacy) {
            probes_.prepend(platform_ == Stream::RuntimePlatform::Windows
                ? Stream::Probe::EmbeddedWindow : Stream::Probe::Runtime);
        } else if (!supported) {
            return finish({SessionEnd::RuntimeProbeFailed, code});
        } else if (currentProbe_ == Stream::Probe::HostIdentity) {
            verifiesHostIdentity_ = true;
        }
        return nextProbe();
    }
    if (videoChange_ && (videoChange_->step == VideoStep::Draining || videoChange_->step == VideoStep::Discarding)) {
        const bool first = videoChange_->step == VideoStep::Draining;
        // The old process and its job must finish before creating another invitation.
        if (forcedStop_ || status != QProcess::NormalExit || (first && code != 0))
            return finish({SessionEnd::WorkerStartFailed, code, forcedStop_});
        return restartVideo(first ? VideoStep::Applying : VideoStep::Restoring);
    }
    if (status == QProcess::NormalExit && code == StreamSession::RouteSwitched &&
        switchState_ == SwitchState::Requested)
        return finish({SessionEnd::RouteSwitched, code});
    if (status == QProcess::NormalExit && code == StreamSession::TransportLost)
        return playbackFailed({SessionEnd::TransportLost, code});
    if (surface_.attachmentFailed())
        return playbackFailed({SessionEnd::AttachmentFailed, code});
    playbackFailed({status == QProcess::NormalExit ? SessionEnd::Exited : SessionEnd::WorkerCrashed, code});
}

bool SessionSupervisor::requestRouteSwitch(const QUuid &attempt)
{
    if (attempt.isNull() || attempt != attempt_ || resolved_.certificateSha256.size() != 32 || !canChangeRoute())
        return false;
    if (platform_ == Stream::RuntimePlatform::Windows) {
        beginStop(SessionEnd::RouteSwitched);
        return true;
    }

    switchState_ = SwitchState::Requested;
    constexpr qint64 requestBytes = sizeof(StreamSession::SwitchRequest) - 1;
    if (worker_->write(StreamSession::SwitchRequest, requestBytes) == requestBytes)
        return true;
    // Keep the running playback intact if its control pipe is no longer usable.
    switchState_ = SwitchState::Unavailable;
    return false;
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
    if (phase_ == Phase::Stopping && stopReason_ == SessionEnd::RouteSwitched && reason == SessionEnd::Cancelled)
        stopReason_ = SessionEnd::Cancelled;
    if (phase_ == Phase::Idle || phase_ == Phase::Stopping || pendingFinish_)
        return;
    stopReason_ = reason;
    resumeAfterRelayStop_ = reason == SessionEnd::RelayLost &&
        platform_ == Stream::RuntimePlatform::Windows && canKeepApplication();
    startupDeadline_.stop();
    setPhase(Phase::Stopping);
    revokePrivacy();
    resetVideoBitrate();

    if (pendingOpen_ && tailnet_) {
        tailnet_->cancelOpen(pendingOpen_);
    } else if (!worker_ || worker_->state() == QProcess::NotRunning) {
        finish({reason});
    } else if (probing_) {
        worker_->cancel();
    } else {
        stopPlaybackWorker();
    }
}

void SessionSupervisor::stopPlaybackWorker()
{
    startupDeadline_.stop();
    stopDeadline_.start(stopGraceMs);
    if (platform_ == Stream::RuntimePlatform::Windows)
        surface_.requestStop();
    else
        worker_->terminate();
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
    result.canResumeAfterLoss = resolved_.certificateSha256.size() == 32 &&
        (result.reason == SessionEnd::RelayLost || result.reason == SessionEnd::TransportLost) &&
        (resumeAfterRelayStop_ || canKeepApplication());
    pendingFinish_ = std::move(result);
    retireWorker();
    if (sessionControlBusy())
        setPhase(Phase::Stopping);
    revokePrivacy();
    resetDisplays();
    resetVideoBitrate();
    completeFinish();
}

void SessionSupervisor::completeFinish()
{
    if (!pendingFinish_ || sessionControlBusy())
        return;
    const auto token = attempt_;
    const auto generation = candidate_.generation;
    const auto result = *pendingFinish_;
    privacyRequest_ = {};
    displayRequest_ = {};
    displayOperation_.clear();
    displayId_.clear();
    controlRoute_.reset();
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
    networkPoorSince_.invalidate();
    const bool unsettledVideo = videoSettingsPending();
    videoChange_.reset();
    switchState_ = SwitchState::Unavailable;
    confirmedVideoSettings_ = {};
    videoMessage_.clear();
    pendingFinish_.reset();
    setPhase(Phase::Idle);
    emit videoSettingsChanged();
    if (unsettledVideo)
        emit videoSettingsApplied(false);
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
    if (watched == view_.owner && videoChange_ && event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
        beginStop(SessionEnd::SurfaceLost);
    }
    if (watched == view_.owner && event->type() == QEvent::Close && phase_ != Phase::Idle) {
        static_cast<QCloseEvent *>(event)->ignore();
        requestStop();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace Aurora::Session
