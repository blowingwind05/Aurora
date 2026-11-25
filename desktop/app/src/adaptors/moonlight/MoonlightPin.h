// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MoonlightCommand.h"

namespace Aurora::Stream {

QByteArray readPin(const Command &command, const QString &hostId);
QByteArray readClientFingerprint(const QString &executable);

} // namespace Aurora::Pairing::Client::Detail
