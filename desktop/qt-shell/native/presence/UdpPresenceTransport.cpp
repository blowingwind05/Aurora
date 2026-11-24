// SPDX-License-Identifier: GPL-3.0-only
#include "UdpPresenceTransport.h"
#include "PresenceAddress.h"

#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <utility>

namespace Aurora::Presence {
namespace {

using Detail::isEndpointAddress;
using Detail::normalizeMappedAddress;
using aurora::tailscale::BackendState;

constexpr int ReceiveBatchSize = 64;

bool isLanAddress(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
        return false;
    const auto ip = address.toIPv4Address();
    const auto first = ip >> 24;
    return first > 0 && first < 224 && (ip & 0xffc00000u) != 0x64400000u;
}

bool canBind(const QHostAddress &binding, QAbstractSocket::NetworkLayerProtocol family)
{
    return binding.protocol() == family && !binding.isNull();
}

bool listen(QUdpSocket &socket, const QHostAddress &binding, QAbstractSocket::NetworkLayerProtocol family)
{
    return socket.state() == QAbstractSocket::BoundState ||
        (canBind(binding, family) && socket.bind(binding, Port, QUdpSocket::DontShareAddress));
}

} // namespace

bool LanSubnet::isUsable() const
{
    return isLanAddress(address) && mask.protocol() == QAbstractSocket::IPv4Protocol && mask.toIPv4Address() != 0;
}

QHostAddress LanSubnet::broadcastAddress() const
{
    return isUsable() ? QHostAddress(address.toIPv4Address() | ~mask.toIPv4Address()) : QHostAddress{};
}

bool LanSubnet::contains(const QHostAddress &remote) const
{
    const auto candidate = normalizeMappedAddress(remote);
    if (!isUsable() || !isLanAddress(candidate) || candidate == address)
        return false;

    const auto netmask = mask.toIPv4Address();
    const auto ip = candidate.toIPv4Address();
    const auto network = address.toIPv4Address() & netmask;
    const auto host = ip & ~netmask;
    return (ip & netmask) == network && host != 0 && host != ~netmask;
}

bool LanSubnet::operator==(const LanSubnet &other) const
{
    return address == other.address && mask == other.mask;
}

struct UdpPresenceTransport::Plan
{
    QHostAddress source;
    QList<QHostAddress> destinations;
    std::optional<LanSubnet> subnet;
};

struct UdpPresenceTransport::ProbeSocket : QObject
{
    ProbeSocket(DatagramProbe probe, QObject *parent)
        : QObject(parent), socket(this), probe(std::move(probe))
    {
        socket.setProxy(QNetworkProxy::NoProxy);
    }

    QUdpSocket socket;
    DatagramProbe probe;
    bool done = false;
};

struct UdpPresenceTransport::Scan : QObject
{

    Scan(quint64 requestId, quint64 policyRevision, QObject *parent)
        : QObject(parent), id(requestId), revision(policyRevision), deadline(this)
    {
        deadline.setSingleShot(true);
        deadline.setTimerType(Qt::PreciseTimer);
    }

    const quint64 id;
    const quint64 revision;
    QTimer deadline;
    QList<ProbeSocket *> groups;
    bool socketError = false;
};

UdpPresenceTransport::UdpPresenceTransport(const Announcement &local, const UdpBindings &bindings, QObject *parent)
    : QObject(parent), local_(local), bindings_(bindings), ipv4_(new QUdpSocket(this)), ipv6_(new QUdpSocket(this))
{
    qRegisterMetaType<ScanResult>();
    for (auto *socket : {ipv4_, ipv6_}) {
        socket->setProxy(QNetworkProxy::NoProxy);
        connect(socket, &QUdpSocket::readyRead, this, [this, socket] { drainRequests(socket); });
    }
}

ListenerState UdpPresenceTransport::startResponding()
{
    if (local_.id.isNull())
        return {};
    return {listen(*ipv4_, bindings_.ipv4, QAbstractSocket::IPv4Protocol),
            listen(*ipv6_, bindings_.ipv6, QAbstractSocket::IPv6Protocol)};
}

void UdpPresenceTransport::stopResponding()
{
    ipv4_->close();
    ipv6_->close();
}

void UdpPresenceTransport::updateAnnouncement(const QString &name, Platform platform, bool hosting)
{
    local_.name = name;
    local_.platform = platform;
    local_.hosting = hosting;
}

void UdpPresenceTransport::updateAccess(const QList<LanSubnet> &lan, const aurora::tailscale::Status *systemTailnet,
                                        quint64 accountGeneration)
{
    if (accountGeneration < generation_)
        return;

    QList<LanSubnet> links;
    for (const auto &link : lan) {
        if (link.isUsable() && !links.contains(link))
            links.append(link);
    }

    std::optional<aurora::tailscale::AccountIdentity> account;
    QHostAddress accountAddress;
    QHash<QString, QString> peers;
    if (systemTailnet && systemTailnet->state == BackendState::Connected && systemTailnet->account &&
        systemTailnet->account->ownerId > 0 && !systemTailnet->account->nodeId.trimmed().isEmpty() &&
        !systemTailnet->account->tailnetId.trimmed().isEmpty() && isEndpointAddress(systemTailnet->localAddress)) {
        account = systemTailnet->account;
        accountAddress = normalizeMappedAddress(systemTailnet->localAddress);
        const auto localKey = accountAddress.toString();
        QSet<QString> ambiguous;
        for (const auto &peer : systemTailnet->peers) {
            if (peer.nodeId.trimmed().isEmpty() || peer.nodeId == account->nodeId)
                continue;
            for (const auto &candidate : {peer.address, peer.alternateAddress}) {
                const auto address = normalizeMappedAddress(candidate);
                const auto key = address.toString();
                if (!isEndpointAddress(address) || key == localKey || ambiguous.contains(key))
                    continue;
                const auto existing = peers.constFind(key);
                if (existing != peers.cend() && existing.value() != peer.nodeId) {
                    peers.remove(key);
                    ambiguous.insert(key);
                } else {
                    peers.insert(key, peer.nodeId);
                }
            }
        }
    }

    if (links == lan_ && account == account_ && accountAddress.toString() == accountAddress_.toString() &&
        peers == tailnetPeers_ && accountGeneration == generation_)
        return;

    lan_ = std::move(links);
    account_ = std::move(account);
    accountAddress_ = accountAddress;
    tailnetPeers_ = std::move(peers);
    generation_ = accountGeneration;
    ++revision_;
    const auto active = scans_.values();
    for (auto *scan : active)
        finish(scan, ScanStatus::AccessChanged);
}

bool UdpPresenceTransport::allows(const QHostAddress &source) const
{
    return tailnetPeers_.contains(source.toString()) ||
        std::any_of(lan_.cbegin(), lan_.cend(), [&source](const LanSubnet &link) { return link.contains(source); });
}

void UdpPresenceTransport::drainRequests(QUdpSocket *socket)
{
    const SourceAllowed allowed = [this](const QHostAddress &source) { return allows(source); };
    for (int i = 0; i < ReceiveBatchSize && socket->hasPendingDatagrams(); ++i) {
        const auto datagram = socket->receiveDatagram(PacketSize + 1);
        if (!datagram.isValid() || datagram.senderPort() <= 0)
            continue;

        const auto reply = answerDatagram(datagram.data(), local_, datagram.senderAddress(), allowed);
        if (!reply.isEmpty())
            socket->writeDatagram(reply, datagram.senderAddress(), static_cast<quint16>(datagram.senderPort()));
    }

    if (socket->hasPendingDatagrams())
        QTimer::singleShot(0, socket, [this, socket] { drainRequests(socket); });
}

quint64 UdpPresenceTransport::probeLan()
{
    QList<Plan> plans;
    if (canBind(bindings_.ipv4, QAbstractSocket::IPv4Protocol)) {
        for (const auto &link : std::as_const(lan_)) {
            if (bindings_.ipv4 == QHostAddress::AnyIPv4 || bindings_.ipv4 == link.address)
                plans.append(Plan{link.address, {link.broadcastAddress()}, link});
        }
    }
    return queueScan(std::move(plans));
}

quint64 UdpPresenceTransport::probePeers(const QList<QHostAddress> &targets)
{
    Plan ipv4{bindings_.ipv4, {}, std::nullopt};
    Plan ipv6{bindings_.ipv6, {}, std::nullopt};
    QSet<QString> seen;
    for (const auto &target : targets) {
        const auto address = normalizeMappedAddress(target);
        const auto key = address.toString();
        if (!isEndpointAddress(address) || !allows(address) || seen.contains(key))
            continue;
        auto &plan = address.protocol() == QAbstractSocket::IPv4Protocol ? ipv4 : ipv6;
        if (!canBind(plan.source, address.protocol()))
            continue;
        seen.insert(key);
        plan.destinations.append(address);
    }

    QList<Plan> plans;
    if (!ipv4.destinations.isEmpty())
        plans.append(std::move(ipv4));
    if (!ipv6.destinations.isEmpty())
        plans.append(std::move(ipv6));
    return queueScan(std::move(plans));
}

quint64 UdpPresenceTransport::queueScan(QList<Plan> plans)
{
    auto *scan = new Scan(++nextRequest_, revision_, this);
    scans_.insert(scan->id, scan);
    QTimer::singleShot(0, scan, [this, scan, plans = std::move(plans)] {
        if (scans_.contains(scan->id))
            beginScan(scan, plans);
    });
    return scan->id;
}

void UdpPresenceTransport::beginScan(Scan *scan, const QList<Plan> &plans)
{
    QDeadlineTimer launchWindow(ProbeTimeoutMs, Qt::PreciseTimer);
    connect(&scan->deadline, &QTimer::timeout, scan, [this, scan] {
        finish(scan, scan->socketError ? ScanStatus::SocketError : ScanStatus::Complete);
    });
    scan->deadline.start(ProbeTimeoutMs);

    for (const auto &plan : plans) {
        const auto remaining = static_cast<int>(launchWindow.remainingTime());
        if (!remaining)
            break;

        auto probe = plan.subnet
            ? DatagramProbe::lan(local_.id, [subnet = *plan.subnet](const QHostAddress &source) {
                return subnet.contains(source);
            }, remaining)
            : DatagramProbe::unicast(local_.id, plan.destinations, remaining);
        if (!probe.remainingMilliseconds())
            continue;

        auto *group = new ProbeSocket(std::move(probe), scan);
        if (!group->socket.bind(plan.source, 0, QUdpSocket::DontShareAddress)) {
            scan->socketError = true;
            delete group;
            continue;
        }

        scan->groups.append(group);
        connect(&group->socket, &QUdpSocket::readyRead, scan,
                [this, scan, group] { drainReplies(scan, group); });
        bool sent = false;
        for (const auto &destination : plan.destinations) {
            if (!group->probe.remainingMilliseconds())
                break;
            const auto bytes = group->socket.writeDatagram(group->probe.request(), destination, Port);
            if (bytes == PacketSize)
                sent = true;
            else
                scan->socketError = true;
        }
        if (!sent) {
            group->done = true;
            group->socket.close();
        }
    }
    finishIfComplete(scan);
}

void UdpPresenceTransport::drainReplies(Scan *scan, ProbeSocket *group)
{
    if (!scans_.contains(scan->id) || group->done)
        return;

    auto &socket = group->socket;
    auto &probe = group->probe;
    for (int i = 0; i < ReceiveBatchSize && probe.remainingMilliseconds() && socket.hasPendingDatagrams(); ++i) {
        const auto datagram = socket.receiveDatagram(PacketSize + 1);
        if (datagram.isValid() && datagram.senderPort() > 0)
            probe.addReply(datagram.data(), datagram.senderAddress(), static_cast<quint16>(datagram.senderPort()));
    }
    if (!probe.remainingMilliseconds()) {
        group->done = true;
        socket.close();
        finishIfComplete(scan);
    } else if (socket.hasPendingDatagrams()) {
        QTimer::singleShot(0, scan, [this, scan, group] { drainReplies(scan, group); });
    }
}

void UdpPresenceTransport::finishIfComplete(Scan *scan)
{
    if (std::all_of(scan->groups.cbegin(), scan->groups.cend(), [](const ProbeSocket *group) { return group->done; }))
        finish(scan, scan->socketError ? ScanStatus::SocketError : ScanStatus::Complete);
}

void UdpPresenceTransport::cancelScan(quint64 requestId)
{
    if (auto *scan = scans_.value(requestId))
        finish(scan, ScanStatus::Cancelled);
}

void UdpPresenceTransport::finish(Scan *scan, ScanStatus status)
{
    if (!scans_.remove(scan->id))
        return;
    scan->deadline.stop();

    ScanResult result{status, {}};
    QSet<QString> seen;
    for (auto *group : std::as_const(scan->groups)) {
        group->socket.close();
        if (status == ScanStatus::AccessChanged)
            continue;
        for (const auto &reply : group->probe.replies()) {
            const auto key = reply.address.toString();
            if (!seen.contains(key)) {
                seen.insert(key);
                result.replies.append(reply);
            }
        }
    }

    QTimer::singleShot(0, this, [this, id = scan->id, revision = scan->revision, result = std::move(result)] {
        if (revision != revision_)
            emit scanFinished(id, {ScanStatus::AccessChanged, {}});
        else
            emit scanFinished(id, result);
    });
    scan->deleteLater();
}

} // namespace Aurora::Presence
