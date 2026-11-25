// SPDX-License-Identifier: GPL-3.0-only
#include "Discovery.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <stdexcept>

using namespace Aurora::Discovery;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

const QLocale English(QLocale::English, QLocale::UnitedStates);
const QUuid StudioId(QStringLiteral("{1eaac04a-091f-4a77-a22d-09dce3c597ee}"));
const QUuid OtherId(QStringLiteral("{acc71c8e-5b4d-4a6f-9ffc-61a8dbe08571}"));
const QHostAddress Near(QStringLiteral("192.168.12.8"));
const QHostAddress Far(QStringLiteral("100.64.4.8"));
const QHostAddress Far6(QStringLiteral("fd7a:115c:a1e0::8"));

Device peer(const QString &id, const QString &name, const QHostAddress &address,
            const QHostAddress &alternate = {}, bool online = true)
{
    Device result;
    result.id = id;
    result.name = name;
    result.platform = QStringLiteral("windows");
    result.address = address;
    result.alternateAddress = alternate;
    result.online = online;
    return result;
}

PresentDevice presence(const QUuid &id, const QHostAddress &address)
{
    return {id, address, QStringLiteral("Studio"), QStringLiteral("windows"), true};
}

StreamHost host(const QHostAddress &address, const QString &id, quint16 port = BundledSunshinePort)
{
    return {{address, port}, id, QStringLiteral("Studio")};
}

template<typename Action>
void checkInvalidInput(Action action)
{
    try {
        action();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("Expected duplicate address rejection");
}

void catalog()
{
    // Cover combined, stock, multi-LAN, and IPv6 discovery inputs.
    const auto studio = peer("tailscale-node", "Studio", Far, Far6);
    const auto localHost = host(Near, "HOST-A");
    const auto remoteHost = host(Far, "host-a");
    const auto lanOnlyAddress = QHostAddress("192.168.12.9");
    LanSnapshot lan{{}, {localHost}, {presence(StudioId, QHostAddress("192.168.13.8")), presence(StudioId, Near)}};
    TailnetSnapshot tailnet{{remoteHost}, {presence(StudioId, Far)}, {{studio.id, StudioId}}};

    auto devices = merge({studio}, &lan, &tailnet, English);
    CHECK(devices.size() == 1);
    CHECK(devices[0].id == studio.id && devices[0].platform == studio.platform && devices[0].tailnetVisible);
    CHECK(devices[0].online && devices[0].canConnect() && devices[0].lan && devices[0].tailnet);
    CHECK(devices[0].lanAddress == Near && devices[0].tailnet->endpoint.address == Far);

    auto embedded = host(Far6, "host-a");
    embedded.viaEmbeddedTailnet = true;
    embedded.supportsSessionPrivacy = true;
    lan.hosts[0].encoderProbePending = true;
    tailnet.hosts = {embedded};
    devices = merge({studio}, &lan, &tailnet);
    CHECK(devices[0].canConnect() && devices[0].lan->encoderProbePending);
    CHECK(devices[0].tailnet->viaEmbeddedTailnet && devices[0].tailnet->supportsSessionPrivacy);
    CHECK(devices[0].tailnet->endpoint.address == Far6 && devices[0].alternateAddress == Far6);

    tailnet.hosts = {embedded, remoteHost};
    devices = merge({studio}, &lan, &tailnet);
    CHECK(devices[0].tailnet->endpoint.address == Far);
    checkInvalidInput([&] {
        auto duplicate = tailnet;
        duplicate.hosts.append(remoteHost);
        merge({studio}, &lan, &duplicate);
    });

    const auto stockLan = host(Near, "stock-host", 47989);
    const auto stockTailnet = host(Far, "stock-host", 47989);
    lan.hosts = {stockLan, localHost};
    tailnet.hosts = {stockTailnet, remoteHost};
    CHECK(merge({studio}, &lan, &tailnet)[0].lan->endpoint.port == BundledSunshinePort);
    CHECK(merge({studio}, &lan, &tailnet)[0].tailnet->endpoint.port == BundledSunshinePort);
    lan.hosts = {stockLan};
    tailnet.hosts = {stockTailnet};
    devices = merge({studio}, &lan, &tailnet);
    CHECK(devices.size() == 1 && devices[0].online && !devices[0].canConnect());
    CHECK(devices[0].lanAddress == QHostAddress("192.168.13.8"));

    lan.hosts = {host(Near, "host-b")};
    tailnet.hosts = {remoteHost};
    devices = merge({studio}, &lan, &tailnet);
    CHECK(devices.size() == 2 && devices[0].tailnet && !devices[0].lan);
    CHECK(devices[1].id == "lan:1eaac04a091f4a77a22d09dce3c597ee" && devices[1].lan && !devices[1].tailnetVisible);

    lan.hosts = {host(lanOnlyAddress, "host-a")};
    lan.devices = {presence(OtherId, lanOnlyAddress)};
    devices = merge({studio}, &lan, &tailnet);
    CHECK(devices.size() == 2 && !devices[0].lan && devices[1].lan);

    const LanSnapshot portsOnly{{}, {localHost}, {}};
    const TailnetSnapshot unknown{{remoteHost}, {presence(StudioId, Far)}, {{"TAILSCALE-NODE", StudioId}}};
    CHECK(merge({studio}, &portsOnly, &unknown).isEmpty());
    CHECK(merge({studio}, nullptr, nullptr).isEmpty());
    CHECK(merge({studio}, &lan, nullptr).size() == 1);

    const TailnetSnapshot offline{{}, {}, {{studio.id, StudioId}}};
    devices = merge({studio}, nullptr, &offline);
    CHECK(devices.size() == 1 && !devices[0].online && !devices[0].canConnect());
    lan.hosts = {localHost};
    lan.devices = {presence(StudioId, Near)};
    devices = merge({studio}, &lan, &offline);
    CHECK(devices.size() == 1 && devices[0].online && devices[0].lan);

    // Tailnet presence alone (even just the alternate address) can make a known node online.
    const TailnetSnapshot present{{}, {presence(StudioId, Far6)}, {{studio.id, StudioId}}};
    devices = merge({studio}, nullptr, &present);
    CHECK(devices[0].online && !devices[0].canConnect());

    // Ordering is stable for names equal under the current-locale ignore-case comparison.
    auto alphabet = peer("alpha", "ALPHA", QHostAddress("100.64.4.2"));
    auto lower = peer("lower", "alpha", QHostAddress("100.64.4.3"));
    auto zulu = peer("zulu", "Zulu", QHostAddress("100.64.4.4"));
    auto early = peer("early", "aardvark", QHostAddress("100.64.4.5"));
    const TailnetSnapshot ordered{{host(zulu.address, "online")}, {presence(StudioId, alphabet.address),
        presence(StudioId, lower.address)}, {{early.id, OtherId}, {lower.id, OtherId},
        {alphabet.id, OtherId}, {zulu.id, OtherId}}};
    devices = merge({early, lower, alphabet, zulu}, nullptr, &ordered, English);
    CHECK(devices.size() == 4);
    CHECK(devices[0].id == zulu.id && devices[1].id == lower.id && devices[2].id == alphabet.id);
    CHECK(devices[3].id == early.id && !devices[3].online);
}

void trust()
{
    // Two same-user peers share the name "studio". Use their stable IDs and prefer IPv4.
    const auto desk = peer("desk", "studio", QHostAddress("100.64.2.1"), QHostAddress("fd7a:115c:a1e0::123"));
    const auto lap = peer("lap", "studio", QHostAddress("fd7a:115c:a1e0::124"), {}, false);
    const QHostAddress self("100.64.2.90");
    NetworkSnapshot network{self, 42, "current", {desk, lap}};
    CHECK(network.isAuthenticated());
    CHECK(tailnetAddresses(desk) == QList<QHostAddress>({desk.address, desk.alternateAddress}));
    CHECK(tailnetAddresses(lap) == QList<QHostAddress>({lap.address}));
    auto duplicateAddress = desk;
    duplicateAddress.alternateAddress = duplicateAddress.address;
    CHECK(tailnetAddresses(duplicateAddress).size() == 1);

    CHECK(findTailnetPeer(&network, desk.address) == &network.devices.constFirst());
    CHECK(findTailnetPeer(&network, QHostAddress("::ffff:100.64.2.1")) == &network.devices.constFirst());
    CHECK(findTailnetPeer(&network, desk.alternateAddress) == &network.devices.constFirst());
    CHECK(findTailnetPeer(&network, lap.address) == &network.devices.constLast());
    CHECK(!findTailnetPeer(&network, QHostAddress("::100.64.2.1")));
    for (const auto &source : {"127.1.2.3", "::1", "0.0.0.0", "::", "::ffff:0.0.0.0", "100.64.2.90",
                              "::ffff:100.64.2.90"}) {
        auto blocked = network;
        const QHostAddress address(source);
        blocked.devices.append(peer("blocked", "blocked", address));
        CHECK(!findTailnetPeer(&blocked, address));
    }
    CHECK(!findTailnetPeer(&network, QHostAddress("100.64.2.99")));
    network.devices.append(peer("duplicate", "studio", desk.alternateAddress));
    CHECK(!findTailnetPeer(&network, desk.alternateAddress));
    network.owner = 0;
    CHECK(!findTailnetPeer(&network, desk.address));

    const KnownIdentities saved{{desk.id, OtherId}, {lap.id, OtherId}, {"stale", StudioId}};
    auto known = resolveKnownIdentities({desk, lap}, {presence(StudioId, desk.address),
        presence(StudioId, desk.alternateAddress)}, saved);
    CHECK(known.size() == 2 && known.value(desk.id) == StudioId && known.value(lap.id) == OtherId);

    known = resolveKnownIdentities({desk, lap}, {presence(StudioId, desk.address),
        presence(OtherId, desk.alternateAddress)}, saved);
    CHECK(!known.contains(desk.id) && known.value(lap.id) == OtherId);
    CHECK(resolveKnownIdentities({desk}, {presence(StudioId, QHostAddress("::ffff:100.64.2.1"))}, {}).isEmpty());
    CHECK(resolveKnownIdentities({desk}, {}, {{"DESK", StudioId}}).isEmpty());
    checkInvalidInput([&] {
        resolveKnownIdentities({desk}, {presence(StudioId, desk.address), presence(OtherId, desk.address)}, saved);
    });
}

void routes()
{
    auto studio = peer("tailscale-node", "Studio", Far, Far6);
    studio.lan = host(Near, "host-a");
    studio.tailnet = host(Far6, "HOST-A");
    studio.tailnet->viaEmbeddedTailnet = true;
    studio.tailnet->supportsPower = true;
    CHECK(studio.canConnect() && studio.supportsPower());

    QList<QHostAddress> attempts;
    auto selected = chooseRoute(studio, [&](const StreamHost &route) {
        attempts.append(route.endpoint.address);
        auto current = route;
        current.id = "Host-A";
        current.encoderProbePending = true;
        return current;
    });
    CHECK(attempts == QList<QHostAddress>({Near}));
    CHECK(selected && selected->route.id == "host-a" && !selected->route.encoderProbePending);
    CHECK(selected->current.encoderProbePending);

    attempts.clear();
    selected = chooseRoute(studio, [&](const StreamHost &route) -> std::optional<StreamHost> {
        attempts.append(route.endpoint.address);
        if (route.endpoint.address == Near)
            return std::nullopt;
        CHECK(route.viaEmbeddedTailnet);
        return route;
    });
    CHECK(attempts == QList<QHostAddress>({Near, Far6}));
    CHECK(selected && selected->route.endpoint.address == Far6 && selected->route.viaEmbeddedTailnet);

    selected = chooseRoute(studio, [&](const StreamHost &route) {
        auto current = route;
        if (route.endpoint.address == Near)
            current.id = "different-host";
        return current;
    });
    CHECK(selected && selected->route.endpoint.address == Far6);

    selected = chooseRoute(studio, [](const StreamHost &route) { return route; }, RouteRequirement::Power);
    CHECK(selected && selected->route.endpoint.address == Far6);
    CHECK(!chooseRoute(studio, [](const StreamHost &route) {
        auto current = route;
        current.supportsPower = false;
        return current;
    }, RouteRequirement::Power));
    CHECK(!chooseRoute(studio, [](const StreamHost &) -> std::optional<StreamHost> { return std::nullopt; }));
    CHECK(!chooseRoute({}, [](const StreamHost &) -> std::optional<StreamHost> {
        throw std::runtime_error("A device without routes must never be probed");
    }));

    CHECK(sameHostIdentity("HOST-A", "host-a"));
    CHECK(!sameHostIdentity("host-a ", "host-a"));
    const auto kelvinSign = QString::fromUtf16(u"\u212a");
    const auto longS = QString::fromUtf16(u"\u017f");
    const auto upperSigma = QString::fromUtf16(u"\u03a3");
    const auto finalSigma = QString::fromUtf16(u"\u03c2");
    const auto upperDeseret = QString::fromUtf16(u"\U00010400");
    const auto lowerDeseret = QString::fromUtf16(u"\U00010428");
    CHECK(!sameHostIdentity("K", kelvinSign));
    CHECK(!sameHostIdentity("S", longS));
    CHECK(sameHostIdentity(upperSigma, finalSigma));
    CHECK(sameHostIdentity(upperDeseret, lowerDeseret));
}

void consumer()
{
    const auto desk = peer("desk", "studio", QHostAddress("100.64.2.1"), QHostAddress("fd7a:115c:a1e0::123"));
    const auto lap = peer("lap", "studio", QHostAddress("fd7a:115c:a1e0::124"), {}, false);
    const auto unrelated = peer("other", "studio", QHostAddress("100.64.2.3"));
    const auto self = QHostAddress("100.64.2.90");
    NetworkSnapshot network{self, 42, "current", {desk, lap, unrelated}};
    const LanSnapshot lan{{}, {host(Near, "host-a")}, {presence(StudioId, Near),
        presence(OtherId, QHostAddress("192.168.12.9"))}};
    const TailnetSnapshot tailnet{{host(desk.alternateAddress, "host-a")}, {},
        {{desk.id, StudioId}, {lap.id, QUuid("{f57e8767-ffaa-482e-91a0-ed7ebe4a4390}")}}};

    CHECK(merge(network.devices, &lan, &tailnet).size() == 3);
    CHECK(visibleDevices(nullptr, &lan, &tailnet).isEmpty());
    network.owner = 0;
    CHECK(visibleDevices(&network, &lan, &tailnet).isEmpty());
    network.owner = 42;
    network.localId = "  ";
    CHECK(visibleDevices(&network, &lan, &tailnet).isEmpty());
    network.localId = "current";
    network.localAddress = {};
    CHECK(visibleDevices(&network, &lan, &tailnet).isEmpty());
    network.localAddress = self;
    CHECK(visibleDevices(&network, &lan, nullptr).isEmpty());

    const auto visible = visibleDevices(&network, &lan, &tailnet);
    CHECK(visible.size() == 2);
    CHECK(visible[0].id == desk.id && visible[1].id == lap.id);
    const auto row = qtDeviceRow(visible[0], "file:///fixture.webp");
    CHECK(row.size() == 8);
    CHECK(row.value("deviceId").toString() == desk.id && row.value("displayName").toString() == "studio");
    CHECK(row.value("online").toBool() && row.value("canConnect").toBool());
    CHECK(row.value("supportsPower").isBool() && !row.value("supportsPower").toBool());
    CHECK(row.value("lanAddress").toString() == Near.toString());
    CHECK(row.value("tailscaleAddress").toString() == desk.alternateAddress.toString());
    CHECK(row.value("wallpaperUrl").toString() == "file:///fixture.webp");

    const auto offline = qtDeviceRow(visible[1]);
    CHECK(!offline.value("online").toBool() && !offline.value("canConnect").toBool());
    CHECK(offline.value("lanAddress").isString() && offline.value("lanAddress").toString().isEmpty());
    CHECK(offline.value("tailscaleAddress").toString() == lap.address.toString());
    auto noLanPresence = visible[0];
    noLanPresence.lanAddress = {};
    CHECK(qtDeviceRow(noLanPresence).value("lanAddress").toString() == Near.toString());
}

void sessionLan()
{
    const auto studio = peer("peer", "Studio", Far, Far6);
    NetworkSnapshot network{QHostAddress("100.64.2.90"), 42, "current", {studio}};
    const KnownIdentities known{{studio.id, StudioId}, {"revoked", StudioId}};
    const QHostAddress moved("192.168.3.22");
    const QHostAddress another("192.168.3.23");
    const QHostAddress disabled("192.168.3.24");
    auto notHosting = presence(StudioId, disabled);
    notHosting.hosting = false;

    CHECK(nearbyHostAddresses(network, known, studio.id, {}).isEmpty());
    QList<PresentDevice> replies{presence(OtherId, Near), presence(StudioId, moved),
        presence(StudioId, moved), notHosting, presence(StudioId, another)};
    CHECK(nearbyHostAddresses(network, known, studio.id, replies) == (QList<QHostAddress>{moved, another}));
    replies.append(presence(OtherId, moved));
    CHECK(nearbyHostAddresses(network, known, studio.id, replies) == (QList<QHostAddress>{another}));
    CHECK(nearbyHostAddresses(network, known, "revoked", replies).isEmpty());
    CHECK(nearbyHostAddresses(network, {}, studio.id, replies).isEmpty());
    network.owner = 0;
    CHECK(nearbyHostAddresses(network, known, studio.id, replies).isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QHash<QString, void (*)()> fixtures{
        {"catalog", catalog}, {"trust", trust}, {"routes", routes}, {"consumer", consumer}, {"session-lan", sessionLan}
    };

    try {
        CHECK(app.arguments().size() == 2);
        const auto run = fixtures.value(app.arguments().at(1));
        CHECK(run);
        run();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Discovery fixture failed: %s\n", error.what());
        return 1;
    }
}
