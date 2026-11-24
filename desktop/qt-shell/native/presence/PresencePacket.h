// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <QUuid>

#include <optional>

namespace Aurora::Presence {

inline constexpr quint16 Port = 48988;
inline constexpr int PacketSize = 192;
inline constexpr int NonceSize = 16;
inline constexpr int NameSize = 140;

enum class Platform : quint8 { Unknown = 0, Windows = 1, MacOS = 2 };
QString platformName(Platform platform);

struct Announcement
{
    QUuid id;
    QString name;
    Platform platform = Platform::Unknown;
    bool hosting = false;
};

struct Response
{
    QHostAddress address;
    Announcement announcement;
};

QByteArray encodeRequest(const QByteArray &nonce);
QByteArray createRequest();

// The reply echoes the request nonce. An invalid request or local ID yields an empty packet.
QByteArray encodeReply(const QByteArray &request, const Announcement &local);

// The embedded helper copies the request nonce into this kind-2 template.
QByteArray announcementTemplate(const Announcement &local);

// The caller supplies the source validated by its transport.
std::optional<Response> decodeReply(const QByteArray &packet, const QByteArray &request,
                                     const QHostAddress &source, const QUuid &localId);

} // namespace Aurora::Presence
