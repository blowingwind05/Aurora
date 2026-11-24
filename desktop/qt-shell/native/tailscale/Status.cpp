// SPDX-License-Identifier: GPL-3.0-only
#include "Status.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace aurora::tailscale {
namespace {

bool digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

// Qt stores decimal int64 tokens exactly, but it can also round a fractional
// or exponential token to an integer. Discard those tokens before reading IDs.
// The original document is parsed first so this cannot repair malformed JSON.
QJsonObject exactIntegerObject(const QByteArray &line)
{
    const auto original = QJsonDocument::fromJson(line);
    if (!original.isObject())
        return {};

    QByteArray normalized;
    const auto size = line.size();
    decltype(line.size()) copied = 0;
    for (decltype(line.size()) pos = 0; pos < size;) {
        const char ch = line.at(pos++);
        if (ch == '"') {
            while (pos < size) {
                const char next = line.at(pos++);
                if (next == '"')
                    break;
                if (next == '\\' && pos < size)
                    ++pos;
            }
        } else if (digit(ch) || ch == '-') {
            const auto start = pos - 1;
            while (pos < size && digit(line.at(pos)))
                ++pos;
            if (pos == size || (line.at(pos) != '.' && line.at(pos) != 'e' && line.at(pos) != 'E'))
                continue;
            while (pos < size && (digit(line.at(pos)) || QByteArrayLiteral(".eE+-").contains(line.at(pos))))
                ++pos;
            normalized.append(line.constData() + copied, start - copied);
            normalized.append("null");
            copied = pos;
        }
    }
    if (!copied)
        return original.object();
    normalized.append(line.constData() + copied, size - copied);
    return QJsonDocument::fromJson(normalized).object();
}

std::optional<qint64> integer(const QJsonValue &value)
{
    if (!value.isDouble())
        return std::nullopt;
    const auto variant = value.toVariant();
    if (variant.userType() != QMetaType::LongLong)
        return std::nullopt;
    return variant.toLongLong();
}

QString label(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

QString stableId(const QJsonObject &object, const char *key)
{
    const auto value = object.value(QLatin1String(key)).toString();
    if (value.isEmpty() || value != value.trimmed())
        return {};
    for (const auto ch : value) {
        if (ch.isSpace() || !ch.isPrint())
            return {};
    }
    return value;
}

bool isPersonalNode(const QJsonObject &node)
{
    const auto tags = node.value(QLatin1String("Tags"));
    const auto shared = node.value(QLatin1String("ShareeNode"));
    const auto sharer = node.value(QLatin1String("AltSharerUserID"));
    return (tags.isUndefined() || tags.isNull() || (tags.isArray() && tags.toArray().isEmpty())) &&
        (shared.isUndefined() || (shared.isBool() && !shared.toBool())) &&
        (sharer.isUndefined() || integer(sharer) == 0);
}

BackendState backendState(const QString &state)
{
    if (state == QLatin1String("Running")) return BackendState::Connected;
    if (state == QLatin1String("NeedsLogin")) return BackendState::NeedsLogin;
    if (state == QLatin1String("NeedsMachineAuth")) return BackendState::NeedsApproval;
    if (state == QLatin1String("Stopped")) return BackendState::Stopped;
    if (state == QLatin1String("Starting") || state == QLatin1String("NoState")) return BackendState::Starting;
    return BackendState::Unknown;
}

QUrl officialLoginUrl(const QString &raw)
{
    const QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host() != QLatin1String("login.tailscale.com") ||
            url.port(443) != 443 || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) ||
            !url.path(QUrl::FullyEncoded).startsWith(QLatin1String("/a/"))) {
        return {};
    }
    return url;
}

bool isNodeAddress(const QString &literal, const QHostAddress &address)
{
    if (literal.contains(QLatin1Char('%')))
        return false;
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        static const QHostAddress tailnet(QStringLiteral("100.64.0.0"));
        if (literal != address.toString() || !address.isInSubnet(tailnet, 10))
            return false;
        const auto ip = address.toIPv4Address();
        // Preserve the legacy discovery exclusions for Tailscale service ranges.
        const auto second = (ip >> 16) & 0xff;
        const auto third = (ip >> 8) & 0xff;
        return !(second == 100 && (third == 0 || third == 100)) &&
            !(second == 115 && (third == 92 || third == 93)) &&
            literal != QLatin1String("100.101.102.103");
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    static const QHostAddress tailnet(QStringLiteral("fd7a:115c:a1e0::"));
    static const QHostAddress service(QStringLiteral("fd7a:115c:a1e0:b1a::"));
    static const QHostAddress dns(QStringLiteral("fd7a:115c:a1e0::53"));
    return address.isInSubnet(tailnet, 48) && !address.isInSubnet(service, 64) && address != dns;
}

struct Addresses
{
    QHostAddress ipv4;
    QHostAddress ipv6;

    QHostAddress preferred() const { return ipv4.isNull() ? ipv6 : ipv4; }
};

Addresses nodeAddresses(const QJsonValue &list, const Addresses &local = {})
{
    Addresses result;
    for (const auto &value : list.toArray()) {
        const auto literal = value.toString();
        const QHostAddress address(literal);
        if (!isNodeAddress(literal, address) || address == local.ipv4 || address == local.ipv6)
            continue;
        auto &first = address.protocol() == QAbstractSocket::IPv4Protocol ? result.ipv4 : result.ipv6;
        if (first.isNull())
            first = address;
    }
    return result;
}

QVector<Peer> readPeers(const QJsonObject &peers, const AccountIdentity &account, const Addresses &local)
{
    QHash<QString, int> appearances;
    QVector<Peer> devices;
    for (auto entry = peers.constBegin(); entry != peers.constEnd(); ++entry) {
        const auto peer = entry.value().toObject();
        const auto id = stableId(peer, "ID");
        if (id.isEmpty() || id == account.nodeId || integer(peer.value(QLatin1String("UserID"))) != account.ownerId ||
                !isPersonalNode(peer))
            continue;
        ++appearances[id];
        const auto addresses = nodeAddresses(peer.value(QLatin1String("TailscaleIPs")), local);
        if (addresses.preferred().isNull())
            continue;

        auto name = label(peer, "HostName");
        if (name.isEmpty()) {
            name = label(peer, "DNSName");
            while (name.endsWith(QLatin1Char('.')))
                name.chop(1);
        }
        devices.append(Peer{id, name.isEmpty() ? id : name, label(peer, "OS"), addresses.preferred(),
            addresses.ipv4.isNull() ? QHostAddress() : addresses.ipv6, peer.value(QLatin1String("Online")).toBool()});
    }
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&appearances](const Peer &peer) {
        return appearances.value(peer.nodeId) != 1;
    }), devices.end());
    std::sort(devices.begin(), devices.end(), [](const Peer &a, const Peer &b) {
        if (a.online != b.online)
            return a.online;
        const int order = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return order ? order < 0 : a.nodeId < b.nodeId;
    });
    return devices;
}

Status readStatus(const QJsonObject &data)
{
    Status status;
    status.state = backendState(label(data, "BackendState"));
    if (status.state == BackendState::NeedsLogin)
        status.loginUrl = officialLoginUrl(label(data, "AuthURL"));
    if (status.state != BackendState::Connected)
        return status;

    const auto self = data.value(QLatin1String("Self")).toObject();
    const auto tailnet = data.value(QLatin1String("CurrentTailnet")).toObject();
    const auto owner = integer(self.value(QLatin1String("UserID")));
    const auto localId = stableId(self, "ID");
    const auto tailnetId = stableId(tailnet, "StableID");
    if (!owner || *owner <= 0 || localId.isEmpty() || tailnetId.isEmpty() || !isPersonalNode(self))
        return status;

    status.account = AccountIdentity{*owner, localId, tailnetId};
    status.tailnetName = label(tailnet, "Name");
    const auto user = data.value(QLatin1String("User")).toObject().value(QString::number(*owner)).toObject();
    const auto userId = user.value(QLatin1String("ID"));
    if (userId.isUndefined() || integer(userId) == *owner) {
        status.displayName = label(user, "DisplayName");
        status.loginName = label(user, "LoginName");
    }

    auto local = nodeAddresses(data.value(QLatin1String("TailscaleIPs")));
    const auto selfAddresses = nodeAddresses(self.value(QLatin1String("TailscaleIPs")));
    status.localAddress = local.preferred().isNull() ? selfAddresses.preferred() : local.preferred();
    if (local.ipv4.isNull())
        local.ipv4 = selfAddresses.ipv4;
    if (local.ipv6.isNull())
        local.ipv6 = selfAddresses.ipv6;
    if (!status.localAddress.isNull())
        status.peers = readPeers(data.value(QLatin1String("Peer")).toObject(), *status.account, local);
    return status;
}

} // namespace

StatusResult decodeStatusReply(const QByteArray &line, const QString &requestId)
{
    if (line.size() > maxStatusReplyBytes)
        return {StatusError::ResponseTooLarge, std::nullopt};

    const auto reply = exactIntegerObject(line);
    const auto id = reply.value(QLatin1String("id"));
    const auto ok = reply.value(QLatin1String("ok"));
    if (requestId.isEmpty() || !id.isString() || id.toString() != requestId || !ok.isBool())
        return {StatusError::InvalidResponse, std::nullopt};

    const auto result = reply.value(QLatin1String("result"));
    const auto error = reply.value(QLatin1String("error"));
    if (!ok.toBool()) {
        if (result.isNull() && error.isString())
            return {StatusError::HelperRejected, std::nullopt};
        return {StatusError::InvalidResponse, std::nullopt};
    }
    const auto data = result.toObject();
    if (!error.isNull() || label(data, "BackendState").isEmpty())
        return {StatusError::InvalidResponse, std::nullopt};
    return {StatusError::None, readStatus(data)};
}

} // namespace aurora::tailscale
