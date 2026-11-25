// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "adaptors/moonlight/MoonlightPin.h"
#include "pairing/PairingWire.h"

#include <algorithm>
#include <utility>

namespace {
using Aurora::Discovery::StreamHost;
using Aurora::Pairing::RequestKind;
using Aurora::Pairing::Client::TailnetExchange;
using GrantStatus = Aurora::Pairing::Client::Status;

constexpr int grantRenewalMs = 3 * 60 * 1000 + 30000;
constexpr int grantRetryMs = 5000;

}

void NativeDesktop::initializePairingClient()
{
    pairingClient = std::make_unique<TailnetExchange>(tailnet.get());
    connect(pairingClient.get(), &TailnetExchange::finished, this, &NativeDesktop::pairingFinished);
    clientGrantRenewal.setSingleShot(true);
    connect(&clientGrantRenewal, &QTimer::timeout, this, &NativeDesktop::renewClientGrant);
}

bool NativeDesktop::canPairOverRoute(const StreamHost &route) const
{
    if (route.viaEmbeddedTailnet != connection->embedded)
        return false;
    const auto *peer = Aurora::Discovery::findTailnetPeer(&localPresence->network(), route.endpoint.address);
    return peer && peer->id == connection->peerId;
}

bool NativeDesktop::beginPairing()
{
    if (!ownsConnection() || connection->pairingAttempted || connection->clientGrantRequired ||
        clientGrantCache.knowsPeer(connection->peerId) ||
        tailnetConfirmation || tailnetVerification || !connection->selected || !connection->prepared)
        return false;

    if (!canPairOverRoute(*connection->selected)) {
        const auto tailnetRoute = std::find_if(connection->routes.cbegin(), connection->routes.cend(), [this](const StreamHost &route) {
            return canPairOverRoute(route) && Aurora::Discovery::sameHostIdentity(route.id, connection->selected->id);
        });
        if (tailnetRoute != connection->routes.cend())
            connection->preferAfterPairing = connection->selected;
        return false;
    }

    const auto &prepared = *connection->prepared;
    if (prepared.generation != tailnetGeneration || prepared.route.viaEmbeddedTailnet != connection->embedded)
        return false;
    return beginTailscaleExchange(connection->selected->endpoint.address, RequestKind::Pair);
}

bool NativeDesktop::beginHostIdentity(RequestKind kind)
{
    if (!ownsConnection() || tailnetConfirmation || tailnetVerification)
        return false;
    if (kind == RequestKind::HostRoutes && !connection->lookingForPublicFallback && connection->prepared &&
        connection->prepared->verifiesHostIdentity && (!connection->clientGranted || connection->recoveringRoute) &&
        (connection->clientGrantRequired || clientGrantCache.knowsPeer(connection->peerId)) &&
        (connection->recoveringRoute || !clientGrantCache.hasRecentGrant(connection->peerId))) {
        kind = RequestKind::ClientGrant;
    }
    const bool discoveryOnly = (kind == RequestKind::HostRoutes && connection->lookingForPublicFallback) ||
        (kind != RequestKind::Pair && connection->recoveringRoute);
    if (!discoveryOnly && (!connection->prepared || connection->prepared->generation != tailnetGeneration ||
                           !connection->prepared->verifiesHostIdentity))
        return false;

    const auto &network = localPresence->network();
    const auto peer = std::find_if(network.devices.cbegin(), network.devices.cend(),
        [this](const auto &device) { return device.id == connection->peerId; });
    if (peer == network.devices.cend())
        return false;

    const auto addresses = Aurora::Discovery::tailnetAddresses(*peer);
    for (const auto &address : addresses) {
        if ((!connection->embedded || address.protocol() == QAbstractSocket::IPv4Protocol) &&
            Aurora::Discovery::findTailnetPeer(&network, address) == &*peer) {
            return beginTailscaleExchange(address, kind);
        }
    }
    return false;
}

void NativeDesktop::renewClientGrant()
{
    if (!ownsConnection() || !connection->clientGranted ||
        sessionSupervisor->phase() != Aurora::Session::SessionSupervisor::Phase::Streaming)
        return;
    if (connection->pendingRouteSwitch || !beginHostIdentity(RequestKind::ClientGrant))
        scheduleClientGrantRenewal(grantRetryMs);
}

void NativeDesktop::scheduleClientGrantRenewal(int delayMs)
{
    if (!connection)
        return;
    connection->clientGrantDue.setRemainingTime(delayMs);
    clientGrantRenewal.start(delayMs);
}

bool NativeDesktop::beginTailscaleExchange(const QHostAddress &peerAddress, RequestKind kind)
{
    if (pairingAttempt || !ownsConnection() || !connection->selected)
        return false;

    const bool discoveryOnly = (kind == RequestKind::HostRoutes && connection->lookingForPublicFallback) ||
        (kind != RequestKind::Pair && connection->recoveringRoute);
    if (!discoveryOnly && !connection->prepared)
        return false;
    const auto generation = discoveryOnly ? tailnetGeneration : connection->prepared->generation;

    Aurora::Pairing::Client::Request request;
    request.epoch = generation;
    request.peerId = connection->peerId;
    request.verifiedHost = *connection->selected;
    request.verifiedHost.endpoint.address = peerAddress;
    request.leaseLifetime = sessionSupervisor.get();
    request.kind = kind;
    if (connection->prepared) {
        request.leasedRoute = connection->prepared->route;
        request.moonlightProgram = connection->prepared->executable;
        if (kind == RequestKind::ClientGrant) {
            request.leasedRoute.certificateSha256 = connection->certificateSha256.isEmpty()
                ? clientGrantCache.hostFingerprint(connection->peerId) : connection->certificateSha256;
        }
    }

    const auto exchange = pairingClient->start(std::move(request), localPresence->network(),
        connection->account, connection->embedded,
        [this, generation] { return ownsConnection() && tailnetGeneration == generation; });
    if (!exchange)
        return false;

    if (kind == RequestKind::Pair)
        connection->pairingAttempted = true;
    const bool renewal = kind == RequestKind::ClientGrant &&
        sessionSupervisor->phase() == Aurora::Session::SessionSupervisor::Phase::Streaming;
    pairingAttempt.emplace(PairingAttempt{generation, kind, exchange, {}, renewal});
    if (!renewal)
        reportConnection(kind == RequestKind::Pair ? tr("正在与被控端自动配对…") : tr("正在核对被控端证书…"), false);
    return true;
}

void NativeDesktop::pairingFinished(quint64 request, const Aurora::Pairing::Client::Result &result)
{
    if (!pairingAttempt || request != pairingAttempt->exchange)
        return;
    pairingAttempt->exchange = 0;
    const auto cancellation = pairingAttempt->cancellation;
    const auto kind = pairingAttempt->kind;
    const bool renewal = pairingAttempt->renewingGrant;
    const auto interrupted = std::move(pairingAttempt->interruptedPlayback);
    const bool current = ownsConnection() && pairingAttempt->generation == tailnetGeneration &&
        result.status != GrantStatus::AuthorizationChanged && result.status != GrantStatus::LeaseClosed;
    pairingAttempt.reset();
    if (cancellation) {
        cancelConnection(*cancellation);
        if (exitAfterSession)
            requestExit();
        return;
    }
    if (!current) {
        cancelConnection(tr("Tailscale 账号或设备已变化，连接已停止。"));
        return;
    }
    if (interrupted) {
        sessionFinished(connection->attempt, tailnetGeneration, *interrupted);
        return;
    }
    if (renewal) {
        const bool granted = result.status == GrantStatus::ClientGranted && result.pinnedSha256 == connection->certificateSha256;
        scheduleClientGrantRenewal(granted ? grantRenewalMs : grantRetryMs);
        if (granted) {
            clientGrantCache.remember(connection->peerId, result.pinnedSha256, result.grantInstance,
                result.clientCertificateSha256, connection->clientGrantDue);
        } else if (result.status == GrantStatus::Rejected) {
            clientGrantCache.forgetGrant(connection->peerId);
        }
        updateRouteQuality();
        return;
    }
    if (kind != RequestKind::Pair) {
        const bool grant = kind == RequestKind::ClientGrant;
        if (result.status != (grant ? GrantStatus::ClientGranted : GrantStatus::HostIdentityVerified) ||
            result.pinnedSha256.size() != Aurora::Pairing::HostCertificateSha256Size) {
            if (grant) {
                cancelConnection(tr("被控端未授予当前设备串流权限。"));
                return;
            }
            if (connection->recoveringRoute) {
                if (kind == RequestKind::HostRoutes && beginHostIdentity(RequestKind::HostIdentity))
                    return;
                cancelConnection(tr("无法通过 Tailscale 重新确认被控端，串流未恢复。"));
                return;
            }
            if (connection->lookingForPublicFallback) {
                cancelConnection(tr("Tailscale 串流路径不可用，未取得已验证的公网 IPv6 候选。"));
                return;
            }
            if (kind == RequestKind::HostRoutes) {
                if (canPairOverRoute(*connection->selected)) {
                    verifyMoonlightTrust();
                    return;
                }
                if (beginHostIdentity(RequestKind::HostIdentity))
                    return;
            }
            failRoute(tr("无法通过 Tailscale 确认被控端证书。"), true);
            return;
        }
        if (!connection->resumeCertificateSha256.isEmpty() &&
            connection->resumeCertificateSha256 != result.pinnedSha256) {
            failRoute(tr("被控端证书已变化，无法恢复串流。"), false);
            return;
        }
        connection->certificateSha256 = result.pinnedSha256;
        if (result.supportsClientGrant) {
            connection->clientGrantRequired = true;
            clientGrantCache.noteSupport(connection->peerId, result.pinnedSha256);
        }
        if (grant) {
            connection->clientGranted = true;
            scheduleClientGrantRenewal(grantRenewalMs);
            clientGrantCache.remember(connection->peerId, result.pinnedSha256, result.grantInstance,
                result.clientCertificateSha256, connection->clientGrantDue);
        } else if (result.supportsClientGrant && connection->prepared && !connection->clientGranted) {
            const auto renewal = clientGrantCache.reuse(connection->peerId, result.pinnedSha256, result.grantInstance,
                Aurora::Stream::readClientFingerprint(connection->prepared->executable));
            if (renewal) {
                connection->clientGranted = true;
                connection->clientGrantDue = *renewal;
            }
        }
        if (kind == RequestKind::HostRoutes || grant) {
            connection->requestedHostRoutes = true;
            connection->publicCertificateSha256 = result.pinnedSha256;
            connection->publicRoutes.clear();
            for (const auto &address : result.publicIpv6) {
                if (address != connection->selected->endpoint.address) {
                    connection->publicRoutes.append({{address, Aurora::Discovery::BundledSunshinePort},
                        connection->selected->id, connection->selected->name, false, false, false, false, true});
                }
            }
        }
        if (connection->recoveringRoute) {
            beginRecoveryRouteQuality();
        } else if (connection->lookingForPublicFallback) {
            connection->lookingForPublicFallback = false;
            for (const auto &route : connection->publicRoutes)
                connection->routes.enqueue(route);
            if (connection->routes.isEmpty()) {
                cancelConnection(tr("未发现已认证的公网 IPv6 候选。"));
                return;
            }
            prepareNextRoute();
        } else {
            verifyMoonlightTrust();
        }
        return;
    }
    if (result.status != Aurora::Pairing::Client::Status::Paired) {
        cancelConnection(tr("未完成自动配对。请检查被控端是否运行 Aurora 并允许同账号设备连接。"));
        return;
    }
    if (connection->preferAfterPairing) {
        connection->routes.prepend(*connection->selected);
        connection->routes.prepend(*std::exchange(connection->preferAfterPairing, std::nullopt));
        failRoute(tr("正在重新检查局域网连接…"), true);
        return;
    }
    launchPreparedSession();
}
