// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QHostAddress>

#include <optional>

namespace Aurora::Pairing {

inline constexpr quint16 Port = 43826;
inline constexpr int BridgeKeySize = 32;

QByteArray requestFrame();
QByteArray dialProof(const QByteArray &key, const QByteArray &challenge);

struct Offer
{
    enum class State { Invalid, Rejected, Ready };
    State state = State::Invalid;
    QByteArray marker;
    QByteArray pin;
};

Offer parseOffer(const QByteArray &frame);

struct EmbeddedSources
{
    QHostAddress peer;
    QHostAddress sunshineSource;
};

std::optional<EmbeddedSources> verifyEmbeddedSources(const QByteArray &frame, const QByteArray &key,
                                                      const QByteArray &challenge);

struct ProxySource
{
    QHostAddress address;
    quint16 port;
};

std::optional<ProxySource> parseProxySource(const QByteArray &frame);

}
