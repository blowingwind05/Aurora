// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"

#include "ControlAdapter.h"
#include "MoonlightProcess.h"

#include <utility>

namespace Aurora::Session {
namespace {

using Discovery::Endpoint;
namespace Control = Stream::Control;
using Control::Operation;
using Control::PrivacyState;

bool sameEndpoint(const Endpoint &a, const Endpoint &b)
{
    return a.port == b.port && a.address.protocol() == b.address.protocol() &&
        a.address.scopeId() == b.address.scopeId() && a.address == b.address;
}

} // namespace

bool SessionSupervisor::matchesControlRoute(const SessionControlRoute &route) const
{
    const auto &checked = route.revalidated;
    const auto &selected = route.selected;
    if (selected.viaEmbeddedTailnet != resolved_.viaEmbeddedTailnet ||
        !Discovery::sameHostIdentity(selected.id, resolved_.hostId)) {
        return false;
    }

    const Endpoint workerEndpoint{resolved_.address, quint16(resolved_.port)};
    if (!checked) {
        return selected.publicIpv6 && !selected.viaEmbeddedTailnet && !route.checkedRelay &&
            workerEndpoint.address.protocol() == QAbstractSocket::IPv6Protocol &&
            resolved_.certificateSha256.size() == 32 && sameEndpoint(selected.endpoint, workerEndpoint);
    }
    if (checked->status != Discovery::ServerInfo::Status::Complete || !checked->current ||
        checked->current->viaEmbeddedTailnet != selected.viaEmbeddedTailnet ||
        !Discovery::sameHostIdentity(selected.id, checked->current->id) ||
        !sameEndpoint(selected.endpoint, checked->current->endpoint)) {
        return false;
    }
    if (const auto *embedded = std::get_if<EmbeddedRoute>(&candidate_.route)) {
        return lease_ && lease_->isValid() && route.checkedRelay &&
            sameEndpoint(*route.checkedRelay, workerEndpoint) &&
            sameEndpoint(selected.endpoint, {embedded->address, Discovery::BundledSunshinePort});
    }
    return !route.checkedRelay && sameEndpoint(selected.endpoint, workerEndpoint);
}

bool SessionSupervisor::canUseSessionControls() const
{
    return controlRoute_ && (controlRoute_->revalidated || pinnedControlAvailable_);
}

bool SessionSupervisor::canQueryPrivacy() const
{
    return canUseSessionControls() && (!controlRoute_->revalidated ||
        controlRoute_->revalidated->current->supportsSessionPrivacy);
}

bool SessionSupervisor::privacyAllowsPlaybackReplacement() const
{
    return privacyRequest_.isNull() && (!canQueryPrivacy() || privacyState_ == PrivacyState::Off ||
        privacyState_ == PrivacyState::Unsupported);
}

QVariant SessionSupervisor::privacyScreen() const
{
    if (phase_ != Phase::Streaming || !canQueryPrivacy() || !privacyState_ ||
        *privacyState_ == PrivacyState::Unavailable || *privacyState_ == PrivacyState::Unsupported) {
        return {};
    }
    const auto mode = privacyState_ == PrivacyState::DisplayOff ? QStringLiteral("display-off")
        : privacyState_ == PrivacyState::On ? QStringLiteral("screensaver") : QStringLiteral("off");
    return QVariantMap{{QStringLiteral("value"), *privacyState_ != PrivacyState::Off},
                       {QStringLiteral("mode"), mode},
                       {QStringLiteral("writable"), privacyRequest_.isNull()}};
}

QString SessionSupervisor::privacyScreenMessage() const
{
    if (phase_ != Phase::Streaming || !canQueryPrivacy())
        return {};
    if (!privacyRequest_.isNull())
        return privacyTarget_ ? tr("正在切换隐私保护…") : tr("正在检查隐私保护…");
    if (!privacyFeedback_.isEmpty())
        return privacyFeedback_;
    if (!privacyState_ || *privacyState_ == PrivacyState::Unavailable)
        return tr("隐私保护状态未确认，重新打开控制中心可重试。");
    if (*privacyState_ == PrivacyState::Unsupported)
        return tr("当前会话不支持隐私保护");
    return {};
}

void SessionSupervisor::refreshPrivacyScreen()
{
    startPrivacy(Operation::PrivacyStatus);
}

void SessionSupervisor::setPrivacyScreen(bool enabled)
{
    setPrivacyMode(enabled ? QStringLiteral("screensaver") : QStringLiteral("off"));
}

void SessionSupervisor::setPrivacyMode(const QString &mode)
{
    if (!privacyState_ || privacyState_ == PrivacyState::Unavailable || privacyState_ == PrivacyState::Unsupported)
        return;
    if (mode == QLatin1String("screensaver") && privacyState_ != PrivacyState::On)
        startPrivacy(Operation::PrivacyOn);
    else if (mode == QLatin1String("display-off") && privacyState_ != PrivacyState::DisplayOff)
        startPrivacy(Operation::PrivacyDisplayOff);
    else if (mode == QLatin1String("off") && privacyState_ != PrivacyState::Off)
        startPrivacy(Operation::PrivacyOff);
}

void SessionSupervisor::startPrivacy(Operation operation)
{
    if (phase_ != Phase::Streaming || videoSettingsPending() || !canQueryPrivacy() || !privacyRequest_.isNull() ||
        !worker_ || worker_->state() != QProcess::Running || !surface_.videoReady()) {
        return;
    }

    const auto token = QUuid::createUuid();
    const auto attempt = attempt_;
    Control::Authorization grant{token, operation, executable_, controlRoute_->selected, controlRoute_->revalidated,
        controlRoute_->checkedRelay, lease_, [this, attempt](const QUuid &current) {
            return attempt_ == attempt && privacyRequest_ == current && canQueryPrivacy() &&
                phase_ == Phase::Streaming && surface_.videoReady() && worker_ && worker_->state() == QProcess::Running;
        }, {}, resolved_.certificateSha256};
    if (privacyControl_->start(std::move(grant))) {
        privacyRequest_ = token;
        privacyFeedback_.clear();
        switch (operation) {
        case Operation::PrivacyOn: privacyTarget_ = PrivacyState::On; break;
        case Operation::PrivacyOff: privacyTarget_ = PrivacyState::Off; break;
        case Operation::PrivacyDisplayOff: privacyTarget_ = PrivacyState::DisplayOff; break;
        default: privacyTarget_.reset(); break;
        }
        emit privacyScreenChanged();
    }
}

void SessionSupervisor::privacyFinished(const QUuid &token, const Control::Result &result)
{
    if (token != privacyRequest_)
        return;
    privacyRequest_ = {};
    const auto requestedMode = std::exchange(privacyTarget_, std::nullopt);
    if (pendingFinish_) {
        completeFinish();
        return;
    }
    if (phase_ == Phase::Streaming && canQueryPrivacy() && surface_.videoReady() &&
        worker_ && worker_->state() == QProcess::Running) {
        privacyState_ = result.status == Control::Status::PrivacyReported ? result.privacy : std::nullopt;
        if (requestedMode && requestedMode != privacyState_) {
            privacyFeedback_ = *requestedMode == PrivacyState::DisplayOff
                ? tr("被控端未能准备好用于关屏的虚拟显示器。")
                : tr("被控端未能确认新的隐私保护状态。");
        }
        emit privacyScreenChanged();
    }
}

void SessionSupervisor::revokePrivacy()
{
    if (!canQueryPrivacy())
        return;
    privacyState_.reset();
    privacyFeedback_.clear();
    if (!privacyRequest_.isNull())
        privacyControl_->cancel(privacyRequest_);
    emit privacyScreenChanged();
}

} // namespace Aurora::Session
