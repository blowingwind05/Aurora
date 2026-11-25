// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../../stream/MoonlightCommand.h"

namespace Aurora::Pairing::Client::Detail {

QByteArray readPin(const Stream::Command &command, const QString &hostId);

} // namespace Aurora::Pairing::Client::Detail
