// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"

#include "MoonlightProcess.h"

#include <utility>

namespace Aurora::Session {
namespace Control = Stream::Control;

QVariantMap SessionSupervisor::videoSettings() const
{
    auto settings = confirmedVideoSettings_.toVariantMap();
    if (liveBitrate_) {
        settings.insert(QStringLiteral("bitrate"), QString::number(liveBitrate_->requestedKbps));
        if (bitrateCapabilityReported_)
            settings.insert(QStringLiteral("liveBitrateMaximum"), liveBitrate_->maxRequestedKbps);
    }
    return settings;
}

void SessionSupervisor::refreshVideoBitrate()
{
    if (hasWindowsVideo())
        startVideoBitrate(Control::Operation::BitrateStatus);
}

bool SessionSupervisor::startVideoBitrate(Control::Operation operation, std::optional<Stream::BitrateChange> change)
{
    if (!bitrateControl_ || !canUseSessionControls() || !bitrateRequest_.isNull() || phase_ != Phase::Streaming ||
        !surface_.videoReady() || !worker_ || worker_->state() != QProcess::Running)
        return false;

    const auto token = QUuid::createUuid();
    const auto attempt = attempt_;
    const auto &route = *controlRoute_;
    Control::Authorization grant{token, operation, executable_, route.selected, route.revalidated,
        route.checkedRelay, lease_, [this, attempt](const QUuid &request) {
            return attempt_ == attempt && bitrateRequest_ == request && canUseSessionControls() &&
                phase_ == Phase::Streaming && surface_.videoReady() && worker_ && worker_->state() == QProcess::Running;
        }, std::move(change), resolved_.certificateSha256};
    if (!bitrateControl_->start(std::move(grant)))
        return false;
    bitrateRequest_ = token;
    return true;
}

void SessionSupervisor::videoBitrateFinished(const QUuid &token, const Control::Result &result)
{
    if (token != bitrateRequest_)
        return;
    bitrateRequest_ = {};
    if (pendingFinish_) {
        completeFinish();
        return;
    }
    if (phase_ != Phase::Streaming || !worker_ || worker_->state() != QProcess::Running || !surface_.videoReady())
        return;

    const bool updating = videoChange_ && videoChange_->step == VideoStep::UpdatingBitrate;
    const bool confirmed = result.bitrate && (!liveBitrate_ ||
        (result.bitrate->session == liveBitrate_->session && result.bitrate->revision >= liveBitrate_->revision));
    const bool recovering = !updating && liveBitrate_ && !bitrateCapabilityReported_;
    if (confirmed)
        liveBitrate_ = result.bitrate;
    const bool unsupported = !updating && !liveBitrate_ && result.status == Control::Status::CapabilityUnavailable;
    bitrateCapabilityReported_ = confirmed || unsupported;
    if (!updating) {
        if (recovering && confirmed)
            videoMessage_ = tr("已重新确认当前码率。");
        emit videoSettingsChanged();
        return;
    }

    const bool applied = result.status == Control::Status::BitrateApplied && confirmed;
    if (applied) {
        view_.preferenceArguments = std::move(videoChange_->requested.preferenceArguments);
        view_.sessionPreferences = std::move(videoChange_->requested.sessionPreferences);
        confirmedVideoSettings_ = view_.sessionPreferences;
        videoMessage_ = tr("编码器已确认新码率。");
    } else if (result.status == Control::Status::BitratePending || result.status == Control::Status::BitrateBusy) {
        videoMessage_ = tr("编码器仍在处理码率变更，请稍后重新检查。");
    } else if (result.status == Control::Status::BitrateExpired) {
        videoMessage_ = tr("码率请求在编码器执行前已到期。");
    } else if (result.status == Control::Status::BitrateRejected) {
        videoMessage_ = tr("请求的码率超出本次串流的协商范围。");
    } else if (result.status == Control::Status::BitrateFailed) {
        videoMessage_ = tr("编码器未能应用请求的码率。");
    } else {
        videoMessage_ = tr("未收到编码器确认，请重新检查当前码率。");
    }
    videoChange_.reset();
    if (!applied)
        refreshVideoBitrate();
    emit videoSettingsApplied(applied);
    emit videoSettingsChanged();
}

void SessionSupervisor::resetVideoBitrate()
{
    if (!bitrateRequest_.isNull())
        bitrateControl_->cancel(bitrateRequest_);
    liveBitrate_.reset();
    bitrateCapabilityReported_ = false;
}

bool SessionSupervisor::sessionControlBusy() const
{
    return (privacyControl_ && privacyControl_->busy()) || (bitrateControl_ && bitrateControl_->busy());
}

} // namespace Aurora::Session
