// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QMetaType>
#include <QString>
#include <QUrl>
#include <QVector>

#include <optional>

namespace aurora::tailscale {

enum class BackendState { Unknown, Starting, Stopped, NeedsLogin, NeedsApproval, Connected };

struct AccountIdentity
{
    qint64 ownerId;
    QString nodeId;
    QString tailnetId;

    bool operator==(const AccountIdentity &other) const
    {
        return ownerId == other.ownerId && nodeId == other.nodeId && tailnetId == other.tailnetId;
    }
};

struct Peer
{
    QString nodeId;
    QString name;
    QString platform;
    QHostAddress address;
    QHostAddress alternateAddress;
    bool online;
};

struct Status
{
    BackendState state = BackendState::Unknown;
    QUrl loginUrl;
    std::optional<AccountIdentity> account;
    QString displayName;
    QString loginName;
    QString tailnetName;
    QHostAddress localAddress;
    QVector<Peer> peers;
};

enum class StatusError {
    None, Unavailable, InvalidStateDirectory, StartFailed, Transport, Timeout,
    ResponseTooLarge, InvalidResponse, HelperRejected
};

struct StatusResult
{
    StatusError error = StatusError::InvalidResponse;
    std::optional<Status> status;
};

struct LoginResult
{
    StatusError error = StatusError::InvalidResponse;
    std::optional<bool> pending;
};

constexpr int maxStatusReplyBytes = 4 * 1024 * 1024;
constexpr int maxActionReplyBytes = 4 * 1024;

// Each decoder takes one JSON line without the terminating LF.
StatusResult decodeStatusReply(const QByteArray &line, const QString &requestId);
LoginResult decodeLoginReply(const QByteArray &line, const QString &requestId);
StatusError decodeLogoutReply(const QByteArray &line, const QString &requestId);

} // namespace aurora::tailscale

Q_DECLARE_METATYPE(aurora::tailscale::StatusError)
Q_DECLARE_METATYPE(aurora::tailscale::StatusResult)
Q_DECLARE_METATYPE(aurora::tailscale::LoginResult)
