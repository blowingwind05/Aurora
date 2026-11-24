// SPDX-License-Identifier: GPL-3.0-only
#include "PresencePacket.h"
#include "PresenceProbe.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QStringList>

#include <cstdio>
#include <stdexcept>

using namespace Aurora::Presence;
using namespace aurora::tailscale;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

const QUuid LocalId(QStringLiteral("{5e0d2b5f-79ec-47fc-a6be-5a22625546e7}"));
const QUuid RemoteId(QStringLiteral("{00112233-4455-6677-8899-aabbccddeeff}"));
const QByteArray Nonce = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
const QHostAddress Source(QStringLiteral("192.168.12.8"));
const Announcement Remote{RemoteId, QString::fromUtf16(u"Studio \u96ea"), Platform::MacOS, true};

void wire()
{
    const auto request = encodeRequest(Nonce);
    const QByteArray requestHeader = QByteArray::fromHex("4155524f52412f50524553454e43452f3101"
                                                        "000102030405060708090a0b0c0d0e0f");
    CHECK(request == requestHeader + QByteArray(158, '\0'));
    CHECK(encodeRequest(Nonce.left(15)).isEmpty());
    CHECK(encodeRequest(Nonce + 'x').isEmpty());

    // Produced with the legacy Encoding.UTF8 and Guid.ToByteArray layout.
    const auto legacyReply = QByteArray::fromBase64(
        "QVVST1JBL1BSRVNFTkNFLzECAAECAwQFBgcICQoLDA0ODzMiEQBVRHdmiJmqu8zd"
        "7v8CAVN0dWRpbyDpm6oAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(legacyReply.size() == PacketSize);
    CHECK(legacyReply.mid(34, 16).toHex() == "33221100554477668899aabbccddeeff");
    CHECK(encodeReply(request, Remote) == legacyReply);
    const auto decoded = decodeReply(legacyReply, request, Source, LocalId);
    CHECK(decoded && decoded->address == Source && decoded->announcement.id == RemoteId);
    CHECK(decoded->announcement.hosting && decoded->announcement.name == Remote.name);
    CHECK(platformName(decoded->announcement.platform) == "macOS");
    auto templ = announcementTemplate(Remote);
    CHECK(templ.mid(18, 16) == QByteArray(16, '\0'));
    templ.replace(18, 16, Nonce);
    CHECK(templ == legacyReply);

    auto dirtyRequest = request;
    dirtyRequest.replace(34, 158, QByteArray(158, 'x'));
    CHECK(encodeReply(dirtyRequest, Remote) == legacyReply);

    auto wrong = legacyReply;
    for (const auto offset : {0, 16, 17, 18, 33}) {
        wrong[offset] = '!';
        CHECK(!decodeReply(wrong, request, Source, LocalId));
        wrong = legacyReply;
    }
    CHECK(!decodeReply(legacyReply.left(PacketSize - 1), request, Source, LocalId));
    CHECK(!decodeReply(legacyReply + "x", request, Source, LocalId));
    CHECK(!decodeReply(legacyReply, QByteArray{}, Source, LocalId));
    CHECK(!decodeReply(legacyReply, request, Source, RemoteId));
    CHECK(!decodeReply(legacyReply, request, Source, QUuid{}));
    wrong.replace(34, 16, QByteArray(16, '\0'));
    CHECK(!decodeReply(wrong, request, Source, LocalId));
    CHECK(encodeReply(legacyReply, Remote).isEmpty());
    CHECK(announcementTemplate({}).isEmpty());

    auto flags = legacyReply;
    flags[50] = 1;
    flags[51] = 2;
    const auto windows = decodeReply(flags, request, Source, LocalId);
    CHECK(windows && !windows->announcement.hosting && platformName(windows->announcement.platform) == "windows");
    flags[50] = 42;
    const auto unknown = decodeReply(flags, request, Source, LocalId);
    CHECK(unknown && unknown->announcement.platform == Platform::Unknown);
    CHECK(platformName(unknown->announcement.platform).isEmpty());

    const auto first = createRequest();
    const auto second = createRequest();
    CHECK(first.size() == PacketSize && second.size() == PacketSize);
    CHECK(first.mid(18, 16) != second.mid(18, 16));
    CHECK(first.mid(34) == QByteArray(158, '\0'));
}

void names()
{
    const auto request = encodeRequest(Nonce);
    const auto getName = [&](const QByteArray &rawName) {
        auto packet = encodeReply(request, Remote);
        packet.replace(52, NameSize, rawName + QByteArray(NameSize - rawName.size(), '\0'));
        const auto decoded = decodeReply(packet, request, Source, LocalId);
        CHECK(decoded);
        return decoded->announcement.name;
    };

    CHECK(getName("  Studio\t\r\n") == "Studio");
    CHECK(getName("\t ") == Source.toString());
    CHECK(getName("A\tB") == Source.toString());
    CHECK(getName(QByteArray("A\0B", 3)) == Source.toString());
    CHECK(getName(QByteArray("A\x7f" "B", 3)) == Source.toString());
    CHECK(getName(QByteArray::fromHex("c28553747564696f")) == "Studio");

    const QString replacement(QChar(0xfffd));
    CHECK(getName(QByteArray::fromHex("e282")) == replacement);
    CHECK(getName(QByteArray::fromHex("e28241")) == replacement + QLatin1Char('A'));
    CHECK(getName(QByteArray::fromHex("eda080")) == replacement.repeated(3));
    CHECK(getName(QByteArray::fromHex("f4908080")) == replacement.repeated(4));
    CHECK(getName(QByteArray::fromHex("efbbbf58")) == QChar(0xfeff) + QStringLiteral("X"));

    auto local = Remote;
    const auto snow = QString::fromUtf16(u"\u96ea");
    local.name = QString(137, QLatin1Char('x')) + snow;
    auto packet = encodeReply(request, local);
    CHECK(packet.mid(52) == local.name.toUtf8());
    local.name.prepend(QLatin1Char('x'));
    packet = encodeReply(request, local);
    CHECK(packet.mid(52) == QByteArray(138, 'x') + QByteArray(2, '\0'));

    const auto emoji = QString::fromUtf16(u"\U0001f600");
    local.name = QString(136, QLatin1Char('x')) + emoji;
    packet = encodeReply(request, local);
    CHECK(packet.mid(52) == local.name.toUtf8());
    local.name.prepend(QLatin1Char('x'));
    packet = encodeReply(request, local);
    CHECK(packet.mid(52) == QByteArray(137, 'x') + QByteArray(3, '\0'));

    local.name = QString(QChar(0xd800));
    CHECK(encodeReply(request, local).mid(52, 3).toHex() == "efbfbd");
}

void datagram()
{
    const QHostAddress mapped("::ffff:192.168.12.8");
    const QHostAddress alternate("fd7a:115c:a1e0::8");
    const QHostAddress outside("192.168.13.8");
    const auto allowSubnet = [](const QHostAddress &address) {
        return address.isInSubnet(QHostAddress("192.168.12.0"), 24);
    };

    const auto request = encodeRequest(Nonce);
    CHECK(answerDatagram(request, Remote, mapped, allowSubnet) == encodeReply(request, Remote));
    CHECK(answerDatagram(request, Remote, outside, allowSubnet).isEmpty());
    CHECK(answerDatagram(request, Remote, Source, {}).isEmpty());
    CHECK(answerDatagram(request.left(191), Remote, Source, allowSubnet).isEmpty());

    auto probe = DatagramProbe::unicast(LocalId, {Source, mapped, alternate});
    CHECK(probe.remainingMilliseconds() > 0 && probe.remainingMilliseconds() <= ProbeTimeoutMs);
    const auto reply = encodeReply(probe.request(), Remote);
    CHECK(!probe.addReply(reply, outside, Port));
    CHECK(!probe.addReply(reply, Source, 48989));
    CHECK(!probe.addReply(encodeReply(request, Remote), Source, Port));
    CHECK(probe.addReply(reply, mapped, Port));
    auto renamed = Remote;
    renamed.name = "Renamed";
    CHECK(probe.addReply(encodeReply(probe.request(), renamed), Source, Port));
    CHECK(probe.replies().size() == 1 && probe.replies().first().address == Source);
    CHECK(probe.replies().first().announcement.name == renamed.name);
    CHECK(probe.addReply(reply, alternate, Port));
    CHECK(probe.replies().size() == 2 && probe.remainingMilliseconds() == 0);
    CHECK(!probe.addReply(reply, alternate, Port));

    auto lan = DatagramProbe::lan(LocalId, allowSubnet);
    const auto lanReply = encodeReply(lan.request(), Remote);
    CHECK(!lan.addReply(lanReply, outside, Port));
    CHECK(lan.addReply(lanReply, mapped, Port));
    lan.cancel();
    CHECK(!lan.addReply(lanReply, Source, Port));
    CHECK(lan.replies().size() == 1 && lan.remainingMilliseconds() == 0);

    auto capped = DatagramProbe::lan(LocalId, [](const QHostAddress &) { return true; });
    const auto cappedReply = encodeReply(capped.request(), Remote);
    for (quint32 index = 1; index <= LanReplyLimit; ++index)
        CHECK(capped.addReply(cappedReply, QHostAddress(0x0a000000u + index), Port));
    CHECK(capped.replies().size() == LanReplyLimit);
    CHECK(!capped.addReply(cappedReply, QHostAddress("10.0.4.7"), Port));
    CHECK(capped.remainingMilliseconds() == 0);

    auto expired = DatagramProbe::unicast(LocalId, {Source}, 0);
    CHECK(!expired.addReply(encodeReply(expired.request(), Remote), Source, Port));
    CHECK(DatagramProbe::unicast(LocalId, {}).remainingMilliseconds() == 0);
    CHECK(DatagramProbe::lan(LocalId, {}).remainingMilliseconds() == 0);
    CHECK(DatagramProbe::unicast(QUuid{}, {Source}).remainingMilliseconds() == 0);
}

void embedded()
{
    const QHostAddress address("100.64.2.1");
    const QHostAddress alternate("fd7a:115c:a1e0::123");
    Status status;
    status.state = BackendState::Connected;
    status.account = AccountIdentity{9007199254740993LL, "local-node", "tailnet-one"};
    status.localAddress = QHostAddress("100.64.2.9");
    status.peers.append(Peer{"desk", "Studio", "macOS", address, alternate, true});

    const auto probe = EmbeddedProbe::start(status, 17, alternate, LocalId);
    CHECK(probe && probe->remainingMilliseconds() > 0);
    const auto parameters = probe->requestParameters();
    CHECK(parameters.size() == 3);
    CHECK(parameters.value("peerId").toString() == "desk");
    CHECK(parameters.value("address").toString() == alternate.toString());
    const auto request = QByteArray::fromBase64(parameters.value("packet").toString().toLatin1());
    CHECK(request.size() == PacketSize && request.at(17) == 1);

    auto reply = encodeReply(request, Remote);
    const QJsonValue result(QString::fromLatin1(reply.toBase64()));
    const auto decoded = probe->decodeResult(result, status, 17);
    CHECK(decoded && decoded->address == alternate && decoded->announcement.id == RemoteId);
    CHECK(!probe->decodeResult(result, status, 18));
    CHECK(!probe->decodeResult(QJsonValue(false), status, 17));
    CHECK(!probe->decodeResult(QJsonValue(QString(256, QLatin1Char('?'))), status, 17));
    CHECK(!probe->decodeResult(QJsonValue(QString(257, QLatin1Char('A'))), status, 17));
    reply[18] = static_cast<char>(reply.at(18) ^ 1);
    CHECK(!probe->decodeResult(QJsonValue(QString::fromLatin1(reply.toBase64())), status, 17));

    auto changed = status;
    ++changed.account->ownerId;
    CHECK(!probe->decodeResult(result, changed, 17));
    changed = status;
    changed.account->nodeId = "another-local-node";
    CHECK(!probe->decodeResult(result, changed, 17));
    changed = status;
    changed.account->tailnetId = "another-tailnet";
    CHECK(!probe->decodeResult(result, changed, 17));
    changed = status;
    changed.localAddress = QHostAddress("100.64.2.10");
    CHECK(!probe->decodeResult(result, changed, 17));
    changed = status;
    changed.peers[0].nodeId = "another-device";
    CHECK(!probe->decodeResult(result, changed, 17));
    changed = status;
    changed.state = BackendState::Stopped;
    CHECK(!EmbeddedProbe::start(changed, 17, address, LocalId));
    CHECK(!probe->decodeResult(result, changed, 17));
    CHECK(!embeddedPublishParameters(changed, Remote));

    changed = status;
    changed.peers.append(Peer{"another", "Studio", "windows", alternate, {}, true});
    CHECK(!EmbeddedProbe::start(changed, 17, alternate, LocalId));
    CHECK(!probe->decodeResult(result, changed, 17));
    CHECK(!EmbeddedProbe::start(status, 17, QHostAddress("100.64.2.20"), LocalId));
    CHECK(!EmbeddedProbe::start(status, 17, status.localAddress, LocalId));
    const auto expired = EmbeddedProbe::start(status, 17, alternate, LocalId, 0);
    CHECK(expired && expired->requestParameters().isEmpty() && !expired->decodeResult(result, status, 17));

    const auto publication = embeddedPublishParameters(status, Remote);
    CHECK(publication && publication->contains("\"owner\":9007199254740993"));
    const auto publishObject = QJsonDocument::fromJson(*publication).object();
    CHECK(publishObject.size() == 4 && publishObject.value("enabled").toBool());
    CHECK(publishObject.value("nodeId").toString() == "local-node");
    CHECK(QByteArray::fromBase64(publishObject.value("packet").toString().toLatin1()) == announcementTemplate(Remote));
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QHash<QString, void (*)()> fixtures{
        {"wire", wire}, {"names", names}, {"datagram", datagram}, {"embedded", embedded}
    };
    try {
        CHECK(app.arguments().size() == 2);
        const auto run = fixtures.value(app.arguments().at(1));
        CHECK(run);
        run();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Presence fixture failed: %s\n", error.what());
        return 1;
    }
}
