// SPDX-License-Identifier: GPL-3.0-only
#include "PairingClient.h"
#include "ClientGrantCache.h"
#include "TailnetExchange.h"
#include "../PairingWire.h"
#include "../../adaptors/tailscale/EmbeddedStatusClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkProxy>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace Aurora::Discovery;
using namespace Aurora::Pairing;
using namespace Aurora::Pairing::Client;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

const QHostAddress Loop(QHostAddress::LocalHost);
const QHostAddress Tail(QStringLiteral("100.64.4.8"));
const QString HostId = QStringLiteral("00112233-4455-6677-8899-aabbccddeeff");
const QByteArray Key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
const QByteArray Challenge(BridgeKeySize, 'q');
const QByteArray OfferBytes = QByteArray(1, '\1') + QByteArray(32, 'a') + QByteArrayLiteral("0234");

template<typename Predicate>
bool waitFor(Predicate ready, int milliseconds = 2500)
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

struct Bridge
{
    Bridge()
    {
        server.setProxy(QNetworkProxy::NoProxy);
        CHECK(server.listen(Loop, 0));
        client.setProxy(QNetworkProxy::NoProxy);
        client.connectToHost(Loop, server.serverPort());
        CHECK(waitFor([this] {
            return server.hasPendingConnections() && client.state() == QAbstractSocket::ConnectedState;
        }));
        peer = server.nextPendingConnection();
        CHECK(peer);
    }

    void send(const QByteArray &bytes)
    {
        CHECK(peer->write(bytes) == bytes.size());
        peer->flush();
    }

    void handshake(RequestKind kind = RequestKind::Pair)
    {
        send(Challenge.left(11));
        CHECK(!waitFor([this] { return peer->bytesAvailable() > 0; }, 35));
        send(Challenge.mid(11));
        const auto expected = dialProof(Key, Challenge) + requestFrame(kind);
        CHECK(waitFor([&] { return peer->bytesAvailable() >= expected.size(); }));
        CHECK(peer->readAll() == expected);
    }

    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *peer = nullptr;
};

class Stdio final : public QIODevice
{
public:
    Stdio() { open(ReadWrite); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return in_.size() + QIODevice::bytesAvailable(); }
    void receive(const QByteArray &bytes)
    {
        in_ += bytes;
        emit readyRead();
    }
    const QByteArray &sent() const { return out_; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const auto count = std::min(maxSize, static_cast<qint64>(in_.size()));
        std::memcpy(data, in_.constData(), static_cast<size_t>(count));
        in_.remove(0, static_cast<int>(count));
        return count;
    }

    qint64 writeData(const char *data, qint64 maxSize) override
    {
        const auto count = std::min<qint64>(3, maxSize);
        out_.append(data, static_cast<int>(count));
        return count;
    }

private:
    QByteArray in_;
    QByteArray out_;
};

struct Fixture
{
    Fixture(const QString &runtime, const QString &certificate, bool embedded = true, Timeouts timeouts = {})
        : directory(QDir::current().filePath("pairing-client-XXXXXX")), coordinator(nullptr, timeouts)
    {
        CHECK(directory.isValid());
        const QDir profile(directory.path());
        request.moonlightProgram = profile.filePath(QFileInfo(runtime).fileName());
        CHECK(QFile::copy(runtime, request.moonlightProgram));
        QFile portable(profile.filePath("portable.dat"));
        CHECK(portable.open(QIODevice::WriteOnly));
        portable.close();

        QFile cert(certificate);
        CHECK(cert.open(QIODevice::ReadOnly));
        const QSslCertificate publicCertificate(cert.readAll());
        CHECK(!publicCertificate.isNull());
        expectedPin = publicCertificate.digest(QCryptographicHash::Sha256);

        network = {QHostAddress("100.64.4.1"), 42, "self", {}};
        Device peer;
        peer.id = "remote-peer";
        peer.address = Tail;
        network.devices.append(peer);
        coordinator.setAuthorization(17, true);

        gameStream.setProxy(QNetworkProxy::NoProxy);
        for (int tries = 0; tries < 5; ++tries) {
            CHECK(gameStream.listen(Loop, 0));
            if (gameStream.serverPort() <= 65535 - 21)
                break;
            gameStream.close();
        }
        CHECK(gameStream.isListening());
        request.epoch = 17;
        request.peerId = peer.id;
        request.verifiedHost = {{Tail, BundledSunshinePort}, HostId, "Studio", embedded};
        request.leasedRoute = embedded ? Aurora::Stream::Route{Loop, gameStream.serverPort(), HostId, true}
                                       : Aurora::Stream::Route{Tail, BundledSunshinePort, HostId, false};
        request.leaseLifetime = &gameStream;
        request.environment.insert("PAIR_FIXTURE_HOST", HostId);
        request.environment.insert("PAIR_FIXTURE_ENDPOINT", embedded
            ? QStringLiteral("127.0.0.1:%1").arg(gameStream.serverPort())
            : QStringLiteral("100.64.4.8:48989"));
        if (embedded) {
            request.environment.insert("PAIR_FIXTURE_ROUTE_BASE", QString::number(gameStream.serverPort()));
        } else {
            request.environment.insert("PAIR_FIXTURE_DIRECT", "1");
            request.environment.insert("AURORA_GAMESTREAM_RELAY_BASE", "12345");
            request.environment.insert("AURORA_GAMESTREAM_HOST_UUID", "ambient-host");
        }
        request.environment.insert("PAIR_FIXTURE_CERT", certificate);
        QObject::connect(&coordinator, &Coordinator::finished, &coordinator,
                         [this](quint64 id, const Result &result) { results.insert(id, result); });
    }

    QByteArray trace() const
    {
        QFile file(QDir(directory.path()).filePath("trace"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }

    Result result(quint64 requestId)
    {
        CHECK(requestId && waitFor([&] { return results.contains(requestId); }));
        return results.take(requestId);
    }

    quint64 start(Bridge &bridge)
    {
        const auto started = coordinator.startLoopback(network, request, &bridge.client, bridge.server.serverPort(), Key);
        CHECK(started.error == StartError::None && started.request);
        return started.request;
    }

    quint64 start(Stdio &stdio)
    {
        const auto started = coordinator.startStdio(network, request, &stdio, &stdio);
        CHECK(started.error == StartError::None && started.request);
        CHECK(waitFor([&] { return stdio.sent() == requestFrame(); }));
        return started.request;
    }

    const QTemporaryDir directory;
    QTcpServer gameStream;
    NetworkSnapshot network;
    Request request;
    QByteArray expectedPin;
    QHash<quint64, Result> results;
    Coordinator coordinator;
};

void embedded(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate);
    Bridge bridge;
    const auto id = fixture.start(bridge);
    bridge.handshake();
    bridge.send(OfferBytes.left(8));
    CHECK(!waitFor([&] { return !fixture.trace().isEmpty(); }, 35));
    bridge.send(OfferBytes.mid(8));
    const auto result = fixture.result(id);
    CHECK(result.status == Status::Paired && result.pinnedSha256 == fixture.expectedPin);
    CHECK(fixture.trace() == "pair\nlist\n");
    CHECK(!fixture.coordinator.busy() && bridge.client.state() == QAbstractSocket::UnconnectedState);
    CHECK(!fixture.gameStream.hasPendingConnections()); // The fake worker never dials.
}

void hostIdentity(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate);
    Bridge bridge;
    fixture.request.kind = RequestKind::HostIdentity;
    fixture.request.leasedRoute = {};
    fixture.request.moonlightProgram.clear();
    const auto id = fixture.start(bridge);
    bridge.handshake(RequestKind::HostIdentity);
    const auto reply = hostIdentityFrame(fixture.expectedPin);
    bridge.send(reply.left(1));
    QCoreApplication::processEvents();
    CHECK(fixture.coordinator.busy());
    bridge.send(reply.mid(1));
    const auto result = fixture.result(id);
    CHECK(result.status == Status::HostIdentityVerified && result.pinnedSha256 == fixture.expectedPin);
    CHECK(fixture.trace().isEmpty() && !fixture.gameStream.hasPendingConnections());

    Bridge routesBridge;
    const QHostAddress global(QStringLiteral("2001:db8::34"));
    fixture.request.kind = RequestKind::HostRoutes;
    const auto routesId = fixture.start(routesBridge);
    routesBridge.handshake(RequestKind::HostRoutes);
    const auto routesReply = hostRoutesFrame(fixture.expectedPin, {global});
    routesBridge.send(routesReply.left(HostIdentityFrameSize));
    QCoreApplication::processEvents();
    CHECK(fixture.coordinator.busy());
    routesBridge.send(routesReply.mid(HostIdentityFrameSize));
    const auto routesResult = fixture.result(routesId);
    CHECK(routesResult.status == Status::HostIdentityVerified && routesResult.pinnedSha256 == fixture.expectedPin);
    CHECK(routesResult.publicIpv6 == QList<QHostAddress>{global} && fixture.trace().isEmpty());

    Fixture direct(runtime, certificate, false);
    Stdio pipe;
    direct.request.kind = RequestKind::HostIdentity;
    const auto started = direct.coordinator.startStdio(direct.network, direct.request, &pipe, &pipe);
    CHECK(started.error == StartError::None && started.request);
    CHECK(waitFor([&] { return pipe.sent() == requestFrame(RequestKind::HostIdentity); }));
    pipe.receive(OfferBytes);
    CHECK(direct.result(started.request).status == Status::InvalidOffer && direct.trace().isEmpty());
}

void clientGrant(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate);
    fixture.request.kind = RequestKind::ClientGrant;
    Bridge bridge;
    const auto attempt = fixture.start(bridge);
    bridge.handshake(RequestKind::ClientGrant);
    const QHostAddress global(QStringLiteral("2001:db8::56"));
    const auto instance = clientGrantInstance(QStringLiteral("host-launch"));
    const auto challenge = grantChallenge(Challenge, fixture.expectedPin, 42, QStringLiteral("tailnet"),
        QStringLiteral("host"), QStringLiteral("client"), {global}, instance);
    bridge.send(challenge.left(12));
    CHECK(!waitFor([&] { return !fixture.trace().isEmpty(); }, 35));
    bridge.send(challenge.mid(12));
    const auto proof = grantProofFrame({QByteArrayLiteral("cert"), QByteArrayLiteral("sig")});
    CHECK(waitFor([&] { return bridge.peer->bytesAvailable() >= proof.size(); }));
    CHECK(bridge.peer->readAll() == proof);
    bridge.send(grantReplyFrame(GrantReply::Accepted));
    const auto granted = fixture.result(attempt);
    CHECK(granted.status == Status::ClientGranted && granted.pinnedSha256 == fixture.expectedPin);
    CHECK(granted.publicIpv6 == QList<QHostAddress>{global} && fixture.trace() == "proof\n");
    CHECK(granted.grantInstance == instance);
    CHECK(granted.clientCertificateSha256 == QCryptographicHash::hash("cert", QCryptographicHash::Sha256));

    fixture.request.leasedRoute.certificateSha256 = QByteArray(32, 'w');
    Bridge conflicting;
    const auto wrongPin = fixture.start(conflicting);
    conflicting.handshake(RequestKind::ClientGrant);
    conflicting.send(challenge);
    CHECK(fixture.result(wrongPin).status == Status::TrustFailed && fixture.trace() == "proof\n");

    Bridge premature;
    const auto earlyAck = fixture.start(premature);
    premature.handshake(RequestKind::ClientGrant);
    premature.send(grantReplyFrame(GrantReply::Accepted));
    CHECK(fixture.result(earlyAck).status == Status::InvalidOffer);

    Bridge legacy;
    const auto unsupported = fixture.start(legacy);
    legacy.handshake(RequestKind::ClientGrant);
    legacy.send(grantReplyFrame(GrantReply::Unsupported));
    CHECK(fixture.result(unsupported).status == Status::GrantUnsupported);
}

void cachedClientGrant(const QString &, const QString &)
{
    const auto peer = QStringLiteral("remote-peer");
    const QByteArray hostPin(32, 'h');
    const QByteArray clientPin(32, 'c');
    const auto instance = clientGrantInstance(QStringLiteral("same-host-launch"));
    ClientGrantCache cache;
    cache.noteSupport(peer, hostPin);
    CHECK(cache.knowsPeer(peer) && cache.hostFingerprint(peer) == hostPin && !cache.hasRecentGrant(peer));
    CHECK(!cache.reuse(peer, hostPin, instance, clientPin));

    const QDeadlineTimer deadline(5000);
    cache.remember(peer, hostPin, instance, clientPin, deadline);
    cache.noteSupport(peer, hostPin);
    CHECK(cache.hasRecentGrant(peer) && cache.reuse(peer, hostPin, instance, clientPin) == deadline);
    CHECK(!cache.reuse(peer, hostPin, clientGrantInstance(QStringLiteral("host-restarted")), clientPin));
    CHECK(!cache.reuse(peer, hostPin, instance, {}));
    CHECK(!cache.reuse(peer, hostPin, instance, QByteArray(32, 'd')));
    CHECK(!cache.reuse(peer, QByteArray(32, 'j'), instance, clientPin));

    cache.forgetGrant(peer);
    CHECK(cache.knowsPeer(peer) && !cache.hasRecentGrant(peer));
    cache.remember(peer, hostPin, instance, clientPin, QDeadlineTimer(0));
    CHECK(!cache.hasRecentGrant(peer) && !cache.reuse(peer, hostPin, instance, clientPin));
    cache.clear();
    CHECK(!cache.knowsPeer(peer));
}

void directStdio(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate, false);
    Stdio pipe;
    auto wrong = fixture.request;
    wrong.leasedRoute.address = Loop;
    CHECK(fixture.coordinator.startStdio(fixture.network, wrong, &pipe, &pipe).error == StartError::InvalidRoute);
    wrong = fixture.request;
    ++wrong.leasedRoute.port;
    CHECK(fixture.coordinator.startStdio(fixture.network, wrong, &pipe, &pipe).error == StartError::InvalidRoute);
    wrong = fixture.request;
    wrong.leasedRoute.hostId = QStringLiteral("wrong-host");
    CHECK(fixture.coordinator.startStdio(fixture.network, wrong, &pipe, &pipe).error == StartError::InvalidRoute);
    CHECK(pipe.sent().isEmpty());
    const auto id = fixture.start(pipe);
    pipe.receive(OfferBytes.left(1));
    QCoreApplication::processEvents();
    CHECK(fixture.trace().isEmpty());
    pipe.receive(OfferBytes.mid(1));
    const auto result = fixture.result(id);
    CHECK(result.status == Status::Paired && result.pinnedSha256 == fixture.expectedPin);
    CHECK(pipe.isOpen() && fixture.trace() == "pair\nlist\n");
}

void rejected(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate);
    Bridge bridge;
    auto changed = fixture.request;
    changed.peerId = "wrong-peer";
    CHECK(fixture.coordinator.startLoopback(fixture.network, changed, &bridge.client, bridge.server.serverPort(), Key).error
          == StartError::Unauthorized);
    changed = fixture.request;
    changed.leasedRoute.hostId = "wrong-host";
    CHECK(fixture.coordinator.startLoopback(fixture.network, changed, &bridge.client, bridge.server.serverPort(), Key).error
          == StartError::InvalidRoute);
    changed = fixture.request;
    changed.leasedRoute.address = Tail;
    CHECK(fixture.coordinator.startLoopback(fixture.network, changed, &bridge.client, bridge.server.serverPort(), Key).error
          == StartError::InvalidRoute);
    CHECK(fixture.coordinator.startLoopback(fixture.network, fixture.request, &bridge.client, 0, Key).error
          == StartError::InvalidTransport);
    CHECK(fixture.coordinator.startLoopback(fixture.network, fixture.request, &bridge.client, bridge.server.serverPort(),
                                            Key.left(31)).error == StartError::InvalidTransport);
    CHECK(bridge.peer->bytesAvailable() == 0 && fixture.trace().isEmpty());

    auto id = fixture.start(bridge);
    bridge.handshake();
    bridge.send(QByteArray(1, '\0'));
    CHECK(fixture.result(id).status == Status::Rejected && fixture.trace().isEmpty());

    Bridge bad;
    id = fixture.start(bad);
    bad.handshake();
    auto corrupt = OfferBytes;
    corrupt[12] = 'G';
    bad.send(corrupt);
    CHECK(fixture.result(id).status == Status::InvalidOffer && fixture.trace().isEmpty());
}

void trust(const QString &runtime, const QString &certificate)
{
    for (const auto &mode : {"missing", "different", "bad-cert", "duplicate", "backup"}) {
        Fixture fixture(runtime, certificate, false);
        fixture.request.environment.insert("PAIR_FIXTURE_STORE", QString::fromLatin1(mode));
        Stdio pipe;
        const auto id = fixture.start(pipe);
        pipe.receive(OfferBytes);
        const auto result = fixture.result(id);
        if (QByteArray(mode) == "backup") {
            CHECK(result.status == Status::Paired && result.pinnedSha256 == fixture.expectedPin);
            CHECK(fixture.trace() == "pair\nlist\n");
        } else {
            CHECK(result.status == Status::TrustMissing && result.pinnedSha256.isEmpty());
            CHECK(fixture.trace() == "pair\n");
        }
    }

    Fixture oldRuntime(runtime, certificate, false);
    oldRuntime.request.environment.insert("PAIR_FIXTURE_BAD_RELAY", "1");
    Stdio oldPipe;
    const auto old = oldRuntime.start(oldPipe);
    oldPipe.receive(OfferBytes);
    CHECK(oldRuntime.result(old).status == Status::RuntimeFailed && oldRuntime.trace().isEmpty());

    Fixture pairFailed(runtime, certificate, false);
    pairFailed.request.environment.insert("PAIR_FIXTURE_EXIT", "2");
    Stdio pairPipe;
    auto id = pairFailed.start(pairPipe);
    pairPipe.receive(OfferBytes);
    CHECK(pairFailed.result(id).status == Status::RuntimeFailed && pairFailed.trace() == "pair\n");

    Fixture listFailed(runtime, certificate, false);
    listFailed.request.environment.insert("PAIR_FIXTURE_LIST_EXIT", "1");
    Stdio listPipe;
    id = listFailed.start(listPipe);
    listPipe.receive(OfferBytes);
    const auto result = listFailed.result(id);
    CHECK(result.status == Status::TrustFailed && result.pinnedSha256.isEmpty());
    CHECK(listFailed.trace() == "pair\nlist\n");
}

void deadlines(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate, true, {350, 240, 3500});
    Bridge challenge;
    auto id = fixture.start(challenge);
    challenge.send(Challenge.left(12));
    CHECK(fixture.result(id).status == Status::BridgeTimeout);
    CHECK(challenge.client.state() == QAbstractSocket::UnconnectedState);

    Bridge offer;
    id = fixture.start(offer);
    offer.handshake();
    offer.send(OfferBytes.left(9));
    CHECK(fixture.result(id).status == Status::OfferTimeout && fixture.trace().isEmpty());

    Fixture hanging(runtime, certificate, false, {3000, 10000, 1600});
    hanging.request.environment.insert("PAIR_FIXTURE_HOLD", "1");
    Stdio pipe;
    id = hanging.start(pipe);
    pipe.receive(OfferBytes);
    CHECK(waitFor([&] { return hanging.trace() == "pair\n"; }));
    CHECK(hanging.result(id).status == Status::Timeout && hanging.trace() == "pair\n");
    CHECK(!hanging.coordinator.busy());
}

void cancellation(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate);
    fixture.request.environment.insert("PAIR_FIXTURE_HOLD", "1");
    Bridge bridge;
    auto id = fixture.start(bridge);
    bridge.handshake();
    bridge.send(OfferBytes);
    CHECK(waitFor([&] { return fixture.trace() == "pair\n"; }));
    CHECK(bridge.client.state() == QAbstractSocket::ConnectedState);
    CHECK(fixture.coordinator.startLoopback(fixture.network, fixture.request, &bridge.client,
                                            bridge.server.serverPort(), Key).error == StartError::Busy);
    fixture.coordinator.setAuthorization(18, false);
    CHECK(fixture.result(id).status == Status::AuthorizationChanged);
    CHECK(fixture.trace() == "pair\n" && bridge.client.state() == QAbstractSocket::UnconnectedState);

    fixture.coordinator.setAuthorization(17, true);
    Bridge next;
    CHECK(fixture.coordinator.startLoopback(fixture.network, fixture.request, &next.client,
                                            next.server.serverPort(), Key).error == StartError::Unauthorized);
    fixture.coordinator.setAuthorization(18, true);
    fixture.request.epoch = 18;
    id = fixture.start(next);
    fixture.coordinator.cancel(id);
    CHECK(fixture.result(id).status == Status::Cancelled);
    CHECK(next.client.state() == QAbstractSocket::UnconnectedState);

    Bridge revoked;
    auto lease = std::make_unique<QObject>();
    fixture.request.leaseLifetime = lease.get();
    id = fixture.start(revoked);
    lease.reset();
    CHECK(fixture.result(id).status == Status::LeaseClosed);
    CHECK(revoked.client.state() == QAbstractSocket::UnconnectedState);
}

void exchangeCancellation(const QString &runtime, const QString &certificate)
{
    Fixture fixture(runtime, certificate);
    aurora::tailscale::EmbeddedStatusClient helper({}, fixture.directory.path());
    TailnetExchange exchange(&helper);
    QList<QPair<quint64, Status>> finished;
    QObject::connect(&exchange, &TailnetExchange::finished, &exchange,
        [&](quint64 id, const Result &result) { finished.append({id, result.status}); });

    const auto first = exchange.start(fixture.request, fixture.network, {}, true, [] { return true; });
    CHECK(first);
    exchange.cancel(first);
    CHECK(finished.isEmpty());
    CHECK(waitFor([&] { return finished.size() == 1; }));
    CHECK(finished.first() == qMakePair(first, Status::Cancelled));

    bool current = true;
    const auto next = exchange.start(fixture.request, fixture.network, {}, true, [&] { return current; });
    CHECK(next && next != first);
    current = false;
    CHECK(waitFor([&] { return finished.size() == 2; }));
    CHECK(finished.last() == qMakePair(next, Status::AuthorizationChanged));
    CHECK(fixture.trace().isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QHash<QString, void (*)(const QString &, const QString &)> fixtures{
        {"embedded", embedded},
        {"host-identity", hostIdentity}, {"client-grant", clientGrant}, {"cached-client-grant", cachedClientGrant},
        {"direct-stdio", directStdio}, {"rejected", rejected}, {"trust", trust},
        {"deadlines", deadlines}, {"cancellation", cancellation}, {"exchange-cancellation", exchangeCancellation}
    };
    try {
        CHECK(app.arguments().size() == 4);
        const auto run = fixtures.value(app.arguments().at(1));
        CHECK(run);
        run(app.arguments().at(2), app.arguments().at(3));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Pairing client fixture failed: %s\n", error.what());
        return 1;
    }
}
