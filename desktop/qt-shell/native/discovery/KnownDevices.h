// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Discovery.h"

namespace Aurora::Discovery {

// Read the existing account- and node-scoped presence identities.
KnownIdentities readKnownDevices(const QString &dataDirectory, const NetworkSnapshot &network);

// Call after validating live presence against the current account and node.
KnownIdentities rememberKnownDevices(const QString &dataDirectory, const NetworkSnapshot &network,
                                    const QList<PresentDevice> &live);

QUuid readOrCreatePresenceIdentity(const QString &dataDirectory);

}
