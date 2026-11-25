// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>

#include "../../../protocol/identity/ClientProof.h"

#include <optional>

namespace Aurora::Pairing {

inline constexpr quint16 Port = 43826;
inline constexpr int BridgeKeySize = 32;

enum class RequestKind { Pair, HostIdentity, HostRoutes, ClientGrant };
inline constexpr int HostCertificateSha256Size = 32;
inline constexpr int HostIdentityFrameSize = 1 + HostCertificateSha256Size;
inline constexpr int HostRouteLimit = 4;
inline constexpr int HostRoutesFrameSize = HostIdentityFrameSize + 1 + HostRouteLimit * 16;

struct HostRoutes
{
    QByteArray certificateSha256;
    QList<QHostAddress> publicIpv6;
    bool supportsClientGrant = false;
    QByteArray grantInstance;
};

QByteArray requestFrame(RequestKind kind = RequestKind::Pair);
std::optional<RequestKind> parseRequestFrame(const QByteArray &frame);
QByteArray hostIdentityFrame(const QByteArray &certificateSha256);
QByteArray parseHostIdentityFrame(const QByteArray &frame);
QByteArray hostRoutesFrame(const QByteArray &certificateSha256, const QList<QHostAddress> &addresses,
    bool supportsClientGrant = false, const QByteArray &grantInstance = {});
std::optional<HostRoutes> parseHostRoutesFrame(const QByteArray &frame);
QByteArray clientGrantInstance(const QString &sunshineSession);

inline constexpr int GrantChallengeFrameSize = 1 + ClientProof::ChallengeSize + HostRoutesFrameSize;
inline constexpr int GrantProofHeaderSize = 4;
inline constexpr int MaxGrantProofFrameSize = GrantProofHeaderSize +
    ClientProof::MaxCertificateBytes + ClientProof::MaxSignatureBytes;

enum class GrantReply { Rejected, Accepted, Unsupported };
struct GrantProof {
    QByteArray certificate;
    QByteArray signature;
};
struct ClientGrantOffer {
    QByteArray challenge;
    HostRoutes routes;
};

QByteArray grantChallenge(const QByteArray &nonce, const QByteArray &hostFingerprint,
    qint64 owner, const QString &tailnet, const QString &hostNode, const QString &clientNode,
    const QList<QHostAddress> &addresses = {}, const QByteArray &grantInstance = {});
std::optional<ClientGrantOffer> parseGrantChallenge(const QByteArray &frame);
QByteArray grantProofFrame(const GrantProof &proof);
int grantProofFrameSize(const QByteArray &header);
std::optional<GrantProof> parseGrantProof(const QByteArray &frame);
QByteArray grantReplyFrame(GrantReply reply);
std::optional<GrantReply> parseGrantReply(const QByteArray &frame);

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
