// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"

#include "ControlAdapter.h"
#include "MoonlightProcess.h"

#include <QThread>

#include <algorithm>
#include <utility>

namespace Aurora::Session {

SessionPlaybackSettings SessionSupervisor::freezePlayback(SessionPlaybackSettings settings)
{
    if (settings.preferenceArguments) {
        auto arguments = settings.preferenceArguments({true, true});
        settings.preferenceArguments = [arguments = std::move(arguments)](Stream::PreferenceDefaults) {
            return arguments;
        };
    }
    return settings;
}

bool SessionSupervisor::quitsRemoteApp(const QStringList &arguments)
{
    return arguments.lastIndexOf(QStringLiteral("--quit-after")) >
        arguments.lastIndexOf(QStringLiteral("--no-quit-after"));
}

bool SessionSupervisor::hasWindowsVideo() const
{
    return platform_ == Stream::RuntimePlatform::Windows && phase_ == Phase::Streaming &&
        view_.owner && !videoChange_ && surface_.videoReady() && worker_ && worker_->state() == QProcess::Running;
}

void SessionSupervisor::requestVideoSetting(QString option, QString value)
{
    if (hasWindowsVideo())
        emit videoSettingRequested(std::move(option), std::move(value));
}

bool SessionSupervisor::reconfigureVideo(SessionPlaybackSettings settings)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!hasWindowsVideo() || !canRestartVideo())
        return false;

    const auto reject = [this](const QString &message) {
        videoMessage_ = message;
        emit videoSettingsChanged();
        return false;
    };
    const auto token = attempt_;
    settings = freezePlayback(std::move(settings));
    if (attempt_ != token || !hasWindowsVideo() || !canRestartVideo())
        return false;
    const auto arguments = settings.preferenceArguments
        ? settings.preferenceArguments({true, true}) : QStringList();
    if (std::any_of(arguments.cbegin(), arguments.cend(), [](const QString &arg) { return arg.contains(QChar::Null); }))
        return reject(tr("无法应用这组画面设置。"));

    auto previous = confirmedVideoSettings_.toVariantMap();
    auto replacement = settings.sessionPreferences.toVariantMap();
    const int requestedKbps = replacement.take(QStringLiteral("bitrate")).toInt();
    const int savedKbps = previous.take(QStringLiteral("bitrate")).toInt();
    const int previousKbps = liveBitrate_ ? liveBitrate_->requestedKbps : savedKbps;
    if (previous == replacement && requestedKbps >= 500) {
        if (requestedKbps == previousKbps)
            return false;
        if (!bitrateRequest_.isNull())
            return reject(tr("正在检查当前编码器，请稍后重试。"));
        if (canUseSessionControls() && !bitrateCapabilityReported_) {
            refreshVideoBitrate();
            return reject(tr("当前码率状态未确认，请稍后重试。"));
        }
        if (liveBitrate_ && requestedKbps <= liveBitrate_->maxRequestedKbps) {
            if (!startVideoBitrate(Stream::Control::Operation::BitrateSet,
                                   Stream::BitrateChange{liveBitrate_->session, requestedKbps, liveBitrate_->revision}))
                return reject(tr("无法开始码率调整，请重试。"));
            videoChange_ = VideoChange{std::move(settings), VideoStep::UpdatingBitrate};
            videoMessage_ = tr("正在等待编码器确认码率…");
            emit videoSettingsChanged();
            return true;
        }
    }

    if (quitsRemoteApp(worker_->arguments()))
        return reject(tr("停止串流会退出远端应用，当前会话无法更改画面设置。"));
    if (!privacyAllowsPlaybackReplacement())
        return reject(tr("请先关闭隐私屏保并确认状态，再更改画面设置。"));
    if (quitsRemoteApp(arguments))
        return reject(tr("无法应用这组画面设置。"));

    resetVideoBitrate();
    videoChange_ = VideoChange{std::move(settings)};
    videoMessage_ = tr("正在重新连接以应用画面设置…");
    setPhase(Phase::Starting);
    emit privacyScreenChanged();
    emit videoSettingsChanged();
    if (attempt_ == token && phase_ == Phase::Starting && videoChange_ && videoChange_->step == VideoStep::Draining)
        stopPlaybackWorker();
    return true;
}

bool SessionSupervisor::canKeepApplication() const
{
    return phase_ == Phase::Streaming && !videoChange_ && worker_ &&
        !quitsRemoteApp(worker_->arguments()) &&
        bitrateRequest_.isNull() && privacyAllowsPlaybackReplacement();
}

bool SessionSupervisor::canChangeRoute() const
{
    return canKeepApplication() && worker_->state() == QProcess::Running &&
        (hasWindowsVideo() || switchState_ == SwitchState::Ready);
}

bool SessionSupervisor::canRestartVideo() const
{
    return view_.owner && (!resolved_.viaEmbeddedTailnet || (lease_ && lease_->isValid()));
}

void SessionSupervisor::restartVideo(VideoStep step)
{
    retireWorker();
    const auto token = attempt_;
    videoChange_->step = step;
    // Release invitations that were prepared but never bound to a running child.
    surface_.requestStop();
    if (attempt_ != token || !videoChange_ || phase_ == Phase::Stopping)
        return;
    if (!canRestartVideo())
        return finish({view_.owner ? SessionEnd::RelayLost : SessionEnd::SurfaceLost});

    metrics_.clear();
    networkPoor_ = false;
    networkPoorSince_.invalidate();
    forcedStop_ = false;
    setPhase(Phase::Starting);
    if (step == VideoStep::Restoring) {
        videoMessage_ = tr("应用失败，正在恢复原画面设置…");
        emit videoSettingsChanged();
    }
    emit connectionChanged();
    QTimer::singleShot(0, this, [this, token, step] {
        if (attempt_ != token || phase_ != Phase::Starting || !videoChange_ || videoChange_->step != step)
            return;
        if (!canRestartVideo())
            return finish({view_.owner ? SessionEnd::RelayLost : SessionEnd::SurfaceLost});
        launchPlayback();
    });
}

void SessionSupervisor::playbackFailed(SessionResult result)
{
    const bool running = worker_ && worker_->state() != QProcess::NotRunning;
    if (phase_ != Phase::Stopping && videoChange_ && videoChange_->step == VideoStep::Applying && canRestartVideo()) {
        if (running) {
            videoChange_->step = VideoStep::Discarding;
            stopPlaybackWorker();
        } else {
            restartVideo(VideoStep::Restoring);
        }
    } else if (running) {
        beginStop(result.reason);
    } else {
        finish(result);
    }
}

void SessionSupervisor::playbackReady()
{
    if (videoChange_ && !canRestartVideo())
        return beginStop(view_.owner ? SessionEnd::RelayLost : SessionEnd::SurfaceLost);
    const auto token = attempt_;
    startupDeadline_.stop();
    if (!videoChange_)
        confirmedVideoSettings_ = view_.sessionPreferences;
    if (networkPoor_)
        networkPoorSince_.start();
    setPhase(Phase::Streaming);
    if (attempt_ != token || phase_ != Phase::Streaming)
        return;
    if (!videoChange_) {
        if (view_.startFullScreen)
            setFullScreen(true);
        emit videoSettingsChanged();
        requestDisplay(QStringLiteral("list"));
        return;
    }

    const bool applied = videoChange_->step == VideoStep::Applying;
    if (applied) {
        view_.preferenceArguments = std::move(videoChange_->requested.preferenceArguments);
        view_.sessionPreferences = std::move(videoChange_->requested.sessionPreferences);
        confirmedVideoSettings_ = view_.sessionPreferences;
    }
    videoMessage_ = applied ? QString() : tr("无法应用新画面设置，已恢复原设置。");
    videoChange_.reset();
    emit videoSettingsApplied(applied);
    emit videoSettingsChanged();
    if (attempt_ == token && phase_ == Phase::Streaming) {
        refreshPrivacyScreen();
        requestDisplay(QStringLiteral("list"));
    }
}

} // namespace Aurora::Session
