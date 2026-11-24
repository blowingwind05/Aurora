// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Discovery.h"

#include <QByteArray>

namespace Aurora::Discovery::ServerInfo {

inline constexpr char NvstreamService[] = "_nvstream._tcp.local.";
inline constexpr int MaxDocumentSize = 32768;
inline constexpr int ProbeTimeoutMs = 1100;
inline constexpr int BatchConcurrency = 8;
inline constexpr int EncoderIntervalMs = 300;
inline constexpr int EncoderTimeoutMs = 30000;

std::optional<StreamHost> parse(const QByteArray &xml, const Endpoint &remote, bool viaEmbeddedTailnet = false);

} // namespace Aurora::Discovery::ServerInfo
