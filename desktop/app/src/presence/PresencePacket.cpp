// SPDX-License-Identifier: GPL-3.0-only
#include "PresencePacket.h"

#include <QRandomGenerator>

#include <algorithm>
#include <cstring>

namespace Aurora::Presence {
namespace {

constexpr char Magic[] = "AURORA/PRESENCE/1";
constexpr int KindOffset = sizeof(Magic) - 1;
constexpr int NonceOffset = KindOffset + 1;
constexpr int IdOffset = NonceOffset + NonceSize;
constexpr int GuidSize = 16;
constexpr int PlatformOffset = IdOffset + GuidSize;
constexpr int HostingOffset = PlatformOffset + 1;
constexpr int NameOffset = HostingOffset + 1;
constexpr char RequestKind = 1;
constexpr char ReplyKind = 2;

static_assert(KindOffset == 17 && NameOffset + NameSize == PacketSize);

bool matches(const QByteArray &packet, char kind)
{
    return packet.size() == PacketSize && packet.at(KindOffset) == kind &&
        std::memcmp(packet.constData(), Magic, KindOffset) == 0;
}

void swapGuidFields(QByteArray &guid)
{
    // .NET Guid uses little-endian order for the first three fields.
    auto *bytes = guid.data();
    std::reverse(bytes, bytes + 4);
    std::swap(bytes[4], bytes[5]);
    std::swap(bytes[6], bytes[7]);
}

QByteArray encodeName(const QString &name)
{
    // Bound conversion and use the same replacement for unpaired surrogates as .NET.
    QString text = name.left(NameSize + 1);
    for (decltype(text.size()) i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            ++i;
        else if (text.at(i).isSurrogate())
            text[i] = QChar(0xfffd);
    }

    auto utf8 = text.toUtf8();
    if (utf8.size() > NameSize) {
        int length = NameSize;
        while ((static_cast<uchar>(utf8.at(length)) & 0xc0) == 0x80)
            --length;
        utf8.truncate(length);
    }
    return utf8;
}

int utf8SequenceSize(uchar lead)
{
    if (lead < 0x80) return 1;
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 0;
}

bool utf8Continuation(uchar lead, uchar next, int offset)
{
    if ((next & 0xc0) != 0x80)
        return false;
    if (offset != 1)
        return true;
    return !(lead == 0xe0 && next < 0xa0) && !(lead == 0xed && next >= 0xa0) &&
        !(lead == 0xf0 && next < 0x90) && !(lead == 0xf4 && next >= 0x90);
}

QString decodeName(const QByteArray &packet, const QHostAddress &source)
{
    const auto *name = packet.constData() + NameOffset;
    int size = NameSize;
    while (size > 0 && name[size - 1] == '\0')
        --size;
    if (std::memchr(name, '\0', size))
        return source.toString();

    // Match .NET replacement of each malformed UTF-8 prefix before decoding with Qt.
    QByteArray valid;
    valid.reserve(size);
    for (int pos = 0; pos < size;) {
        const auto lead = static_cast<uchar>(name[pos]);
        const int expected = utf8SequenceSize(lead);
        int length = 1;
        while (length < expected && pos + length < size &&
               utf8Continuation(lead, static_cast<uchar>(name[pos + length]), length))
            ++length;
        if (length == expected)
            valid.append(name + pos, length);
        else
            valid.append("\xef\xbf\xbd", 3);
        pos += length;
    }

    // A BOM inside this field is data. Decode under an ASCII prefix so Qt cannot strip it.
    const auto decoded = QString::fromUtf8(" " + valid).mid(1).trimmed();
    if (decoded.isEmpty() || std::any_of(decoded.cbegin(), decoded.cend(), [](QChar ch) {
            return ch.category() == QChar::Other_Control;
        }))
        return source.toString();
    return decoded;
}

Platform decodePlatform(char value)
{
    switch (value) {
    case 1: return Platform::Windows;
    case 2: return Platform::MacOS;
    default: return Platform::Unknown;
    }
}

QByteArray encodeAnnouncement(const QByteArray &nonce, const Announcement &local)
{
    if (local.id.isNull())
        return {};

    auto packet = encodeRequest(nonce);
    packet[KindOffset] = ReplyKind;
    auto guid = local.id.toRfc4122();
    swapGuidFields(guid);
    std::memcpy(packet.data() + IdOffset, guid.constData(), GuidSize);
    packet[PlatformOffset] = static_cast<char>(local.platform);
    packet[HostingOffset] = local.hosting ? 1 : 0;
    const auto name = encodeName(local.name);
    std::memcpy(packet.data() + NameOffset, name.constData(), name.size());
    return packet;
}

} // namespace

QString platformName(Platform platform)
{
    switch (platform) {
    case Platform::Windows: return QStringLiteral("windows");
    case Platform::MacOS: return QStringLiteral("macOS");
    default: return {};
    }
}

QByteArray encodeRequest(const QByteArray &nonce)
{
    if (nonce.size() != NonceSize)
        return {};
    QByteArray packet(PacketSize, '\0');
    std::memcpy(packet.data(), Magic, KindOffset);
    packet[KindOffset] = RequestKind;
    std::memcpy(packet.data() + NonceOffset, nonce.constData(), NonceSize);
    return packet;
}

QByteArray createRequest()
{
    quint32 words[NonceSize / sizeof(quint32)];
    QRandomGenerator::system()->fillRange(words);
    return encodeRequest(QByteArray(reinterpret_cast<const char *>(words), NonceSize));
}

QByteArray encodeReply(const QByteArray &request, const Announcement &local)
{
    return matches(request, RequestKind) ? encodeAnnouncement(request.mid(NonceOffset, NonceSize), local)
                                         : QByteArray{};
}

QByteArray announcementTemplate(const Announcement &local)
{
    return encodeAnnouncement(QByteArray(NonceSize, '\0'), local);
}

std::optional<Response> decodeReply(const QByteArray &packet, const QByteArray &request,
                                     const QHostAddress &source, const QUuid &localId)
{
    if (source.isNull() || localId.isNull() || !matches(packet, ReplyKind) || !matches(request, RequestKind) ||
        std::memcmp(packet.constData() + NonceOffset, request.constData() + NonceOffset, NonceSize) != 0)
        return std::nullopt;

    auto guid = packet.mid(IdOffset, GuidSize);
    swapGuidFields(guid);
    const auto id = QUuid::fromRfc4122(guid);
    if (id.isNull() || id == localId)
        return std::nullopt;

    return Response{source, {id, decodeName(packet, source), decodePlatform(packet.at(PlatformOffset)),
                              packet.at(HostingOffset) == 1}};
}

} // namespace Aurora::Presence
