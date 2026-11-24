// SPDX-License-Identifier: GPL-3.0-only
#include "ProviderSelection.h"

namespace aurora::tailscale {

ProviderMode selectWindowsProvider(QString savedPreference, bool hasSystemRuntime, bool supportsEmbedded)
{
    if (savedPreference.startsWith(QChar(0xfeff)))
        savedPreference.remove(0, 1);
    savedPreference = savedPreference.trimmed();
    if (savedPreference == QLatin1String("System") && hasSystemRuntime)
        return ProviderMode::System;
    if (savedPreference == QLatin1String("Embedded") && supportsEmbedded)
        return ProviderMode::Embedded;
    return !hasSystemRuntime && supportsEmbedded ? ProviderMode::Embedded : ProviderMode::System;
}

} // namespace aurora::tailscale
