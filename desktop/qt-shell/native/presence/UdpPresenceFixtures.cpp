// SPDX-License-Identifier: GPL-3.0-only
#include "UdpPresenceTransport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QStringList>
#include <QThread>
#include <QUdpSocket>

#include <cstdio>
#include <stdexcept>

using namespace Aurora::Presence;
using namespace aurora::tailscale;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

const QHostAddress Loop4(QHostAddress::LocalHost);
const QHostAddress Loop6(QHostAddress::LocalHostIPv6);
const QHostAddress OtherLoop4(QStringLiteral("127.0.0.2"));
const UdpBindings Loopback4{Loop4, {}};
const Announcement Local{QUuid("{5e0d2b5f-79ec-47fc-a6be-5a22625546e7}"), "Local", Platform::Windows, false};
const Announcement Remote{QUuid("{00112233-4455-6677-8899-aabbccddeeff}"), "Remote", Platform::MacOS, true};

template<typename Predicate>
bool waitFor(Predicate ready, int milliseconds = 1500)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        if (!ready())
            QThread::msleep(1);
    }
    return ready();
}

void bind4(QUdpSocket &socket, quint16 port)
{
    socket.setProxy(QNetworkProxy::NoProxy);
    CHECK(socket.bind(Loop4, port, QUdpSocket::DontShareAddress));
}

QNetworkDatagram receive(QUdpSocket &socket)
{
    CHECK(waitFor([&socket] { return socket.hasPendingDatagrams(); }));
    auto result = socket.receiveDatagram(PacketSize + 1);
    CHECK(result.isValid());
    return result;
}

void send(QUdpSocket &socket, const QByteArray &bytes, const QHostAddress &target, quint16 port)
{
    CHECK(target.isLoopback());
    CHECK(socket.writeDatagram(bytes, target, port) == bytes.size());
}

// The actual account provider only supplies normalized Tailnet addresses. These
// synthetic snapshots use loopback so the transport fixture stays local.
Status systemStatus(const QHostAddress &primary = Loop4, const QHostAddress &alternate = {})
{
    Status status;
    status.state = BackendState::Connected;
    status.account = AccountIdentity{42, "local-node", "tailnet"};
    status.localAddress = QHostAddress("100.64.2.9");
    status.peers.append(Peer{"remote-peer", "Fixture", "windows", primary, alternate, true});
    return status;
}

using Results = QHash<quint64, ScanResult>;

void collect(UdpPresenceTransport &transport, Results &results)
{
    QObject::connect(&transport, &UdpPresenceTransport::scanFinished, &transport,
                     [&results](quint64 request, const ScanResult &result) { results.insert(request, result); });
}

void subnet()
{
    const LanSubnet link{QHostAddress("192.168.12.17"), QHostAddress("255.255.255.0")};
    const auto neighbor = QHostAddress("192.168.12.8");
    CHECK(link.isUsable());
    CHECK(link.broadcastAddress() == QHostAddress("192.168.12.255"));
    CHECK(link.contains(neighbor) && link.contains(QHostAddress("::ffff:192.168.12.8")));
    for (const auto &address : {"192.168.12.0", "192.168.12.17", "192.168.12.255", "192.168.13.8",
                                "127.0.0.1", "100.64.12.8", "224.0.0.1", "::1"}) {
        CHECK(!link.contains(QHostAddress(address)));
    }

    const LanSubnet nonOctet{QHostAddress("192.168.12.65"), QHostAddress("255.255.255.192")};
    CHECK(nonOctet.broadcastAddress() == QHostAddress("192.168.12.127"));
    CHECK(nonOctet.contains(QHostAddress("192.168.12.126")));
    CHECK(!nonOctet.contains(QHostAddress("192.168.12.64")));
    CHECK(!nonOctet.contains(QHostAddress("192.168.12.127")));
    const LanSubnet pointToPoint{QHostAddress("192.168.12.8"), QHostAddress("255.255.255.254")};
    CHECK(!pointToPoint.contains(QHostAddress("192.168.12.9")));
    const LanSubnet noMask{link.address, {}};
    const LanSubnet loopback{Loop4, QHostAddress("255.0.0.0")};
    const LanSubnet tailnet{QHostAddress("100.64.12.17"), link.mask};
    CHECK(!noMask.isUsable() && noMask.broadcastAddress().isNull());
    CHECK(!loopback.isUsable() && !tailnet.isUsable());

    // Feed synthetic datagrams through the same per-link probe and source predicate.
    auto probe = DatagramProbe::lan(Local.id, [link](const QHostAddress &address) { return link.contains(address); });
    const auto valid = encodeReply(probe.request(), Remote);
    CHECK(!probe.addReply(valid, link.broadcastAddress(), Port));
    CHECK(!probe.addReply(valid, neighbor, Port + 1));
    CHECK(probe.addReply(valid, neighbor, Port));
    CHECK(probe.addReply(valid, QHostAddress("::ffff:192.168.12.8"), Port));
    CHECK(probe.replies().size() == 1);

    Results results;
    UdpPresenceTransport transport(Local, Loopback4);
    collect(transport, results);
    transport.updateAccess({link}, nullptr, 1);
    const auto skipped = transport.probeLan();
    CHECK(waitFor([&] { return results.contains(skipped); }));
    CHECK(results.value(skipped).status == ScanStatus::Complete && results.value(skipped).replies.isEmpty());
}

void responder()
{
    UdpPresenceTransport responder(Remote, Loopback4);
    const auto bound = responder.startResponding();
    CHECK(bound.ipv4 && !bound.ipv6);
    UdpPresenceTransport second(Remote, Loopback4);
    CHECK(!second.startResponding().ipv4);

    QUdpSocket sender;
    bind4(sender, 0);
    const auto request = encodeRequest(QByteArray(NonceSize, 'Q'));
    send(sender, request, Loop4, Port);
    CHECK(!waitFor([&] { return sender.hasPendingDatagrams(); }, 75));

    // IPv4-mapped configured addresses allow the actual IPv4 sender.
    auto status = systemStatus(QHostAddress("::ffff:127.0.0.1"));
    responder.updateAccess({}, &status, 2);
    send(sender, request + QByteArray(512, 'x'), Loop4, Port);
    send(sender, request.left(PacketSize - 1), Loop4, Port);
    send(sender, encodeReply(request, Local), Loop4, Port);
    CHECK(!waitFor([&] { return sender.hasPendingDatagrams(); }, 75));

    send(sender, request, Loop4, Port);
    const auto response = receive(sender);
    CHECK(response.senderAddress() == Loop4 && response.senderPort() == Port);
    CHECK(response.data() == encodeReply(request, Remote));

    responder.updateAnnouncement("Fresh name", Platform::Windows, false);
    send(sender, request, Loop4, Port);
    const auto updated = decodeReply(receive(sender).data(), request, Loop4, Local.id);
    CHECK(updated && updated->announcement.name == "Fresh name" && !updated->announcement.hosting);
    CHECK(updated->announcement.platform == Platform::Windows);

    status.peers.append(Peer{"ambiguous", "Fixture", "windows", Loop4, {}, true});
    responder.updateAccess({}, &status, 2);
    send(sender, request, Loop4, Port);
    CHECK(!waitFor([&] { return sender.hasPendingDatagrams(); }, 75));

    status.peers.removeLast();
    responder.updateAccess({}, nullptr, 3);
    responder.updateAccess({}, &status, 2); // A late status update from the prior account.
    send(sender, request, Loop4, Port);
    CHECK(!waitFor([&] { return sender.hasPendingDatagrams(); }, 75));

    responder.stopResponding();
    CHECK(second.startResponding().ipv4);
}

void probe()
{
    QUdpSocket remote;
    QUdpSocket wrongPort;
    bind4(remote, Port);
    bind4(wrongPort, 0);

    Results results;
    UdpPresenceTransport transport(Local, Loopback4);
    collect(transport, results);
    const auto status = systemStatus(Loop4, OtherLoop4);
    transport.updateAccess({}, &status, 1);

    // Wrong port, wrong nonce, oversized and self replies cannot complete the scan.
    const auto directed = transport.probePeers({Loop4, QHostAddress("::ffff:127.0.0.1")});
    const auto request = receive(remote);
    CHECK(request.senderAddress() == Loop4 && request.senderPort() != Port);
    const auto reply = encodeReply(request.data(), Remote);
    CHECK(reply.size() == PacketSize);
    send(wrongPort, reply, Loop4, static_cast<quint16>(request.senderPort()));
    send(remote, reply + QByteArray(512, 'x'), Loop4, static_cast<quint16>(request.senderPort()));
    auto wrongNonce = reply;
    wrongNonce[18] = static_cast<char>(wrongNonce.at(18) ^ 1);
    send(remote, wrongNonce, Loop4, static_cast<quint16>(request.senderPort()));
    send(remote, encodeReply(request.data(), Local), Loop4, static_cast<quint16>(request.senderPort()));
    CHECK(!waitFor([&] { return results.contains(directed); }, 75));

    send(remote, reply, Loop4, static_cast<quint16>(request.senderPort()));
    CHECK(waitFor([&] { return results.contains(directed); }));
    CHECK(results.value(directed).status == ScanStatus::Complete);
    CHECK(results.value(directed).replies.size() == 1);
    CHECK(results.value(directed).replies.first().announcement.id == Remote.id);
    CHECK(!remote.hasPendingDatagrams()); // Only one request for the duplicate IPv4 target.

    // One loopback address responds twice while another stays silent.
    QElapsedTimer elapsed;
    elapsed.start();
    const auto partial = transport.probePeers({Loop4, OtherLoop4});
    const auto partialRequest = receive(remote);
    CHECK(partialRequest.data().mid(18, NonceSize) != request.data().mid(18, NonceSize));
    send(remote, encodeReply(partialRequest.data(), Remote), Loop4, static_cast<quint16>(partialRequest.senderPort()));
    auto newer = Remote;
    newer.name = "Latest";
    send(remote, encodeReply(partialRequest.data(), newer), Loop4, static_cast<quint16>(partialRequest.senderPort()));
    CHECK(waitFor([&] { return results.contains(partial); }));
    CHECK(elapsed.elapsed() >= ProbeTimeoutMs - 30);
    CHECK(results.value(partial).status == ScanStatus::Complete);
    CHECK(results.value(partial).replies.size() == 1);
    CHECK(results.value(partial).replies.first().announcement.name == "Latest");
}

void lifecycle()
{
    QUdpSocket remote;
    bind4(remote, Port);
    Results results;
    UdpPresenceTransport transport(Local, Loopback4);
    collect(transport, results);
    auto status = systemStatus();
    transport.updateAccess({}, &status, 1);

    const auto stale = transport.probePeers({Loop4});
    receive(remote);
    status.peers[0].nodeId = "other-node-at-same-address";
    transport.updateAccess({}, &status, 1);
    CHECK(waitFor([&] { return results.contains(stale); }));
    CHECK(results.value(stale).status == ScanStatus::AccessChanged && results.value(stale).replies.isEmpty());

    const auto cancelled = transport.probePeers({Loop4});
    transport.cancelScan(cancelled);
    CHECK(waitFor([&] { return results.contains(cancelled); }));
    CHECK(results.value(cancelled).status == ScanStatus::Cancelled);
    CHECK(!remote.hasPendingDatagrams());

    status.state = BackendState::Stopped;
    transport.updateAccess({}, &status, 2);
    const auto blocked = transport.probePeers({Loop4});
    CHECK(waitFor([&] { return results.contains(blocked); }));
    CHECK(results.value(blocked).status == ScanStatus::Complete && results.value(blocked).replies.isEmpty());
    CHECK(!remote.hasPendingDatagrams());
}

void dualStack()
{
    UdpPresenceTransport responder(Remote, {Loop4, Loop6});
    const auto bound = responder.startResponding();
    CHECK(bound.ipv4 && bound.ipv6);

    Results results;
    UdpPresenceTransport transport(Local, {Loop4, Loop6});
    collect(transport, results);
    const auto status = systemStatus(Loop4, Loop6);
    responder.updateAccess({}, &status, 1);
    transport.updateAccess({}, &status, 1);
    const auto mixed = transport.probePeers({Loop6, Loop4});
    CHECK(waitFor([&] { return results.contains(mixed); }));
    CHECK(results.value(mixed).status == ScanStatus::Complete);
    const auto replies = results.value(mixed).replies;
    CHECK(replies.size() == 2);
    CHECK(replies[0].address == Loop4 && replies[1].address == Loop6);
    CHECK(replies[0].announcement.id == Remote.id && replies[1].announcement.id == Remote.id);
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QHash<QString, void (*)()> fixtures{
        {"subnet", subnet}, {"responder", responder}, {"probe", probe}, {"lifecycle", lifecycle}, {"dual-stack", dualStack}
    };
    try {
        CHECK(app.arguments().size() == 2);
        const auto name = app.arguments().at(1);
        if (name == "dual-stack") {
            QUdpSocket ipv6Check;
            ipv6Check.setProxy(QNetworkProxy::NoProxy);
            if (!ipv6Check.bind(Loop6, 0))
                return 77;
        }
        const auto run = fixtures.value(name);
        CHECK(run);
        run();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "UDP presence fixture failed: %s\n", error.what());
        return 1;
    }
}
