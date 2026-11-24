// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDevicePresence.h"
#include "discovery/KnownDevices.h"

#include <QHostInfo>
#include <QNetworkInterface>

#include <algorithm>

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
    connect(&embeddedDeadline, &QTimer::timeout, this, &NativeDevicePresence::clearEmbeddedScan);
}

void NativeDevicePresence::update(const aurora::tailscale::Status &status, bool embedded, bool scan)
{
    currentNetwork = networkFromStatus(status);
    if (!(account == status.account)) {
        clearEmbeddedScan();
        account = status.account;
        ++accountGeneration;
        lanRequest = peersRequest = 0;
        lanDevices = {};
        tailnetDevices = {};
    }
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
    if (!scan)
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
    if (!status.account || localIdentity.isNull())
        return;
    if (embeddedClient != &client) {
        disconnect(embeddedReplies);
        embeddedClient = &client;
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
    if (result.status != ScanStatus::Complete || !currentNetwork.isAuthenticated())
        return;

    auto &devices = lan ? lanDevices.devices : tailnetDevices.devices;
    devices.clear();
    for (const auto &reply : result.replies) {
        const auto &device = reply.announcement;
        devices.append({device.id, reply.address, device.name, platformName(device.platform), device.hosting});
    }
    if (!lan)
        tailnetDevices.known = rememberKnownDevices(directory, currentNetwork, tailnetDevices.devices);
    emit changed();
}
