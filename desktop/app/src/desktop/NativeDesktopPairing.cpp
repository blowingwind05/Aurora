// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "pairing/PairingWire.h"

#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>

#include <algorithm>
#include <array>
#include <utility>

namespace {
using Aurora::Discovery::StreamHost;
using Aurora::Pairing::BridgeKeySize;
using Aurora::Pairing::Client::Coordinator;
using aurora::tailscale::EmbeddedStatusClient;
using aurora::tailscale::StatusError;

QByteArray newBridgeKey()
{
    std::array<quint32, BridgeKeySize / sizeof(quint32)> words;
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    return QByteArray(reinterpret_cast<const char *>(words.data()), BridgeKeySize);
}
}

void NativeDesktop::initializePairingClient()
{
    pairingClient = std::make_unique<Coordinator>();
    connect(pairingClient.get(), &Coordinator::finished, this, &NativeDesktop::pairingFinished);
    connect(tailnet.get(), &EmbeddedStatusClient::pairOpenFinished, this, &NativeDesktop::receivePairingLease);
    pairingConnectRetry.setInterval(120);
    connect(&pairingConnectRetry, &QTimer::timeout, this, &NativeDesktop::requestEmbeddedPairing);
#ifdef Q_OS_WIN
    using aurora::tailscale::system::SystemPairingNc;
    systemPairing = std::make_unique<SystemPairingNc>();
    connect(systemPairing.get(), &SystemPairingNc::ready, this, &NativeDesktop::startSystemPairingExchange);
    connect(systemPairing.get(), &SystemPairingNc::finished, this, [this](quint64 request, const auto &) {
        if (!pairingAttempt || pairingAttempt->systemRequest != request)
            return;
        pairingAttempt->systemRequest = 0;
        if (!pairingAttempt->exchange)
            cancelConnection(tr("系统 Tailscale 无法打开自动配对通道。"));
        // The host can close the control channel after approving the PIN.
        // Once the exchange starts, its protocol and trust checks own the result.
    });
#endif
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
    if (!ownsConnection() || connection->pairingAttempted ||
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
#ifndef Q_OS_WIN
    if (!connection->embedded)
        return false;
#endif
    connection->pairingAttempted = true;
    pairingAttempt.emplace(PairingAttempt{prepared.generation, connection->embedded ? newBridgeKey() : QByteArray{}});
    pairingClient->setAuthorization(prepared.generation, true);
    reportConnection(tr("正在与被控端自动配对…"), false);
    if (connection->embedded) {
        pairingConnectRetry.start();
        requestEmbeddedPairing();
    }
#ifdef Q_OS_WIN
    else {
        systemPairing->setAuthorization(prepared.generation, connection->account);
        pairingAttempt->systemRequest = systemPairing->start({prepared.generation, connection->account,
            connection->peerId, connection->selected->endpoint.address});
        if (!pairingAttempt->systemRequest)
            cancelConnection(tr("无法启动系统 Tailscale 自动配对。"));
    }
#endif
    return true;
}

void NativeDesktop::requestEmbeddedPairing()
{
    if (!pairingAttempt)
        return;
    if (!ownsConnection() || pairingAttempt->generation != tailnetGeneration) {
        cancelConnection(tr("Tailscale 账号或设备已变化，配对已停止。"));
        return;
    }
    if (pairingAttempt->connectDeadline.hasExpired()) {
        cancelConnection(tr("无法建立自动配对连接。请检查被控端 Aurora 是否允许同账号设备连接。"));
        return;
    }
    if (!pairingAttempt->allocation && !pairingAttempt->lease) {
        pairingAttempt->allocation = tailnet->requestPairOpen(pairingAttempt->generation, connection->peerId,
            connection->selected->endpoint.address, pairingAttempt->key);
    }
}

void NativeDesktop::receivePairingLease(quint64 generation, quint64 request,
                                       const aurora::tailscale::PairingLeaseResult &result)
{
    if (!pairingAttempt || request != pairingAttempt->allocation)
        return;
    pairingAttempt->allocation = 0;
    if (!ownsConnection() || generation != pairingAttempt->generation || generation != tailnetGeneration ||
        result.error != StatusError::None || !result.lease || !result.lease->isValid() || !result.lease->localPort()) {
        cancelConnection(tr("对端 Aurora 暂时无法自动配对。"));
        return;
    }
    pairingAttempt->lease = result.lease;
    auto *socket = new QTcpSocket(this);
    pairingAttempt->socket = socket;
    socket->setProxy(QNetworkProxy::NoProxy);
    connect(socket, &QTcpSocket::connected, this, &NativeDesktop::startPairingExchange);
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        if (pairingAttempt && pairingAttempt->socket == socket && !pairingAttempt->exchange)
            cancelConnection(tr("无法连接自动配对通道。"));
    });
    socket->connectToHost(result.lease->address(), *result.lease->localPort());
}

void NativeDesktop::startPairingExchange()
{
    if (!pairingAttempt || !ownsConnection() || !pairingAttempt->lease || !pairingAttempt->lease->isValid() ||
        pairingAttempt->generation != tailnetGeneration || pairingAttempt->connectDeadline.hasExpired()) {
        cancelConnection(tr("自动配对连接已失效。"));
        return;
    }
    const auto &prepared = *connection->prepared;
    const Aurora::Pairing::Client::Request request{prepared.generation, connection->peerId, *connection->selected,
                                                   prepared.route, sessionSupervisor.get(), prepared.executable};
    const auto started = pairingClient->startLoopback(localPresence->network(), request, pairingAttempt->socket,
        *pairingAttempt->lease->localPort(), std::exchange(pairingAttempt->key, {}));
    pairingAttempt->exchange = started.request;
    if (!started.request) {
        cancelConnection(tr("无法验证自动配对通道或内置串流组件。"));
        return;
    }
    pairingConnectRetry.stop();
}

#ifdef Q_OS_WIN
void NativeDesktop::startSystemPairingExchange(quint64 id, QIODevice *channel)
{
    if (!pairingAttempt || id != pairingAttempt->systemRequest)
        return;
    if (!ownsConnection() || connection->embedded || pairingAttempt->generation != tailnetGeneration) {
        cancelConnection(tr("系统 Tailscale 账号或设备已变化，配对已停止。"));
        return;
    }
    const auto &prepared = *connection->prepared;
    const Aurora::Pairing::Client::Request request{prepared.generation, connection->peerId, *connection->selected,
                                                   prepared.route, sessionSupervisor.get(), prepared.executable};
    const auto started = pairingClient->startStdio(localPresence->network(), request, channel, channel);
    pairingAttempt->exchange = started.request;
    if (!started.request)
        cancelConnection(tr("无法验证系统 Tailscale 配对通道。"));
}
#endif

void NativeDesktop::pairingFinished(quint64 request, const Aurora::Pairing::Client::Result &result)
{
    if (!pairingAttempt || request != pairingAttempt->exchange)
        return;
    pairingAttempt->exchange = 0;
    const auto cancellation = pairingAttempt->cancellation;
    const bool ready = result.status == Aurora::Pairing::Client::Status::Paired &&
        ownsConnection() && (!connection->embedded || (pairingAttempt->lease && pairingAttempt->lease->isValid())) &&
        pairingAttempt->generation == tailnetGeneration;
    releasePairingTransport();
    if (cancellation) {
        cancelConnection(*cancellation);
        if (exitAfterSession)
            requestExit();
        return;
    }
    if (!ready) {
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

void NativeDesktop::releasePairingTransport()
{
    if (!pairingAttempt)
        return;
    auto attempt = std::exchange(pairingAttempt, std::nullopt);
    pairingConnectRetry.stop();
    pairingClient->setAuthorization(attempt->generation, false);
    if (attempt->allocation)
        tailnet->cancelPairing(attempt->allocation);
    if (attempt->socket) {
        disconnect(attempt->socket, nullptr, this, nullptr);
        attempt->socket->abort();
        attempt->socket->deleteLater();
    }
    if (attempt->lease)
        attempt->lease->close();
#ifdef Q_OS_WIN
    systemPairing->setAuthorization(attempt->generation, std::nullopt);
#endif
    attempt->key.fill('\0');
}
