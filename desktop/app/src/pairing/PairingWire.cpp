// SPDX-License-Identifier: GPL-3.0-only
#include "PairingWire.h"

#include <QMessageAuthenticationCode>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace Aurora::Pairing {
namespace {
const auto proxyMagic = QByteArrayLiteral("\r\n\r\n\0\r\nQUIT\n");
const auto grantCapability = QByteArrayLiteral("AURORA-GRANT/1\0\0");
const auto grantInstanceTag = QByteArrayLiteral("ACG1");
constexpr int grantInstanceSize = 12;

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

bool globalIpv6(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv6Protocol && address.scopeId().isEmpty() &&
        (address.toIPv6Address().c[0] & 0xe0) == 0x20;
}
}

QByteArray requestFrame(RequestKind kind)
{
    // Both requests have the same bounded size, including the newline.
    switch (kind) {
    case RequestKind::Pair: return QByteArrayLiteral("AURORA/PAIR/1\n");
    case RequestKind::HostIdentity: return QByteArrayLiteral("AURORA/CERT/1\n");
    case RequestKind::HostRoutes: return QByteArrayLiteral("AURORA/CERT/2\n");
    case RequestKind::ClientGrant: return QByteArrayLiteral("AURORA/AUTH/1\n");
    }
    return {};
}

std::optional<RequestKind> parseRequestFrame(const QByteArray &frame)
{
    if (frame == requestFrame(RequestKind::Pair))
        return RequestKind::Pair;
    if (frame == requestFrame(RequestKind::HostIdentity))
        return RequestKind::HostIdentity;
    if (frame == requestFrame(RequestKind::HostRoutes))
        return RequestKind::HostRoutes;
    if (frame == requestFrame(RequestKind::ClientGrant))
        return RequestKind::ClientGrant;
    return std::nullopt;
}

QByteArray hostIdentityFrame(const QByteArray &certificateSha256)
{
    return certificateSha256.size() == HostCertificateSha256Size
        ? QByteArray(1, '\2') + certificateSha256 : QByteArray{};
}

QByteArray parseHostIdentityFrame(const QByteArray &frame)
{
    return frame.size() == HostIdentityFrameSize && frame.at(0) == '\2'
        ? frame.mid(1) : QByteArray{};
}

QByteArray hostRoutesFrame(const QByteArray &certificateSha256, const QList<QHostAddress> &addresses,
    bool supportsClientGrant, const QByteArray &grantInstance)
{
    if (certificateSha256.size() != HostCertificateSha256Size)
        return {};

    QByteArray frame(HostRoutesFrameSize, '\0');
    frame[0] = '\3';
    std::memcpy(frame.data() + 1, certificateSha256.constData(), HostCertificateSha256Size);
    // Legacy readers only read the advertised address count and ignore unused slots.
    const int maxRoutes = supportsClientGrant ? HostRouteLimit - 1 : HostRouteLimit;
    QList<QHostAddress> added;
    for (const auto &address : addresses) {
        if (added.size() == maxRoutes)
            break;
        if (!globalIpv6(address) || added.contains(address))
            continue;
        const auto bytes = address.toIPv6Address();
        std::memcpy(frame.data() + HostIdentityFrameSize + 1 + added.size() * 16, bytes.c, 16);
        added.append(address);
    }
    frame[HostIdentityFrameSize] = char(added.size());
    if (supportsClientGrant)
        std::memcpy(frame.data() + frame.size() - grantCapability.size(), grantCapability.constData(), grantCapability.size());
    if (supportsClientGrant && added.size() <= HostRouteLimit - 2 && grantInstance.size() == grantInstanceSize) {
        // An unused address slot can carry a launch identity without consuming any advertised route.
        const auto extension = grantInstanceTag + grantInstance;
        std::memcpy(frame.data() + frame.size() - 32, extension.constData(), extension.size());
    }
    return frame;
}

std::optional<HostRoutes> parseHostRoutesFrame(const QByteArray &frame)
{
    if (frame.size() != HostRoutesFrameSize || frame.at(0) != '\3')
        return std::nullopt;
    const int count = static_cast<unsigned char>(frame.at(HostIdentityFrameSize));
    if (count > HostRouteLimit)
        return std::nullopt;

    HostRoutes result{frame.mid(1, HostCertificateSha256Size), {}};
    result.supportsClientGrant = count < HostRouteLimit && frame.endsWith(grantCapability);
    if (result.supportsClientGrant && count <= HostRouteLimit - 2 &&
        frame.mid(frame.size() - 32, grantInstanceTag.size()) == grantInstanceTag)
        result.grantInstance = frame.mid(frame.size() - 32 + grantInstanceTag.size(), grantInstanceSize);
    for (int index = 0; index < count; ++index) {
        Q_IPV6ADDR bytes{};
        std::memcpy(bytes.c, frame.constData() + HostIdentityFrameSize + 1 + index * 16, 16);
        const QHostAddress address(bytes);
        if (!globalIpv6(address) || result.publicIpv6.contains(address))
            return std::nullopt;
        result.publicIpv6.append(address);
    }
    return result;
}

QByteArray clientGrantInstance(const QString &sunshineSession)
{
    if (sunshineSession.isEmpty())
        return {};
    return QCryptographicHash::hash(QByteArrayLiteral("aurora-grant-instance/1\0") + sunshineSession.toUtf8(),
                                    QCryptographicHash::Sha256).left(grantInstanceSize);
}

QByteArray grantChallenge(const QByteArray &nonce, const QByteArray &hostFingerprint,
    qint64 owner, const QString &tailnet, const QString &hostNode, const QString &clientNode,
    const QList<QHostAddress> &addresses, const QByteArray &grantInstance)
{
    if (nonce.size() != ClientProof::DigestSize || nonce == QByteArray(nonce.size(), '\0') ||
        hostFingerprint.size() != HostCertificateSha256Size || owner <= 0 || hostNode == clientNode)
        return {};
    auto scope = QByteArrayLiteral("aurora-tailnet-scope/1\0");
    for (const auto &part : {QByteArray::number(owner), tailnet.toUtf8(), hostNode.toUtf8(), clientNode.toUtf8()}) {
        if (part.isEmpty() || part.size() > 512 || part.contains('\0'))
            return {};
        const auto length = qToBigEndian(quint16(part.size()));
        scope.append(reinterpret_cast<const char *>(&length), sizeof(length));
        scope.append(part);
    }
    return QByteArray(1, '\4') + nonce + hostFingerprint +
        QCryptographicHash::hash(scope, QCryptographicHash::Sha256) +
        hostRoutesFrame(hostFingerprint, addresses, !grantInstance.isEmpty(), grantInstance);
}

std::optional<ClientGrantOffer> parseGrantChallenge(const QByteArray &frame)
{
    if (frame.size() != GrantChallengeFrameSize || frame.at(0) != '\4')
        return {};
    const auto routes = parseHostRoutesFrame(frame.right(HostRoutesFrameSize));
    const auto challenge = frame.mid(1, ClientProof::ChallengeSize);
    if (!routes || routes->certificateSha256 != challenge.mid(ClientProof::DigestSize, ClientProof::DigestSize))
        return {};
    return ClientGrantOffer{challenge, *routes};
}

QByteArray grantProofFrame(const GrantProof &proof)
{
    if (proof.certificate.isEmpty() || proof.certificate.size() > ClientProof::MaxCertificateBytes ||
        proof.signature.isEmpty() || proof.signature.size() > ClientProof::MaxSignatureBytes)
        return {};
    QByteArray frame(GrantProofHeaderSize, '\0');
    qToBigEndian(quint16(proof.certificate.size()), frame.data());
    qToBigEndian(quint16(proof.signature.size()), frame.data() + 2);
    return frame + proof.certificate + proof.signature;
}

int grantProofFrameSize(const QByteArray &header)
{
    if (header.size() != GrantProofHeaderSize)
        return 0;
    const auto certificate = qFromBigEndian<quint16>(header.constData());
    const auto signature = qFromBigEndian<quint16>(header.constData() + 2);
    if (!certificate || certificate > ClientProof::MaxCertificateBytes ||
        !signature || signature > ClientProof::MaxSignatureBytes)
        return 0;
    return GrantProofHeaderSize + certificate + signature;
}

std::optional<GrantProof> parseGrantProof(const QByteArray &frame)
{
    if (frame.size() < GrantProofHeaderSize || grantProofFrameSize(frame.left(GrantProofHeaderSize)) != frame.size())
        return {};
    const auto certificate = qFromBigEndian<quint16>(frame.constData());
    return GrantProof{frame.mid(GrantProofHeaderSize, certificate), frame.mid(GrantProofHeaderSize + certificate)};
}

QByteArray grantReplyFrame(GrantReply reply)
{
    switch (reply) {
    case GrantReply::Rejected: return QByteArray(1, '\0');
    case GrantReply::Accepted: return QByteArray(1, '\5');
    case GrantReply::Unsupported: return QByteArray(1, '\6');
    }
    return {};
}

std::optional<GrantReply> parseGrantReply(const QByteArray &frame)
{
    for (const auto reply : {GrantReply::Rejected, GrantReply::Accepted, GrantReply::Unsupported}) {
        if (frame == grantReplyFrame(reply))
            return reply;
    }
    return {};
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
