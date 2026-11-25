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

bool SessionSupervisor::matchesPrivacyRoute(const SessionPrivacyRoute &route) const
{
    const auto &checked = route.revalidated;
    const auto &selected = route.selected;
    if (checked.status != Discovery::ServerInfo::Status::Complete || !checked.current ||
        !checked.current->supportsSessionPrivacy || selected.viaEmbeddedTailnet != resolved_.viaEmbeddedTailnet ||
        checked.current->viaEmbeddedTailnet != selected.viaEmbeddedTailnet ||
        !Discovery::sameHostIdentity(selected.id, resolved_.hostId) ||
        !Discovery::sameHostIdentity(selected.id, checked.current->id) ||
        !sameEndpoint(selected.endpoint, checked.current->endpoint)) {
        return false;
    }

    const Endpoint workerEndpoint{resolved_.address, quint16(resolved_.port)};
    if (const auto *embedded = std::get_if<EmbeddedRoute>(&candidate_.route)) {
        return lease_ && lease_->isValid() && route.checkedRelay &&
            sameEndpoint(*route.checkedRelay, workerEndpoint) &&
            sameEndpoint(selected.endpoint, {embedded->address, Discovery::BundledSunshinePort});
    }
    return !route.checkedRelay && sameEndpoint(selected.endpoint, workerEndpoint);
}

QVariant SessionSupervisor::privacyScreen() const
{
    if (phase_ != Phase::Streaming || !privacyRoute_ || !privacyState_ ||
        *privacyState_ == PrivacyState::Unavailable) {
        return {};
    }
    return QVariantMap{{QStringLiteral("value"), *privacyState_ == PrivacyState::On},
                       {QStringLiteral("writable"), privacyRequest_.isNull()}};
}

QString SessionSupervisor::privacyScreenMessage() const
{
    if (phase_ != Phase::Streaming || !privacyRoute_)
        return {};
    if (!privacyRequest_.isNull())
        return privacyChanging_ ? tr("正在切换隐私屏保…") : tr("正在检查隐私屏保…");
    if (!privacyState_)
        return tr("隐私屏保状态未确认，重新打开控制中心可重试。");
    if (*privacyState_ == PrivacyState::Unavailable)
        return tr("当前会话不支持隐私屏保");
    return {};
}

void SessionSupervisor::refreshPrivacyScreen()
{
    startPrivacy(Operation::PrivacyStatus);
}

void SessionSupervisor::setPrivacyScreen(bool enabled)
{
    if (privacyState_ == PrivacyState::Off && enabled)
        startPrivacy(Operation::PrivacyOn);
    else if (privacyState_ == PrivacyState::On && !enabled)
        startPrivacy(Operation::PrivacyOff);
}

void SessionSupervisor::startPrivacy(Operation operation)
{
    if (phase_ != Phase::Streaming || !privacyRoute_ || !privacyRequest_.isNull() ||
        !worker_ || worker_->state() != QProcess::Running || !surface_.videoReady()) {
        return;
    }

    const auto token = QUuid::createUuid();
    const auto attempt = attempt_;
    Control::Authorization grant{token, operation, executable_, privacyRoute_->selected, privacyRoute_->revalidated,
        privacyRoute_->checkedRelay, lease_, [this, attempt](const QUuid &current) {
            return attempt_ == attempt && privacyRequest_ == current && privacyRoute_ &&
                phase_ == Phase::Streaming && surface_.videoReady() && worker_ && worker_->state() == QProcess::Running;
        }};
    if (privacyControl_->start(std::move(grant))) {
        privacyRequest_ = token;
        privacyChanging_ = operation != Operation::PrivacyStatus;
        emit privacyScreenChanged();
    }
}

void SessionSupervisor::privacyFinished(const QUuid &token, const Control::Result &result)
{
    if (token != privacyRequest_)
        return;
    privacyRequest_ = {};
    if (pendingFinish_) {
        completeFinish();
        return;
    }
    if (phase_ == Phase::Streaming && privacyRoute_ && surface_.videoReady() &&
        worker_ && worker_->state() == QProcess::Running) {
        privacyState_ = result.status == Control::Status::PrivacyReported ? result.privacy : std::nullopt;
        emit privacyScreenChanged();
    }
}

void SessionSupervisor::revokePrivacy()
{
    if (!privacyRoute_)
        return;
    privacyRoute_.reset();
    privacyState_.reset();
    if (!privacyRequest_.isNull())
        privacyControl_->cancel(privacyRequest_);
    emit privacyScreenChanged();
}

} // namespace Aurora::Session
