// SPDX-License-Identifier: GPL-3.0-only
#include "PlatformBackends.h"

#include <QThread>

#include <utility>

namespace Aurora::Platform {

std::optional<HostSleepMode> parseHostSleepMode(const QString &token)
{
    if (token == QStringLiteral("System"))
        return HostSleepMode::System;
    if (token == QStringLiteral("OnAc"))
        return HostSleepMode::OnAc;
    if (token == QStringLiteral("Always"))
        return HostSleepMode::Always;
    return std::nullopt;
}

QString hostSleepModeToken(HostSleepMode mode)
{
    switch (mode) {
    case HostSleepMode::System: return QStringLiteral("System");
    case HostSleepMode::OnAc: return QStringLiteral("OnAc");
    case HostSleepMode::Always: return QStringLiteral("Always");
    }
    return {};
}

HostSleepPrevention::HostSleepPrevention(HostSleepMode initialMode)
    : HostSleepPrevention(initialMode, makeHostSleepBackend())
{
}

HostSleepPrevention::HostSleepPrevention(HostSleepMode initialMode, std::unique_ptr<HostSleepBackend> backend)
    : backend_(std::move(backend)), mode_(initialMode)
{
    refreshTimer_.setInterval(2000);
    connect(&refreshTimer_, &QTimer::timeout, this, &HostSleepPrevention::refreshAssertion);
}

HostSleepPrevention::~HostSleepPrevention()
{
    setHostActive(false);
}

void HostSleepPrevention::applyMode(HostSleepMode mode)
{
    Q_ASSERT(QThread::currentThread() == thread());
    mode_ = mode;
    update();
}

void HostSleepPrevention::setHostActive(bool active)
{
    Q_ASSERT(QThread::currentThread() == thread());
    hostActive_ = active;
    update();
}

void HostSleepPrevention::update()
{
    if (!backend_)
        return;

    const bool enabled = hostActive_ && mode_ != HostSleepMode::System;
    if (enabled && !resumeMonitoring_) {
        resumeMonitoring_ = backend_->startResumeMonitoring();
    } else if (!enabled && resumeMonitoring_) {
        backend_->stopResumeMonitoring();
        resumeMonitoring_ = false;
    }

    const bool poll = enabled && (mode_ == HostSleepMode::OnAc || resumeMonitoring_);
    if (poll && !refreshTimer_.isActive())
        refreshTimer_.start();
    if (!poll)
        refreshTimer_.stop();
    refreshAssertion();
}

void HostSleepPrevention::refreshAssertion()
{
    if (resumeMonitoring_ && backend_->takeResume())
        releaseAssertion();

    const bool required = hostActive_ && (mode_ == HostSleepMode::Always
        || (mode_ == HostSleepMode::OnAc && backend_->onAcPower()));
    if (required && !assertionHeld_)
        assertionHeld_ = backend_->acquireAssertion();
    if (!required)
        releaseAssertion();
}

void HostSleepPrevention::releaseAssertion()
{
    if (!assertionHeld_)
        return;
    backend_->releaseAssertion();
    assertionHeld_ = false;
}

} // namespace Aurora::Platform
