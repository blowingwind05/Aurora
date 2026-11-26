// SPDX-License-Identifier: GPL-3.0-only
#include "PlatformBackends.h"

namespace Aurora::Platform {

std::unique_ptr<StartupBackend> makeStartupBackend(const QString &)
{
    return {};
}

std::unique_ptr<HostSleepBackend> makeHostSleepBackend()
{
    return {};
}

QVariantMap highContrastToolTipColors()
{
    return {};
}

} // namespace Aurora::Platform
