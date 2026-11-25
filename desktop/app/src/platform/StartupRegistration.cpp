// SPDX-License-Identifier: GPL-3.0-only
#include "PlatformBackends.h"

#include <utility>

namespace Aurora::Platform {

StartupRegistration::StartupRegistration(ClaimInitialization claim, const QString &windowsLauncherPath)
    : StartupRegistration(std::move(claim), makeStartupBackend(windowsLauncherPath))
{
}

StartupRegistration::StartupRegistration(ClaimInitialization claim, std::unique_ptr<StartupBackend> backend)
    : claim_(std::move(claim)), backend_(std::move(backend))
{
}

StartupRegistration::~StartupRegistration() = default;

StartupState StartupRegistration::read() const
{
    return backend_ && backend_->available() ? backend_->read() : StartupState::Unavailable;
}

StartupResult StartupRegistration::initializeDefault()
{
    const auto result = applyDefault();
    if (result != StartupResult::Ok && result != StartupResult::Unavailable)
        initializationFailed_ = true;
    return result;
}

StartupResult StartupRegistration::applyDefault()
{
    if (!backend_ || !backend_->available())
        return StartupResult::Unavailable;

    const auto claim = claim_ ? claim_() : StartupInitialization::Failed;
    if (claim == StartupInitialization::Failed)
        return StartupResult::MarkerFailed;
    if (claim == StartupInitialization::AlreadyClaimed)
        return StartupResult::Ok;

    const auto state = backend_->read();
    if (state == StartupState::ReadFailed)
        return StartupResult::ReadFailed;
    return state == StartupState::Off ? backend_->setEnabled(true) : StartupResult::Ok;
}

StartupResult StartupRegistration::change(bool enabled)
{
    if (!backend_ || !backend_->available())
        return StartupResult::Unavailable;
    if (!claim_ || claim_() == StartupInitialization::Failed)
        return StartupResult::MarkerFailed;

    const auto result = backend_->setEnabled(enabled);
    if (result == StartupResult::Ok)
        initializationFailed_ = false;
    return result;
}

} // namespace Aurora::Platform
