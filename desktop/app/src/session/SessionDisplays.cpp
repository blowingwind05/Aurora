// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"

#include "../../../runtime/stream/qt-session/AuroraDisplayControls.h"
#include "MoonlightProcess.h"

#include <algorithm>
#include <utility>

namespace Aurora::Session {
namespace Control = Stream::Control;

QVariantMap SessionSupervisor::displayState() const
{
    auto state = displaySnapshot_;
    state.insert(QStringLiteral("busy"), !displayRequest_.isNull());
    state.insert(QStringLiteral("creating"), displayOperation_ == QLatin1String("create"));
    state.insert(QStringLiteral("message"), displayMessage_);
    return state;
}

bool SessionSupervisor::requestDisplay(const QString &operation, const QString &id)
{
    const bool reading = operation == QLatin1String("list");
    const bool creating = operation == QLatin1String("create");
    const bool selecting = operation == QLatin1String("select");
    if (!displayControl_ || !hasWindowsVideo() || !canUseSessionControls() || !displayRequest_.isNull() ||
        (!reading && !privacyAllowsPlaybackReplacement())) {
        if (!reading && displayRequest_.isNull()) {
            displayMessage_ = tr("暂时无法更改显示器，请确认隐私保护已关闭。");
            emit displaysChanged();
        }
        return false;
    }

    quint32 target = 0;
    if (selecting || operation == QLatin1String("remove")) {
        if (selecting && !displaySnapshot_.value(QStringLiteral("canSelect")).toBool())
            return false;
        target = id.toUInt();
        const auto displays = displaySnapshot_.value(QStringLiteral("displays")).toList();
        const auto display = std::find_if(displays.cbegin(), displays.cend(), [&](const QVariant &value) {
            return value.toMap().value(QStringLiteral("id")).toString() == id;
        });
        if (!target || id != QString::number(target) || display == displays.cend() ||
            !display->toMap().value(selecting ? QStringLiteral("ready") : QStringLiteral("removable")).toBool())
            return false;
    } else if ((!reading && !creating) || (creating && !displaySnapshot_.value(QStringLiteral("canCreate")).toBool())) {
        return false;
    }

    const auto token = QUuid::createUuid();
    const auto attempt = attempt_;
    const auto &route = *controlRoute_;
    Control::Authorization grant{token, Control::Operation::DisplayControl, executable_, route.selected, route.revalidated,
        route.checkedRelay, lease_, [this, attempt](const QUuid &request) {
            return attempt_ == attempt && displayRequest_ == request && canUseSessionControls() && hasWindowsVideo();
        }, {}, resolved_.certificateSha256, {}, {operation, target}};
    if (!displayControl_->start(std::move(grant)))
        return false;

    displayRequest_ = token;
    displayOperation_ = operation;
    displayId_ = id;
    displayMessage_.clear();
    if (!reading)
        emit displaysChanged();
    return true;
}

void SessionSupervisor::displayFinished(const QUuid &token, const Control::Result &result)
{
    if (token != displayRequest_)
        return;
    displayRequest_ = {};
    const auto operation = std::exchange(displayOperation_, {});
    const auto requestedId = std::exchange(displayId_, {});
    if (pendingFinish_) {
        completeFinish();
        return;
    }
    if (!hasWindowsVideo())
        return;

    const auto snapshot = AuroraDisplayControls::snapshot(result.displaySnapshot);
    if (!snapshot.isEmpty()) {
        displaySnapshot_ = snapshot;
        if (snapshot.value(QStringLiteral("status")).toString() != QLatin1String("ok") ||
            (operation == QLatin1String("select") && snapshot.value(QStringLiteral("activeDisplayId")).toString() != requestedId))
            displayMessage_ = tr("被控端未确认显示器变更。");
    } else if (operation != QLatin1String("list")) {
        const bool refreshing = requestDisplay(QStringLiteral("list"));
        displayMessage_ = refreshing ? tr("正在重新确认显示器状态…") : tr("未收到显示器变更确认。");
    } else if (!displayMessage_.isEmpty()) {
        displayMessage_ = tr("未能重新确认显示器状态。");
    }
    emit displaysChanged();
}

void SessionSupervisor::resetDisplays()
{
    if (!displayRequest_.isNull())
        displayControl_->cancel(displayRequest_);
    displaySnapshot_.clear();
    displayMessage_.clear();
    emit displaysChanged();
}

} // namespace Aurora::Session
