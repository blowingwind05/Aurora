// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Discovery.h"

namespace Aurora::Discovery {

// Read the existing account- and node-scoped presence identities.
KnownIdentities readKnownDevices(const QString &dataDirectory, const NetworkSnapshot &network);

}
