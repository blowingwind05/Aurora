// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "HostSleepPrevention.h"
#include "StartupRegistration.h"

namespace Aurora::Platform {

// Native operations used by the controllers. Fixtures can supply in-memory implementations.
class StartupBackend
{
public:
    virtual ~StartupBackend() = default;
    virtual bool available() const = 0;
    virtual StartupState read() const = 0;
    virtual StartupResult setEnabled(bool enabled) = 0;
};

class HostSleepBackend
{
public:
    virtual ~HostSleepBackend() = default;
    virtual bool onAcPower() const = 0;
    virtual bool acquireAssertion() = 0;
    virtual void releaseAssertion() = 0;

    // Windows invalidates requests on explicit sleep. When monitoring starts,
    // the controller polls for resume alongside its power-source checks.
    virtual bool startResumeMonitoring() { return false; }
    virtual void stopResumeMonitoring() {}
    virtual bool takeResume() { return false; }
};

std::unique_ptr<StartupBackend> makeStartupBackend(const QString &windowsLauncherPath);
std::unique_ptr<HostSleepBackend> makeHostSleepBackend();

#ifdef Q_OS_WIN
QString windowsStartupCommand(const QString &launcher);
#endif

} // namespace Aurora::Platform
