// SPDX-License-Identifier: GPL-3.0-only
#include "PairingWire.h"

using namespace Aurora::Pairing;

int main()
{
    const auto key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto challenge = QByteArray::fromHex("1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100");
    if (requestFrame() != "AURORA/PAIR/1\n" || dialProof(key, challenge).toHex() !=
        "5a2940d24084f30d4d32db9027b87b027cdf3b94b30bfbb6c9d57ec80a7aef0e")
        return 1;

    const auto signedSources = QByteArray::fromHex(
        "04644000027f000002a08a39496a22387a52240304fcf5fb5df12a3bf46262348d38d02e207eb52092");
    const auto sources = verifyEmbeddedSources(signedSources, key, challenge);
    if (!sources || sources->peer.toString() != QStringLiteral("100.64.0.2") ||
        sources->sunshineSource.toString() != QStringLiteral("127.0.0.2") ||
        verifyEmbeddedSources(signedSources, key, QByteArray(32, 0)) ||
        verifyEmbeddedSources(signedSources.chopped(1), key, challenge))
        return 2;

    const auto proxy = QByteArray::fromHex("0d0a0d0a000d0a515549540a2111000c644000027f00000212345678");
    const auto endpoint = parseProxySource(proxy);
    if (!endpoint || endpoint->address.toString() != QStringLiteral("100.64.0.2") ||
        endpoint->port != 0x1234 || parseProxySource(proxy.chopped(1)))
        return 3;

    const auto marker = QByteArrayLiteral("00112233445566778899aabbccddeeff");
    const auto accepted = QByteArray(1, 1) + marker + "0427";
    const auto offer = parseOffer(accepted);
    if (offer.state != Offer::State::Ready || offer.marker != marker || offer.pin != "0427" ||
        parseOffer(QByteArray(1, 0)).state != Offer::State::Rejected ||
        parseOffer(accepted.chopped(1) + "x").state != Offer::State::Invalid)
        return 4;
    const auto identity = hostIdentityFrame(key);
    if (requestFrame().size() != requestFrame(RequestKind::HostIdentity).size() ||
        parseRequestFrame(requestFrame()) != RequestKind::Pair ||
        parseRequestFrame(requestFrame(RequestKind::HostIdentity)) != RequestKind::HostIdentity ||
        parseRequestFrame(requestFrame(RequestKind::HostIdentity) + "x") ||
        parseHostIdentityFrame(identity) != key ||
        !parseHostIdentityFrame(accepted).isEmpty() ||
        !parseHostIdentityFrame(identity + "x").isEmpty() ||
        !hostIdentityFrame(key.chopped(1)).isEmpty())
        return 5;
    const QHostAddress global(QStringLiteral("2001:db8::56"));
    const auto advertised = hostRoutesFrame(key, {QHostAddress::LocalHostIPv6, QHostAddress(QStringLiteral("fe80::1")),
        QHostAddress(QStringLiteral("fd7a:115c:a1e0::21")), global, global});
    const auto routes = parseHostRoutesFrame(advertised);
    auto invalid = advertised;
    invalid[HostIdentityFrameSize] = char(HostRouteLimit + 1);
    if (!routes || routes->certificateSha256 != key || routes->publicIpv6 != QList<QHostAddress>{global} ||
        parseHostRoutesFrame(invalid) || parseHostRoutesFrame(advertised.chopped(1)) ||
        parseRequestFrame(requestFrame(RequestKind::HostRoutes)) != RequestKind::HostRoutes ||
        requestFrame(RequestKind::HostRoutes).size() != requestFrame().size())
        return 6;
    const auto instance = clientGrantInstance(QStringLiteral("host-launch"));
    const auto modern = parseHostRoutesFrame(hostRoutesFrame(key, {global}, true, instance));
    if (!modern || !modern->supportsClientGrant || modern->publicIpv6 != QList<QHostAddress>{global} ||
        modern->grantInstance != instance || instance == clientGrantInstance(QStringLiteral("new-launch")) ||
        routes->supportsClientGrant || !routes->grantInstance.isEmpty())
        return 9;
    const auto threeRoutes = parseHostRoutesFrame(hostRoutesFrame(key, {global, QHostAddress("2001:db8::2"),
        QHostAddress("2001:db8::3")}, true, instance));
    if (!threeRoutes || !threeRoutes->supportsClientGrant || threeRoutes->publicIpv6.size() != 3 ||
        !threeRoutes->grantInstance.isEmpty())
        return 10;

    const auto grant = grantChallenge(challenge, key, 42, QStringLiteral("network"),
        QStringLiteral("host-node"), QStringLiteral("client-node"), {global}, instance);
    const auto grantOffer = parseGrantChallenge(grant);
    auto changedPin = grant;
    if (changedPin.size() == GrantChallengeFrameSize)
        changedPin[1 + int(Aurora::ClientProof::DigestSize)] = char(0xff);
    if (!grantOffer || grantOffer->challenge.size() != Aurora::ClientProof::ChallengeSize ||
        grantOffer->challenge.left(Aurora::ClientProof::DigestSize) != challenge ||
        grantOffer->routes.certificateSha256 != key || grantOffer->routes.publicIpv6 != QList<QHostAddress>{global} ||
        grantOffer->routes.grantInstance != instance ||
        parseGrantChallenge(changedPin) || parseGrantChallenge(grant.chopped(1)) ||
        parseRequestFrame(requestFrame(RequestKind::ClientGrant)) != RequestKind::ClientGrant)
        return 7;

    const auto proof = grantProofFrame({key, challenge});
    const auto decoded = parseGrantProof(proof);
    if (!decoded || decoded->certificate != key || decoded->signature != challenge ||
        parseGrantProof(proof + "extra") || parseGrantProof(proof.chopped(1)) ||
        grantProofFrame({key, QByteArray(Aurora::ClientProof::MaxSignatureBytes + 1, 'x')}).size() != 0 ||
        parseGrantReply(grantReplyFrame(GrantReply::Accepted)) != GrantReply::Accepted ||
        parseGrantReply(grantReplyFrame(GrantReply::Unsupported)) != GrantReply::Unsupported ||
        parseGrantReply(accepted))
        return 8;
    return 0;
}
