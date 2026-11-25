// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "PresencePacket.h"
#include "../adaptors/tailscale/Status.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>

#include <functional>

namespace Aurora::Presence {

inline constexpr int ProbeTimeoutMs = 900;
inline constexpr int LanReplyLimit = 512;

using SourceAllowed = std::function<bool(const QHostAddress &)>;

// The callback receives the normalized sender and must implement the transport's trusted
// peer or same-subnet policy. Send any reply to the original transport endpoint.
QByteArray answerDatagram(const QByteArray &request, const Announcement &local,
                          const QHostAddress &sender, const SourceAllowed &allowSource);

class DatagramProbe
{
public:
    static DatagramProbe unicast(const QUuid &localId, const QList<QHostAddress> &targets,
                                 int timeoutMs = ProbeTimeoutMs);
    static DatagramProbe lan(const QUuid &localId, SourceAllowed allowSource,
                             int timeoutMs = ProbeTimeoutMs);

    const QByteArray &request() const;
    int remainingMilliseconds() const;
    bool addReply(const QByteArray &packet, const QHostAddress &sender, quint16 senderPort);
    const QList<Response> &replies() const;
    void cancel();

private:
    using ReplyIndex = decltype(QList<Response>().size());

    DatagramProbe(const QUuid &localId, qsizetype limit, SourceAllowed allowSource, int timeoutMs);

    QUuid localId_;
    qsizetype limit_;
    SourceAllowed allowSource_;
    QDeadlineTimer deadline_;
    QByteArray request_;
    QList<Response> replies_;
    QHash<QString, ReplyIndex> replyIndexes_;
};

class EmbeddedProbe
{
public:
    // Use the active embedded status and generation, and discard work during mode transitions.
    static std::optional<EmbeddedProbe> start(const aurora::tailscale::Status &status, quint64 generation,
                                              const QHostAddress &target, const QUuid &localId,
                                              int timeoutMs = ProbeTimeoutMs);

    int remainingMilliseconds() const;
    QJsonObject requestParameters() const;
    const QByteArray &packet() const { return request_; }
    // The transport must correlate this result to its own existing helper RPC request.
    std::optional<Response> decodeResult(const QJsonValue &result, const aurora::tailscale::Status &current,
                                         quint64 generation) const;

private:
    EmbeddedProbe(const aurora::tailscale::AccountIdentity &account, quint64 generation,
                  const QHostAddress &localAddress, const QHostAddress &target, const QString &peerId,
                  const QUuid &localId, int timeoutMs);

    aurora::tailscale::AccountIdentity account_;
    quint64 generation_;
    QHostAddress localAddress_;
    QHostAddress target_;
    QString peerId_;
    QUuid localId_;
    QDeadlineTimer deadline_;
    QByteArray request_;
};

// Serialized params for the existing publish RPC. Preserve these bytes to keep the owner ID exact.
std::optional<QByteArray> embeddedPublishParameters(const aurora::tailscale::Status &status,
                                                    const Announcement &local);

} // namespace Aurora::Presence
