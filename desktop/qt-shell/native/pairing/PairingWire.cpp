// SPDX-License-Identifier: GPL-3.0-only
#include "PairingWire.h"

#include <QMessageAuthenticationCode>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace Aurora::Pairing {
namespace {
const auto proxyMagic = QByteArrayLiteral("\r\n\r\n\0\r\nQUIT\n");

QByteArray proof(const QByteArray &purpose, const QByteArray &key, const QByteArray &challenge,
                 const QByteArray &payload = {})
{
    if (key.size() != BridgeKeySize || challenge.size() != BridgeKeySize)
        return {};
    return QMessageAuthenticationCode::hash(purpose + challenge + payload, key, QCryptographicHash::Sha256);
}

bool matchesProof(const QByteArray &first, const QByteArray &second)
{
    if (first.size() != BridgeKeySize || second.size() != BridgeKeySize)
        return false;
    unsigned char difference = 0;
    for (int i = 0; i < BridgeKeySize; ++i)
        difference |= static_cast<unsigned char>(first.at(i) ^ second.at(i));
    return difference == 0;
}

QHostAddress addressFromBytes(const char *bytes, int length)
{
    if (length == 4)
        return QHostAddress(qFromBigEndian<quint32>(bytes));
    static const unsigned char mappedPrefix[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    if (std::memcmp(bytes, mappedPrefix, sizeof mappedPrefix) == 0)
        return QHostAddress(qFromBigEndian<quint32>(bytes + sizeof mappedPrefix));
    Q_IPV6ADDR address{};
    std::memcpy(address.c, bytes, sizeof address.c);
    return QHostAddress(address);
}

bool isDigit(char value) { return value >= '0' && value <= '9'; }
}

QByteArray requestFrame()
{
    return QByteArrayLiteral("AURORA/PAIR/1\n");
}

QByteArray dialProof(const QByteArray &key, const QByteArray &challenge)
{
    return proof(QByteArrayLiteral("AURORA/PAIR/DIAL/1"), key, challenge);
}

Offer parseOffer(const QByteArray &frame)
{
    if (frame.size() == 1 && frame[0] == 0)
        return {Offer::State::Rejected, {}, {}};
    if (frame.size() != 37 || frame[0] != 1 ||
        !std::all_of(frame.constBegin() + 1, frame.constBegin() + 33, [](char value) {
            return isDigit(value) || (value >= 'a' && value <= 'f');
        }) || !std::all_of(frame.constBegin() + 33, frame.constEnd(), isDigit))
        return {};
    return {Offer::State::Ready, frame.mid(1, 32), frame.right(4)};
}

std::optional<EmbeddedSources> verifyEmbeddedSources(const QByteArray &frame, const QByteArray &key,
                                                      const QByteArray &challenge)
{
    const auto family = frame.isEmpty() ? 0 : frame[0];
    const int peerLength = family == 4 ? 4 : family == 6 ? 16 : 0;
    const int payloadSize = 1 + peerLength + 4;
    if (peerLength == 0 || frame.size() != payloadSize + BridgeKeySize ||
        !matchesProof(frame.right(BridgeKeySize),
                      proof(QByteArrayLiteral("AURORA/PAIR/HOST/1"), key, challenge, frame.left(payloadSize))))
        return std::nullopt;

    const auto peer = addressFromBytes(frame.constData() + 1, peerLength);
    const auto sunshine = addressFromBytes(frame.constData() + 1 + peerLength, 4);
    if (!sunshine.isLoopback() || peer.isLoopback())
        return std::nullopt;
    return EmbeddedSources{peer, sunshine};
}

std::optional<ProxySource> parseProxySource(const QByteArray &frame)
{
    if (frame.size() < 16 || !frame.startsWith(proxyMagic) || frame[12] != 0x21)
        return std::nullopt;
    const int addressLength = frame[13] == 0x11 ? 4 : frame[13] == 0x21 ? 16 : 0;
    const auto size = qFromBigEndian<quint16>(frame.constData() + 14);
    if (!addressLength || size < addressLength * 2 + 4 || size > 512 || frame.size() != size + 16)
        return std::nullopt;
    const auto *addresses = frame.constData() + 16;
    return ProxySource{addressFromBytes(addresses, addressLength),
                       qFromBigEndian<quint16>(addresses + addressLength * 2)};
}

}
