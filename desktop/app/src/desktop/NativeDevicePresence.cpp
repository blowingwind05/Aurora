// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDevicePresence.h"
#include "discovery/KnownDevices.h"

#include <QHostInfo>
#include <QNetworkInterface>

#include <algorithm>
#include <utility>

namespace {
using namespace Aurora::Presence;
using namespace Aurora::Discovery;

Platform localPlatform()
{
#ifdef Q_OS_WIN
    return Platform::Windows;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#else
    return Platform::Unknown;
#endif
}

QList<LanSubnet> localLinks()
{
    QList<LanSubnet> result;
    for (const auto &link : QNetworkInterface::allInterfaces()) {
        if (!link.flags().testFlag(QNetworkInterface::IsUp) ||
            (link.type() != QNetworkInterface::Ethernet && link.type() != QNetworkInterface::Wifi))
            continue;
        for (const auto &entry : link.addressEntries()) {
            const LanSubnet subnet{entry.ip(), entry.netmask()};
            if (subnet.isUsable() && !result.contains(subnet))
                result.append(subnet);
        }
    }
    return result;
}

NetworkSnapshot networkFromStatus(const aurora::tailscale::Status &status)
{
    if (!status.account)
        return {};
    NetworkSnapshot result{status.localAddress, status.account->ownerId, status.account->nodeId, {}};
    for (const auto &peer : status.peers) {
        Device device;
        device.id = peer.nodeId;
        device.name = peer.name;
        device.platform = peer.platform;
        device.address = peer.address;
        device.alternateAddress = peer.alternateAddress;
        device.online = peer.online;
        result.devices.append(device);
    }
    return result;
}
}

NativeDevicePresence::NativeDevicePresence(const QString &directory) : directory(directory)
{
    embeddedDeadline.setSingleShot(true);
    routeDeadline.setSingleShot(true);
    connect(&embeddedDeadline, &QTimer::timeout, this, &NativeDevicePresence::finishEmbeddedScan);
    connect(&routeDeadline, &QTimer::timeout, this, [this] { finishRouteSurvey(true); });
    connect(&serverInfo, &ServerInfo::Client::batchFinished, this, &NativeDevicePresence::receiveRouteBatch);
}

void NativeDevicePresence::update(const aurora::tailscale::Status &status, bool embedded, bool scan)
{
    currentNetwork = networkFromStatus(status);
    if (!(account == status.account) || embeddedMode != embedded) {
        finishRouteSurvey(false);
        clearEmbeddedScan();
        account = status.account;
        embeddedMode = embedded;
        ++accountGeneration;
        lanRequest = peersRequest = 0;
        sessionLanPeer.clear();
        lanDevices = {};
        tailnetDevices = {};
    }
    serverInfo.updateDiscovery(accountGeneration, currentNetwork, lanDevices.devices, tailnetDevices.devices, embeddedMode);
    if (!currentNetwork.isAuthenticated()) {
        if (transport) {
            transport->updateAccess({}, nullptr, accountGeneration);
            transport->stopResponding();
        }
        return;
    }

    if (!transport) {
        localIdentity = readOrCreatePresenceIdentity(directory);
        const Announcement announce{localIdentity, QHostInfo::localHostName(), localPlatform(), hosting};
        transport = std::make_unique<UdpPresenceTransport>(announce);
        connect(transport.get(), &UdpPresenceTransport::scanFinished, this, &NativeDevicePresence::receiveScan);
    }
    const auto links = localLinks();
    lanDevices.address = links.isEmpty() ? QHostAddress{} : links.first().address;
    tailnetDevices.known = readKnownDevices(directory, currentNetwork);
    transport->updateAccess(links, embedded ? nullptr : &status, accountGeneration);
    transport->startResponding();
    if (!scan || sessionActive)
        return;

    if (lanRequest)
        transport->cancelScan(lanRequest);
    lanRequest = transport->probeLan();
    if (peersRequest)
        transport->cancelScan(peersRequest);
    peersRequest = 0;
    if (!embedded) {
        clearEmbeddedScan();
        QList<QHostAddress> targets;
        for (const auto &peer : currentNetwork.devices) {
            if (peer.online)
                targets.append(tailnetAddresses(peer));
        }
        peersRequest = transport->probePeers(targets);
    }
}

void NativeDevicePresence::scanEmbedded(aurora::tailscale::EmbeddedStatusClient &client,
                                        const aurora::tailscale::Status &status, quint64 generation)
{
    clearEmbeddedScan();
    if (sessionActive || !status.account || localIdentity.isNull())
        return;
    if (embeddedClient != &client) {
        disconnect(embeddedReplies);
        disconnect(embeddedOpens);
        embeddedClient = &client;
        embeddedOpens = connect(&client, &aurora::tailscale::EmbeddedStatusClient::openFinished,
                                this, &NativeDevicePresence::receiveProbeLease);
        embeddedReplies = connect(&client, &aurora::tailscale::EmbeddedStatusClient::presenceFinished, this,
            [this](quint64 replyGeneration, quint64 request, const aurora::tailscale::PresenceResult &result) {
                if (replyGeneration != embeddedGeneration || !embeddedDeadline.isActive())
                    return;
                const auto it = embeddedProbes.find(request);
                if (it == embeddedProbes.end())
                    return;
                if (result.error == aurora::tailscale::StatusError::None) {
                    const auto response = it.value().decodeResult(QString::fromLatin1(result.packet.toBase64()),
                                                                   embeddedStatus, embeddedGeneration);
                    if (response && std::none_of(embeddedDevices.cbegin(), embeddedDevices.cend(),
                                                [&response](const PresentDevice &known) {
                                                    return known.address == response->address;
                                                })) {
                        const auto &item = response->announcement;
                        embeddedDevices.append({item.id, response->address, item.name, platformName(item.platform), item.hosting});
                    }
                }
                embeddedProbes.erase(it);
                sendEmbeddedProbes();
            });
    }
    embeddedStatus = status;
    embeddedGeneration = generation;
    for (const auto &peer : status.peers) {
        if (!peer.online)
            continue;
        if (!peer.address.isNull())
            embeddedTargets.enqueue({peer.nodeId, peer.address});
        if (!peer.alternateAddress.isNull() && peer.alternateAddress != peer.address)
            embeddedTargets.enqueue({peer.nodeId, peer.alternateAddress});
    }
    embeddedDeadline.start(4000);
    sendEmbeddedProbes();
}

void NativeDevicePresence::sendEmbeddedProbes()
{
    if (!embeddedClient || !embeddedDeadline.isActive())
        return;
    while (embeddedProbes.size() < 8 && !embeddedTargets.isEmpty()) {
        const auto target = embeddedTargets.dequeue();
        const auto probe = EmbeddedProbe::start(embeddedStatus, embeddedGeneration, target.second, localIdentity);
        if (!probe)
            continue;
        const auto request = embeddedClient->requestPresence(embeddedGeneration, target.first, target.second, probe->packet());
        if (!request) {
            embeddedTargets.prepend(target);
            QTimer::singleShot(35, this, &NativeDevicePresence::sendEmbeddedProbes);
            return;
        }
        embeddedProbes.insert(request, *probe);
    }
    if (embeddedTargets.isEmpty() && embeddedProbes.isEmpty())
        finishEmbeddedScan();
}

void NativeDevicePresence::finishEmbeddedScan()
{
    if (embeddedStatus.account == account && currentNetwork.isAuthenticated()) {
        tailnetDevices.devices = embeddedDevices;
        tailnetDevices.known = rememberKnownDevices(directory, currentNetwork, tailnetDevices.devices);
        emit changed();
    }
    clearEmbeddedScan();
    startRouteSurvey();
}

void NativeDevicePresence::clearEmbeddedScan()
{
    embeddedDeadline.stop();
    embeddedProbes.clear();
    embeddedTargets.clear();
    embeddedDevices.clear();
}

void NativeDevicePresence::setHosting(bool ready)
{
    hosting = ready;
    if (transport)
        transport->updateAnnouncement(QHostInfo::localHostName(), localPlatform(), hosting);
}

void NativeDevicePresence::setSessionActive(bool active)
{
    sessionActive = active;
    if (!active) {
        cancelSessionLanProbe(lanRequest);
        return;
    }
    sessionLanPeer.clear();
    if (transport) {
        if (const auto request = std::exchange(lanRequest, 0))
            transport->cancelScan(request);
        if (const auto request = std::exchange(peersRequest, 0))
            transport->cancelScan(request);
    }
    clearEmbeddedScan();
    finishRouteSurvey(false);
}

quint64 NativeDevicePresence::probeSessionLan(const QString &peerId)
{
    if (!sessionActive || !transport || lanRequest || !currentNetwork.isAuthenticated() ||
        tailnetDevices.known.value(peerId).isNull())
        return 0;
    sessionLanPeer = peerId;
    return lanRequest = transport->probeLan();
}

void NativeDevicePresence::cancelSessionLanProbe(quint64 request)
{
    if (!request || request != lanRequest || sessionLanPeer.isEmpty())
        return;
    lanRequest = 0;
    sessionLanPeer.clear();
    transport->cancelScan(request);
}

QByteArray NativeDevicePresence::announcementPacket(bool canHost) const
{
    return announcementTemplate({localIdentity, QHostInfo::localHostName(), localPlatform(), canHost});
}

void NativeDevicePresence::receiveScan(quint64 request, const Aurora::Presence::ScanResult &result)
{
    const bool lan = request == lanRequest;
    if (!lan && request != peersRequest)
        return;
    (lan ? lanRequest : peersRequest) = 0;
    const auto activePeer = lan ? std::exchange(sessionLanPeer, {}) : QString{};
    if (result.status != ScanStatus::Complete || !currentNetwork.isAuthenticated()) {
        if (!activePeer.isEmpty())
            emit sessionLanProbed(request, {});
        return;
    }

    auto &devices = lan ? lanDevices.devices : tailnetDevices.devices;
    devices.clear();
    for (const auto &reply : result.replies) {
        const auto &device = reply.announcement;
        devices.append({device.id, reply.address, device.name, platformName(device.platform), device.hosting});
    }
    if (!activePeer.isEmpty()) {
        // Keep the normal catalog survey suspended while streaming. The reply is only a
        // route hint. The playback coordinator must still prove the exact endpoint over TLS.
        serverInfo.updateDiscovery(accountGeneration, currentNetwork, devices, tailnetDevices.devices, embeddedMode);
        emit sessionLanProbed(request, nearbyHostAddresses(currentNetwork, tailnetDevices.known, activePeer, devices));
        return;
    }
    if (!lan)
        tailnetDevices.known = rememberKnownDevices(directory, currentNetwork, tailnetDevices.devices);
    emit changed();
    startRouteSurvey();
}

void NativeDevicePresence::startRouteSurvey()
{
    if (sessionActive || !currentNetwork.isAuthenticated() || lanRequest || peersRequest ||
        embeddedDeadline.isActive() || routeSurvey)
        return;
    serverInfo.updateDiscovery(accountGeneration, currentNetwork, lanDevices.devices, tailnetDevices.devices, embeddedMode);
    routeSurvey.emplace();
    for (const auto &device : lanDevices.devices) {
        if (device.hosting)
            routeSurvey->candidates.append({device.address, {}});
    }
    for (const auto &device : tailnetDevices.devices) {
        if (!device.hosting)
            continue;
        if (!embeddedMode)
            routeSurvey->candidates.append({device.address, {}});
        else if (device.address.protocol() == QAbstractSocket::IPv4Protocol) {
            if (const auto *peer = findTailnetPeer(&currentNetwork, device.address))
                routeSurvey->targets.enqueue({peer->id, device.address});
        }
    }
    routeDeadline.start(4000);
    if (!routeSurvey->candidates.isEmpty()) {
        routeSurvey->batch = serverInfo.probeBatch(routeSurvey->candidates, routeSurvey->targets.isEmpty() ? 4000 : 1600);
        routeSurvey->candidates.clear();
    }
    continueRouteSurvey();
}

void NativeDevicePresence::continueRouteSurvey()
{
    if (!routeSurvey || routeSurvey->opening || routeSurvey->batch)
        return;
    if (!routeSurvey->candidates.isEmpty() && (routeSurvey->candidates.size() >= 8 || routeSurvey->targets.isEmpty())) {
        routeSurvey->batch = serverInfo.probeBatch(routeSurvey->candidates, qMax(1, routeDeadline.remainingTime()));
        routeSurvey->candidates.clear();
        if (!routeSurvey->batch)
            finishRouteSurvey(true);
        return;
    }
    if (!embeddedClient || routeSurvey->targets.isEmpty()) {
        finishRouteSurvey(true);
        return;
    }
    const auto next = routeSurvey->targets.first();
    const auto opening = embeddedClient->requestOpen(embeddedGeneration, next.first, next.second);
    if (opening) {
        routeSurvey->opening = opening;
        routeSurvey->openingAddress = next.second;
        routeSurvey->targets.dequeue();
    } else {
        QTimer::singleShot(40, this, &NativeDevicePresence::continueRouteSurvey);
    }
}

void NativeDevicePresence::receiveProbeLease(quint64 generation, quint64 request,
                                             const aurora::tailscale::OpenResult &result)
{
    if (!routeSurvey || routeSurvey->opening != request)
        return;
    routeSurvey->opening = 0;
    if (generation != embeddedGeneration) {
        finishRouteSurvey(false);
        return;
    }
    if (result.lease && result.lease->isValid()) {
        routeSurvey->leases.append(result.lease);
        routeSurvey->candidates.append({routeSurvey->openingAddress,
                                           Endpoint{result.lease->address(), result.lease->port()}});
    }
    continueRouteSurvey();
}

void NativeDevicePresence::receiveRouteBatch(quint64 request, const ServerInfo::BatchResult &result)
{
    if (!routeSurvey || routeSurvey->batch != request)
        return;
    routeSurvey->batch = 0;
    routeSurvey->leases.clear();
    if (result.status == ServerInfo::Status::AccessChanged) {
        finishRouteSurvey(false);
        return;
    }
    for (const auto &route : result.hosts) {
        const auto address = route.endpoint.address;
        const bool onLan = !route.viaEmbeddedTailnet &&
            std::any_of(lanDevices.devices.cbegin(), lanDevices.devices.cend(), [&address](const PresentDevice &item) {
                return item.address == address;
            });
        (onLan ? routeSurvey->lan : routeSurvey->tailnet).append(route);
    }
    continueRouteSurvey();
}

void NativeDevicePresence::finishRouteSurvey(bool keep)
{
    if (!routeSurvey)
        return;
    auto survey = std::exchange(routeSurvey, std::nullopt);
    routeDeadline.stop();
    if (survey->opening && embeddedClient)
        embeddedClient->cancelOpen(survey->opening);
    if (survey->batch)
        serverInfo.cancel(survey->batch);
    if (keep) {
        lanDevices.hosts = std::move(survey->lan);
        tailnetDevices.hosts = std::move(survey->tailnet);
        emit changed();
    }
}
