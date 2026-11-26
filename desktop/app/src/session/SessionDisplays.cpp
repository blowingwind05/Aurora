// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"

#include "../../../runtime/stream/AuroraStreamSessionProtocol.h"
#include "MoonlightProcess.h"

#include <algorithm>
#include <utility>

namespace Aurora::Session {
namespace Control = Stream::Control;

QVariantMap SessionSupervisor::displayState() const
{
    auto state = displaySnapshot_;
    state.insert(QStringLiteral("busy"), !displayRequest_.isNull() || displaySwitch_.active());
    state.insert(QStringLiteral("switchTarget"), displaySwitch_.target());
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
        displaySwitch_.active() || (!reading && !privacyAllowsPlaybackReplacement())) {
        if (!reading && displayRequest_.isNull()) {
            displayMessage_ = tr("暂时无法更改显示器，请确认隐私保护已关闭。");
            emit displaysChanged();
        }
        return false;
    }

    if (selecting || operation == QLatin1String("remove")) {
        if (selecting && !displaySnapshot_.value(QStringLiteral("canSelect")).toBool())
            return false;
        const auto target = id.toUInt();
        const auto displays = displaySnapshot_.value(QStringLiteral("displays")).toList();
        const auto display = std::find_if(displays.cbegin(), displays.cend(), [&](const QVariant &value) {
            return value.toMap().value(QStringLiteral("id")).toString() == id;
        });
        if (!target || id != QString::number(target) || display == displays.cend() ||
            !display->toMap().value(selecting ? QStringLiteral("selectable") : QStringLiteral("removable")).toBool())
            return false;
    } else if ((!reading && !creating) || (creating && !displaySnapshot_.value(QStringLiteral("canCreate")).toBool())) {
        return false;
    }

    if (selecting) {
        if (id == displaySnapshot_.value(QStringLiteral("activeDisplayId")).toString())
            return false;
        if (!sendDisplayCommand(StreamSession::DisplayPause)) {
            displayMessage_ = tr("当前串流无法切换显示器。");
            emit displaysChanged();
            return false;
        }
        displaySwitch_.begin(id);
        displayPausePending_ = true;
        displayMessage_.clear();
        const auto attempt = attempt_;
        QTimer::singleShot(2200, this, [this, attempt] {
            if (attempt_ == attempt && displayPausePending_)
                finishDisplaySwitch(tr("主控播放进程未能暂停鼠标，串流已结束。"), true);
        });
        QTimer::singleShot(18050, Qt::PreciseTimer, this, [this, attempt] {
            if (attempt_ == attempt && displaySwitch_.active() && displaySwitch_.expired())
                finishDisplaySwitch(tr("切换显示器超时，串流已结束。"), true);
        });
        emit displaysChanged();
        return true;
    }
    return startDisplayOperation(operation, id);
}

void SessionSupervisor::beginPausedDisplaySwitch(bool paused)
{
    if (!displayPausePending_ || !displaySwitch_.active())
        return;
    displayPausePending_ = false;
    if (!paused || !startDisplayOperation(QStringLiteral("select"), displaySwitch_.target()))
        finishDisplaySwitch(tr("当前串流无法切换显示器。"));
}

bool SessionSupervisor::sendDisplayCommand(const QByteArray &command)
{
    return worker_ && worker_->state() == QProcess::Running &&
        worker_->processEnvironment().contains(StreamSession::DisplayChannel) &&
        worker_->write(command) == command.size();
}

bool SessionSupervisor::startDisplayOperation(const QString &operation, const QString &id)
{
    if (!displayControl_ || !displayRequest_.isNull() || !hasWindowsVideo() || !canUseSessionControls())
        return false;
    const auto token = QUuid::createUuid();
    const auto attempt = attempt_;
    const auto &route = *controlRoute_;
    Control::Authorization grant{token, Control::Operation::DisplayControl, executable_, route.selected, route.revalidated,
        route.checkedRelay, lease_, [this, attempt](const QUuid &request) {
            return attempt_ == attempt && displayRequest_ == request && canUseSessionControls() && hasWindowsVideo();
        }, {}, resolved_.certificateSha256, {}, {operation, id.toUInt()}};
    if (!displayControl_->start(std::move(grant)))
        return false;

    displayRequest_ = token;
    displayOperation_ = operation;
    displayMessage_.clear();
    if (operation != QLatin1String("list"))
        emit displaysChanged();
    return true;
}

void SessionSupervisor::displayFinished(const QUuid &token, const Control::Result &result)
{
    if (token != displayRequest_)
        return;
    displayRequest_ = {};
    const auto operation = std::exchange(displayOperation_, {});
    if (pendingFinish_) {
        completeFinish();
        return;
    }
    if (!hasWindowsVideo())
        return;

    const auto snapshot = AuroraDisplayControls::snapshot(result.displaySnapshot);
    if (!snapshot.isEmpty())
        displaySnapshot_ = snapshot;
    if (displaySwitch_.active()) {
        using Progress = AuroraDisplayControls::SwitchProgress;
        const auto progress = displaySwitch_.observe(snapshot);
        if (progress == Progress::Applied)
            return finishDisplaySwitch({});
        if (progress == Progress::Rejected)
            return finishDisplaySwitch(tr("被控端未能切换显示器。"));
        if (progress == Progress::Uncertain)
            return finishDisplaySwitch(tr("无法确认鼠标目标，串流已结束。"), true);
        if (progress == Progress::Present) {
            const QByteArray request = QByteArray(StreamSession::DisplayPresent) +
                QByteArray::number(displaySwitch_.generation()) + ',' + QByteArray::number(displaySwitch_.firstFrame()) + '\n';
            if (!sendDisplayCommand(request))
                return finishDisplaySwitch(tr("无法确认新画面，串流已结束。"), true);
            displaySwitch_.presentationRequested();
        }
        const auto attempt = attempt_;
        QTimer::singleShot(250, this, [this, attempt] {
            if (attempt_ == attempt && displaySwitch_.active())
                startDisplayOperation(QStringLiteral("list"));
        });
        emit displaysChanged();
        return;
    }
    if (!snapshot.isEmpty()) {
        if (snapshot.value(QStringLiteral("status")).toString() != QLatin1String("ok"))
            displayMessage_ = tr("被控端未确认显示器变更。");
    } else if (operation != QLatin1String("list")) {
        const bool refreshing = requestDisplay(QStringLiteral("list"));
        displayMessage_ = refreshing ? tr("正在重新确认显示器状态…") : tr("未收到显示器变更确认。");
    } else if (!displayMessage_.isEmpty()) {
        displayMessage_ = tr("未能重新确认显示器状态。");
    }
    emit displaysChanged();
}

void SessionSupervisor::finishDisplaySwitch(const QString &message, bool uncertain)
{
    displayPausePending_ = false;
    displaySwitch_.clear();
    displayMessage_ = message;
    if (uncertain || !sendDisplayCommand(StreamSession::DisplayRelease)) {
        emit displaysChanged();
        requestStop();
        return;
    }
    emit displaysChanged();
}

void SessionSupervisor::resetDisplays()
{
    if (!displayRequest_.isNull())
        displayControl_->cancel(displayRequest_);
    displayPausePending_ = false;
    displaySwitch_.clear();
    displaySnapshot_.clear();
    displayMessage_.clear();
    emit displaysChanged();
}

} // namespace Aurora::Session
