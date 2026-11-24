// SPDX-License-Identifier: GPL-3.0-only
#include "PresenceProbe.h"

#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <utility>

namespace Aurora::Presence {
namespace {

using aurora::tailscale::BackendState;
using aurora::tailscale::Status;

QHostAddress normalizeMappedAddress(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const QHostAddress ipv4(address.toIPv4Address());
        if (address.isEqual(ipv4, QHostAddress::ConvertV4MappedToIPv4))
            return ipv4;
    }
    return address;
}

bool isEndpointAddress(const QHostAddress &address)
{
    return !address.isNull() && address != QHostAddress::Any &&
        address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6;
}

bool isAuthenticated(const Status &status)
{
    return status.state == BackendState::Connected && status.account && status.account->ownerId > 0 &&
        !status.account->nodeId.trimmed().isEmpty() && !status.account->tailnetId.trimmed().isEmpty() &&
        isEndpointAddress(status.localAddress);
}

QString findPeerId(const Status &status, const QHostAddress &target)
{
    if (!isEndpointAddress(target) || target.isLoopback() ||
        target.toString() == normalizeMappedAddress(status.localAddress).toString())
        return {};

    QString found;
    const auto key = target.toString();
    for (const auto &peer : status.peers) {
        if (normalizeMappedAddress(peer.address).toString() != key &&
            normalizeMappedAddress(peer.alternateAddress).toString() != key)
            continue;
        if (!found.isEmpty() || peer.nodeId.trimmed().isEmpty() || peer.nodeId == status.account->nodeId)
            return {};
        found = peer.nodeId;
    }
    return found;
}

QDeadlineTimer probeDeadline(int timeoutMs)
{
    return QDeadlineTimer(std::clamp(timeoutMs, 0, ProbeTimeoutMs), Qt::PreciseTimer);
}

} // namespace

QByteArray answerDatagram(const QByteArray &request, const Announcement &local,
                          const QHostAddress &sender, const SourceAllowed &allowSource)
{
    const auto source = normalizeMappedAddress(sender);
    if (!allowSource || !isEndpointAddress(source) || !allowSource(source))
        return {};
    return encodeReply(request, local);
}

DatagramProbe::DatagramProbe(const QUuid &localId, qsizetype limit, SourceAllowed allowSource, int timeoutMs)
    : localId_(localId), limit_(limit), allowSource_(std::move(allowSource)),
      deadline_(probeDeadline(timeoutMs)), request_(createRequest())
{
}

DatagramProbe DatagramProbe::unicast(const QUuid &localId, const QList<QHostAddress> &targets, int timeoutMs)
{
    QSet<QString> addresses;
    for (const auto &target : targets) {
        const auto address = normalizeMappedAddress(target);
        if (isEndpointAddress(address))
            addresses.insert(address.toString());
    }
    const auto limit = addresses.size();
    return DatagramProbe(localId, limit, [addresses = std::move(addresses)](const QHostAddress &sender) {
        return addresses.contains(sender.toString());
    }, timeoutMs);
}

DatagramProbe DatagramProbe::lan(const QUuid &localId, SourceAllowed allowSource, int timeoutMs)
{
    return DatagramProbe(localId, LanReplyLimit, std::move(allowSource), timeoutMs);
}

const QByteArray &DatagramProbe::request() const
{
    return request_;
}

int DatagramProbe::remainingMilliseconds() const
{
    if (localId_.isNull() || !allowSource_ || replies_.size() >= limit_)
        return 0;
    return static_cast<int>(deadline_.remainingTime());
}

bool DatagramProbe::addReply(const QByteArray &packet, const QHostAddress &sender, quint16 senderPort)
{
    if (!remainingMilliseconds() || senderPort != Port)
        return false;
    const auto source = normalizeMappedAddress(sender);
    if (!isEndpointAddress(source) || !allowSource_(source) || deadline_.hasExpired())
        return false;
    auto response = decodeReply(packet, request_, source, localId_);
    if (!response)
        return false;

    const auto key = source.toString();
    const auto index = replyIndexes_.constFind(key);
    if (index == replyIndexes_.cend()) {
        replyIndexes_.insert(key, replies_.size());
        replies_.append(std::move(*response));
    } else {
        replies_[index.value()] = std::move(*response);
    }
    return true;
}

const QList<Response> &DatagramProbe::replies() const
{
    return replies_;
}

void DatagramProbe::cancel()
{
    deadline_.setRemainingTime(0, Qt::PreciseTimer);
}

EmbeddedProbe::EmbeddedProbe(const aurora::tailscale::AccountIdentity &account, quint64 generation,
                             const QHostAddress &localAddress, const QHostAddress &target, const QString &peerId,
                             const QUuid &localId, int timeoutMs)
    : account_(account), generation_(generation), localAddress_(localAddress), target_(target),
      peerId_(peerId), localId_(localId), deadline_(probeDeadline(timeoutMs)), request_(createRequest())
{
}

std::optional<EmbeddedProbe> EmbeddedProbe::start(const Status &status, quint64 generation,
                                                  const QHostAddress &target, const QUuid &localId, int timeoutMs)
{
    if (!isAuthenticated(status) || localId.isNull())
        return std::nullopt;
    const auto address = normalizeMappedAddress(target);
    const auto peerId = findPeerId(status, address);
    if (peerId.isEmpty())
        return std::nullopt;
    return EmbeddedProbe(*status.account, generation, status.localAddress, address, peerId, localId, timeoutMs);
}

int EmbeddedProbe::remainingMilliseconds() const
{
    return static_cast<int>(deadline_.remainingTime());
}

QJsonObject EmbeddedProbe::requestParameters() const
{
    if (deadline_.hasExpired())
        return {};
    return {
        {QStringLiteral("peerId"), peerId_},
        {QStringLiteral("address"), target_.toString()},
        {QStringLiteral("packet"), QString::fromLatin1(request_.toBase64())}
    };
}

std::optional<Response> EmbeddedProbe::decodeResult(const QJsonValue &result, const Status &current,
                                                      quint64 generation) const
{
    if (deadline_.hasExpired() || generation != generation_ || !isAuthenticated(current) ||
        !(*current.account == account_) || current.localAddress.toString() != localAddress_.toString() ||
        findPeerId(current, target_) != peerId_ || !result.isString())
        return std::nullopt;
    const auto encoded = result.toString();
    constexpr int EncodedSize = ((PacketSize + 2) / 3) * 4;
    if (encoded.size() != EncodedSize)
        return std::nullopt;
    const auto packet = QByteArray::fromBase64(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    return decodeReply(packet, request_, target_, localId_);
}

std::optional<QByteArray> embeddedPublishParameters(const Status &status, const Announcement &local)
{
    if (!isAuthenticated(status))
        return std::nullopt;
    const auto packet = announcementTemplate(local);
    if (packet.isEmpty())
        return std::nullopt;
    const QJsonObject parameters{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("nodeId"), status.account->nodeId},
        {QStringLiteral("packet"), QString::fromLatin1(packet.toBase64())}
    };
    auto json = QJsonDocument(parameters).toJson(QJsonDocument::Compact);
    // Qt 5 JSON serialization can round qint64 values beyond the exact double range.
    json.chop(1);
    json += ",\"owner\":";
    json += QByteArray::number(status.account->ownerId);
    json += '}';
    return json;
}

} // namespace Aurora::Presence
