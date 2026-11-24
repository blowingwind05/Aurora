// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

#include <functional>
#include <memory>

namespace Aurora::Platform {

enum class StartupState { Off, On, OtherLocation, NeedsApproval, Unavailable, ReadFailed };
enum class StartupResult { Ok, Unavailable, MarkerFailed, ReadFailed, WriteFailed, InvalidLauncher };
enum class StartupInitialization { Claimed, AlreadyClaimed, Failed };

class StartupBackend;

class StartupRegistration
{
public:
    // Atomically create the existing per-user startup-initialized marker.
    using ClaimInitialization = std::function<StartupInitialization()>;

    // On Windows pass the installed desktop launcher as an absolute executable path.
    // macOS obtains the launcher from the main application bundle.
    StartupRegistration(ClaimInitialization claim, const QString &windowsLauncherPath);
    StartupRegistration(ClaimInitialization claim, std::unique_ptr<StartupBackend> backend);
    ~StartupRegistration();

    StartupState read() const;
    StartupResult initializeDefault();
    StartupResult change(bool enabled);
    bool initializationFailed() const { return initializationFailed_; }

private:
    StartupResult applyDefault();

    ClaimInitialization claim_;
    std::unique_ptr<StartupBackend> backend_;
    bool initializationFailed_ = false;
};

} // namespace Aurora::Platform
