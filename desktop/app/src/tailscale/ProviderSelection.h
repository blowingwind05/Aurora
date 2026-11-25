// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

namespace aurora::tailscale {

enum class ProviderMode { System, Embedded };

// Applies the old Windows startup rule to already-read text and capabilities.
ProviderMode selectWindowsProvider(QString savedPreference, bool hasSystemRuntime, bool supportsEmbedded);

} // namespace aurora::tailscale
