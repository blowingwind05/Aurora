// SPDX-License-Identifier: GPL-3.0-only
#include "Discovery.h"

#include <QCollator>
#include <QSet>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Aurora::Discovery {
namespace {

// Keep address families and IPv6 scopes distinct for catalog and presence keys.
QString addressKey(const QHostAddress &address)
{
    return address.toString();
}

QHostAddress normalizeMappedAddress(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const QHostAddress ipv4(address.toIPv4Address());
        if (address.isEqual(ipv4, QHostAddress::ConvertV4MappedToIPv4))
            return ipv4;
    }
    return address;
}

using StreamRoutes = QHash<QString, const StreamHost *>;

StreamRoutes streamRoutes(const QList<StreamHost> &hosts)
{
    StreamRoutes routes;
    for (const auto &host : hosts) {
        if (host.endpoint.port != BundledSunshinePort)
            continue;
        const auto key = addressKey(host.endpoint.address);
        if (key.isEmpty() || routes.contains(key))
            throw std::invalid_argument("Aurora stream routes require unique addresses on the bundled port");
        routes.insert(key, &host);
    }
    return routes;
}

} // namespace

bool Device::canConnect() const
{
    return lan.has_value() || tailnet.has_value();
}

bool Device::supportsPower() const
{
    return (lan && lan->supportsPower) || (tailnet && tailnet->supportsPower);
}

bool NetworkSnapshot::isAuthenticated() const
{
    return owner > 0 && !localId.trimmed().isEmpty() && !localAddress.isNull();
}

QList<QHostAddress> tailnetAddresses(const Device &peer)
{
    QList<QHostAddress> addresses;
    if (!peer.address.isNull())
        addresses.append(peer.address);
    if (!peer.alternateAddress.isNull() && addressKey(peer.alternateAddress) != addressKey(peer.address))
        addresses.append(peer.alternateAddress);
    return addresses;
}

const Device *findTailnetPeer(const NetworkSnapshot *network, const QHostAddress &source)
{
    if (!network || !network->isAuthenticated() || source.isNull())
        return nullptr;

    const auto normalized = normalizeMappedAddress(source);
    const auto key = addressKey(normalized);
    if (normalized.isLoopback() || normalized == QHostAddress::AnyIPv4 || normalized == QHostAddress::AnyIPv6 ||
        key == addressKey(normalizeMappedAddress(network->localAddress)))
        return nullptr;

    const Device *match = nullptr;
    for (const auto &peer : network->devices) {
        const auto addresses = tailnetAddresses(peer);
        const bool ownsAddress = std::any_of(addresses.cbegin(), addresses.cend(), [&key](const QHostAddress &candidate) {
            return addressKey(normalizeMappedAddress(candidate)) == key;
        });
        if (!ownsAddress)
            continue;
        if (match)
            return nullptr;
        match = &peer;
    }
    return match;
}

KnownIdentities resolveKnownIdentities(const QList<Device> &peers, const QList<PresentDevice> &present,
                                       const KnownIdentities &saved)
{
    QHash<QString, QUuid> live;
    for (const auto &device : present) {
        const auto key = addressKey(device.address);
        if (key.isEmpty() || live.contains(key))
            throw std::invalid_argument("Tailnet presence requires unique addresses");
        live.insert(key, device.id);
    }

    KnownIdentities known;
    for (const auto &peer : peers) {
        std::optional<QUuid> identity;
        bool conflicting = false;
        for (const auto &address : tailnetAddresses(peer)) {
            const auto candidate = live.constFind(addressKey(address));
            if (candidate == live.cend())
                continue;
            if (identity && *identity != candidate.value()) {
                conflicting = true;
                break;
            }
            identity = candidate.value();
        }
        if (conflicting)
            continue;

        if (identity)
            known.insert(peer.id, *identity);
        else if (const auto previous = saved.value(peer.id); !previous.isNull())
            known.insert(peer.id, previous);
    }
    return known;
}

QList<Device> merge(const QList<Device> &peers, const LanSnapshot *lan, const TailnetSnapshot *tailnet,
                    const QLocale &locale)
{
    const auto lanRoutes = lan ? streamRoutes(lan->hosts) : StreamRoutes{};
    const auto tailnetRoutes = tailnet ? streamRoutes(tailnet->hosts) : StreamRoutes{};
    QHash<QUuid, const PresentDevice *> local;
    QList<QUuid> localOrder;
    if (lan) {
        for (const auto &device : lan->devices) {
            auto entry = local.find(device.id);
            if (entry == local.end()) {
                local.insert(device.id, &device);
                localOrder.append(device.id);
            } else if (!lanRoutes.contains(addressKey(entry.value()->address)) &&
                       lanRoutes.contains(addressKey(device.address))) {
                entry.value() = &device;
            }
        }
    }

    QList<Device> devices;
    if (tailnet) {
        QSet<QString> liveTailnet;
        for (const auto &device : tailnet->devices)
            liveTailnet.insert(addressKey(device.address));

        for (const auto &peer : peers) {
            const auto known = tailnet->known.constFind(peer.id);
            if (known == tailnet->known.cend())
                continue;

            Device device;
            device.id = peer.id;
            device.name = peer.name;
            device.platform = peer.platform;
            device.address = peer.address;
            device.alternateAddress = peer.alternateAddress;
            device.tailnetVisible = true;
            for (const auto &address : tailnetAddresses(peer)) {
                const auto key = addressKey(address);
                if (!device.tailnet) {
                    if (const auto *route = tailnetRoutes.value(key))
                        device.tailnet = *route;
                }
                device.online = device.online || liveTailnet.contains(key);
            }

            const auto *nearby = local.value(known.value());
            if (nearby) {
                const auto *route = lanRoutes.value(addressKey(nearby->address));
                if (!device.tailnet || !route || sameHostIdentity(device.tailnet->id, route->id)) {
                    if (route)
                        device.lan = *route;
                    device.lanAddress = nearby->address;
                    local.remove(known.value());
                }
            }
            device.online = device.online || device.tailnet.has_value() || !device.lanAddress.isNull();
            devices.append(std::move(device));
        }
    }

    for (const auto &id : std::as_const(localOrder)) {
        const auto *peer = local.value(id);
        if (!peer)
            continue;
        Device device;
        device.id = QStringLiteral("lan:") + id.toString(QUuid::Id128);
        device.name = peer->name;
        device.platform = peer->platform;
        device.address = peer->address;
        device.lanAddress = peer->address;
        device.online = true;
        if (const auto *route = lanRoutes.value(addressKey(peer->address)))
            device.lan = *route;
        devices.append(std::move(device));
    }

    QCollator collator(locale);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(devices.begin(), devices.end(), [&collator](const Device &left, const Device &right) {
        if (left.canConnect() != right.canConnect())
            return left.canConnect();
        if (left.online != right.online)
            return left.online;
        return collator.compare(left.name, right.name) < 0;
    });
    return devices;
}

QList<Device> visibleDevices(const NetworkSnapshot *network, const LanSnapshot *lan,
                             const TailnetSnapshot *tailnet, const QLocale &locale)
{
    if (!network || !network->isAuthenticated())
        return {};

    auto devices = merge(network->devices, lan, tailnet, locale);
    devices.erase(std::remove_if(devices.begin(), devices.end(), [](const Device &device) {
        return !device.tailnetVisible;
    }), devices.end());
    return devices;
}

QJsonObject qtDeviceRow(const Device &device, const QString &wallpaperUrl)
{
    auto lanAddress = device.lanAddress;
    if (lanAddress.isNull() && device.lan)
        lanAddress = device.lan->endpoint.address;
    const auto tailnetAddress = device.tailnet ? device.tailnet->endpoint.address
        : device.tailnetVisible ? device.address : QHostAddress{};

    return {
        {QStringLiteral("deviceId"), device.id},
        {QStringLiteral("displayName"), device.name},
        {QStringLiteral("online"), device.online},
        {QStringLiteral("canConnect"), device.canConnect()},
        {QStringLiteral("supportsPower"), device.supportsPower()},
        {QStringLiteral("lanAddress"), lanAddress.toString()},
        {QStringLiteral("tailscaleAddress"), tailnetAddress.toString()},
        {QStringLiteral("wallpaperUrl"), wallpaperUrl}
    };
}

bool sameHostIdentity(const QString &first, const QString &second)
{
    if (first.size() != second.size() || first.isNull() != second.isNull())
        return false;

    // OrdinalIgnoreCase uses simple uppercase mapping without folding ASCII into non-ASCII.
    for (decltype(first.size()) i = 0; i < first.size(); ++i) {
        uint left = first.at(i).unicode();
        uint right = second.at(i).unicode();
        if ((left < 0x80) != (right < 0x80))
            return false;
        if (QChar::isHighSurrogate(left) && QChar::isHighSurrogate(right) && i + 1 < first.size()) {
            const uint leftTrail = first.at(i + 1).unicode();
            const uint rightTrail = second.at(i + 1).unicode();
            if (QChar::isLowSurrogate(leftTrail) && QChar::isLowSurrogate(rightTrail)) {
                left = QChar::surrogateToUcs4(left, leftTrail);
                right = QChar::surrogateToUcs4(right, rightTrail);
                ++i;
            }
        }
        if (QChar::toUpper(left) != QChar::toUpper(right))
            return false;
    }
    return true;
}

std::optional<RouteSelection> chooseRoute(const Device &device, const RouteProbe &probe,
                                          RouteRequirement requirement)
{
    for (const auto *route : {&device.lan, &device.tailnet}) {
        if (!*route)
            continue;
        auto current = probe(**route);
        if (current && sameHostIdentity(current->id, (*route)->id) &&
            (requirement != RouteRequirement::Power || current->supportsPower))
            return RouteSelection{**route, std::move(*current)};
    }
    return std::nullopt;
}

} // namespace Aurora::Discovery
