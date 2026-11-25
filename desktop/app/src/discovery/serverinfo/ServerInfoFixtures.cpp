// SPDX-License-Identifier: GPL-3.0-only
#include "ServerInfoClient.h"
#include "HttpBody.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace Aurora::Discovery;
using namespace Aurora::Discovery::ServerInfo;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

const QHostAddress Loop(QHostAddress::LocalHost);
const QHostAddress Tail(QStringLiteral("100.64.4.8"));
const QUuid PeerGuid(QStringLiteral("{1eaac04a-091f-4a77-a22d-09dce3c597ee}"));

QByteArray xml(const QByteArray &id = "Host-A", const QByteArray &extensions = {})
{
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><root status_code=\"200\">"
        "<PairStatus>0</PairStatus><appversion>7.1.0.0</appversion><hostname>Studio &amp; Games</hostname>"
        "<uniqueid>" + id + "</uniqueid>" + extensions + "</root>";
}

QByteArray http(const QByteArray &body, const QByteArray &status = "200 OK", const QByteArray &headers = {})
{
    return "HTTP/1.1 " + status + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) +
        "\r\n" + headers + "\r\n" + body;
}

template<typename Predicate>
bool waitFor(Predicate ready, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (!ready())
            QThread::msleep(1);
    }
    return ready();
}

struct LoopbackServer
{
    struct Request
    {
        QPointer<QTcpSocket> socket;
        QByteArray head;
    };

    explicit LoopbackServer(const QHostAddress &loop = Loop)
    {
        CHECK(loop.isLoopback());
        server.setProxy(QNetworkProxy::NoProxy);
        CHECK(server.listen(loop, 0));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                requests.append({socket, bytes.left(end + 4)});
                bytes.remove(0, end + 4);
            });
        });
    }

    quint16 port() const { return server.serverPort(); }

    void send(int request, const QByteArray &bytes, bool close = true)
    {
        CHECK(request < requests.size() && requests.at(request).socket);
        auto *socket = requests.at(request).socket.data();
        CHECK(socket->write(bytes) == bytes.size());
        if (close)
            socket->disconnectFromHost();
    }

    QList<Request> requests;
    QTcpServer server;
};

struct Results
{
    QHash<quint64, BatchResult> batches;
    QHash<quint64, CheckResult> checks;

    explicit Results(Client &client)
    {
        QObject::connect(&client, &Client::batchFinished, &client,
                         [this](quint64 request, const BatchResult &result) { batches.insert(request, result); });
        QObject::connect(&client, &Client::checkFinished, &client,
                         [this](quint64 request, const CheckResult &result) { checks.insert(request, result); });
    }

    BatchResult batch(quint64 request)
    {
        CHECK(request && waitFor([&] { return batches.contains(request); }));
        return batches.take(request);
    }

    CheckResult check(quint64 request)
    {
        CHECK(request && waitFor([&] { return checks.contains(request); }));
        return checks.take(request);
    }
};

NetworkSnapshot account()
{
    return {QHostAddress("100.64.4.1"), 42, "current", {}};
}

PresentDevice present(const QHostAddress &address, bool hosting = true)
{
    return {PeerGuid, address, "Studio", "windows", hosting};
}

void allowLoopback(Client &client)
{
    client.updateDiscovery(1, account(), {present(Loop)}, {}, false);
}

void xmlFixtures()
{
    const Endpoint endpoint{QHostAddress("fd7a:115c:a1e0::8"), BundledSunshinePort};
    auto parsed = parse(xml(" Host-A ", "<AuroraEncoderStatus>probing</AuroraEncoderStatus>"
        "<AuroraPower>1</AuroraPower><AuroraSessionPrivacy>1</AuroraSessionPrivacy>"
        "<ExternalPort>47989</ExternalPort><LocalIP>192.0.2.99</LocalIP>"), endpoint, true);
    CHECK(parsed && parsed->id == "Host-A" && parsed->name == "Studio & Games");
    CHECK(parsed->endpoint.address == endpoint.address && parsed->endpoint.port == BundledSunshinePort);
    CHECK(parsed->viaEmbeddedTailnet && parsed->encoderProbePending && parsed->supportsPower && parsed->supportsSessionPrivacy);

    for (const auto &encoder : {"failed", "ready", "", " probing "}) {
        parsed = parse(xml("Host-A", "<AuroraEncoderStatus>" + QByteArray(encoder) + "</AuroraEncoderStatus>"), endpoint);
        CHECK(parsed && !parsed->encoderProbePending);
    }
    parsed = parse(xml("first", "<uniqueid>second</uniqueid>"), endpoint);
    CHECK(parsed && parsed->id == "first");
    CHECK(parse(xml(QByteArray(128, 'x')), endpoint));
    CHECK(!parse(xml(QByteArray(129, 'x')), endpoint));
    CHECK(!parse(xml(" \r\t "), endpoint));
    CHECK(!parse(xml() + "<root/>", endpoint));
    CHECK(!parse(xml().chopped(3), endpoint));
    CHECK(!parse("<root status_code='200'><hostname>Printer</hostname></root>", endpoint));

    auto body = xml();
    body.replace("<root ", "<root xmlns='urn:wrong' ");
    CHECK(!parse(body, endpoint));
    body = xml();
    body.replace("status_code=\"200\"", "status_code=\"201\"");
    CHECK(!parse(body, endpoint));
    body = xml();
    body.replace("<PairStatus>0</PairStatus>", "");
    CHECK(!parse(body, endpoint));
    body = xml();
    body.replace("<appversion>7.1.0.0</appversion>", "");
    CHECK(!parse(body, endpoint));
    body = xml();
    body.insert(body.indexOf("<root"), "<!DOCTYPE root [<!ENTITY x SYSTEM 'file:///unused'>]>");
    CHECK(!parse(body, endpoint));
    body = xml();
    body.append(QByteArray(MaxDocumentSize - body.size(), ' '));
    CHECK(parse(body, endpoint));
    CHECK(!parse(body + ' ', endpoint));
}

void framing()
{
    using Body = Aurora::Discovery::ServerInfo::Detail::HttpBody;
    const auto content = xml();
    const auto encoded = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
        QByteArray::number(content.size(), 16) + ";fixture=yes\r\n" + content +
        "\r\n0\r\nX-Trailer: fixture\r\n\r\n";
    Body chunked;
    auto state = Body::Result::Pending;
    for (char byte : encoded) {
        CHECK(state == Body::Result::Pending);
        state = chunked.append(QByteArray(1, byte));
    }
    CHECK(state == Body::Result::Complete && chunked.body() == content);

    Body close;
    CHECK(close.append("HTTP/1.0 200 OK\r\n\r\n" + content) == Body::Result::Pending);
    CHECK(close.end() == Body::Result::Complete && close.body() == content);
    Body truncated;
    CHECK(truncated.append(http(content).chopped(1)) == Body::Result::Pending);
    CHECK(truncated.end() == Body::Result::Invalid);
    Body oversizedHeaders;
    CHECK(oversizedHeaders.append("HTTP/1.1 200 OK\r\nX: " + QByteArray(16384, 'a')) == Body::Result::Invalid);
    Body ambiguous;
    CHECK(ambiguous.append("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n")
          == Body::Result::Invalid);
    Body brokenChunk;
    CHECK(brokenChunk.append("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabzz")
          == Body::Result::Invalid);
}

void httpFixtures()
{
    LoopbackServer server;
    LoopbackServer redirect;
    struct ScopedProxy
    {
        QNetworkProxy previous = QNetworkProxy::applicationProxy();
        ~ScopedProxy() { QNetworkProxy::setApplicationProxy(previous); }
    } proxyScope;
    LoopbackServer proxy;
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, Loop.toString(), proxy.port(),
                                                    "ambient-user", "ambient-password"));
    Client client(nullptr, server.port());
    Results results(client);
    allowLoopback(client);

    auto probe = client.probeBatch({{Loop, {}}, {Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 1; }));
    const auto request = server.requests.at(0).head.toLower();
    CHECK(request.startsWith("get /serverinfo http/1.1\r\n"));
    CHECK(request.contains("\r\nhost: 127.0.0.1:" + QByteArray::number(server.port()) + "\r\n"));
    CHECK(!request.contains("cookie:") && !request.contains("authorization:"));
    server.send(0, http(xml(), "200 OK", "Set-Cookie: fixture=blocked\r\n"));
    auto batch = results.batch(probe);
    CHECK(batch.status == Status::Complete && batch.hosts.size() == 1);
    CHECK(batch.hosts.first().endpoint.port == server.port());

    probe = client.probeBatch({{Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 2; }));
    CHECK(!server.requests.at(1).head.toLower().contains("cookie:"));
    server.send(1, http(xml(), "302 Found", "Location: http://127.0.0.1:" + QByteArray::number(redirect.port()) + "/serverinfo\r\n"));
    CHECK(results.batch(probe).hosts.isEmpty());
    CHECK(redirect.requests.isEmpty());

    probe = client.probeBatch({{Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 3; }));
    server.send(2, http({}, "401 Unauthorized", "WWW-Authenticate: Negotiate\r\nWWW-Authenticate: NTLM\r\n"));
    CHECK(results.batch(probe).hosts.isEmpty());
    CHECK(server.requests.size() == 3);

    probe = client.probeBatch({{Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 4; }));
    server.send(3, "HTTP/1.1 200 OK\r\nContent-Length: 32769\r\n\r\n", false);
    CHECK(results.batch(probe).hosts.isEmpty());

    probe = client.probeBatch({{Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 5; }));
    server.send(4, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + QByteArray(MaxDocumentSize + 1, 'x'));
    CHECK(results.batch(probe).hosts.isEmpty());

    probe = client.probeBatch({{Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 6; }));
    const auto chunk = xml();
    server.send(5, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
        QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n0\r\n\r\n");
    CHECK(results.batch(probe).hosts.size() == 1);
    CHECK(proxy.requests.isEmpty());
}

void ipv6()
{
    const QHostAddress loop6(QHostAddress::LocalHostIPv6);
    LoopbackServer server(loop6);
    Client client(nullptr, server.port());
    Results results(client);
    client.updateDiscovery(1, account(), {present(loop6)}, {}, false);
    const auto probe = client.probeBatch({{loop6, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 1; }));
    CHECK(server.requests.first().head.toLower().contains("\r\nhost: [::1]:" +
        QByteArray::number(server.port()) + "\r\n"));
    server.send(0, http(xml()));
    const auto result = results.batch(probe);
    CHECK(result.hosts.size() == 1 && result.hosts.first().endpoint.address == loop6);
}

void lifecycle()
{
    LoopbackServer server;
    Client client(nullptr, server.port());
    Results results(client);
    const StreamHost route{{Loop, server.port()}, "host-a", "Studio"};
    auto batch = client.probeBatch({{Loop, {}}});
    CHECK(results.batch(batch).hosts.isEmpty() && server.requests.isEmpty());
    CHECK(results.check(client.revalidate(route)).status == Status::RejectedTarget);

    client.updateDiscovery(1, account(), {present(Loop, false)}, {}, false);
    CHECK(results.check(client.revalidate(route)).status == Status::RejectedTarget);
    allowLoopback(client);
    const auto badPort = StreamHost{{Loop, 1}, "host-a", "Studio"};
    CHECK(results.check(client.revalidate(badPort)).status == Status::RejectedTarget);
    CHECK(server.requests.isEmpty());

    batch = client.probeBatch({{Loop, {}}}, 200);
    CHECK(waitFor([&] { return server.requests.size() == 1; }));
    const auto deadline = results.batch(batch);
    CHECK(deadline.status == Status::Timeout && deadline.hosts.isEmpty());

    batch = client.probeBatch({{Loop, {}}});
    CHECK(waitFor([&] { return server.requests.size() == 2; }));
    client.cancel(batch);
    CHECK(results.batch(batch).status == Status::Cancelled);

    const auto stale = client.revalidate(route);
    CHECK(waitFor([&] { return server.requests.size() == 3; }));
    client.updateDiscovery(2, {}, {}, {}, false);
    CHECK(results.check(stale).status == Status::AccessChanged);
    allowLoopback(client); // Generation 1 is stale after sign-out.
    CHECK(results.check(client.revalidate(route)).status == Status::RejectedTarget);
    CHECK(server.requests.size() == 3);
}

void revalidation()
{
    LoopbackServer server;
    Client client(nullptr, server.port());
    Results results(client);
    allowLoopback(client);
    const StreamHost expected{{Loop, server.port()}, "host-a", "Studio"};
    const QByteArray probing = "<AuroraEncoderStatus>probing</AuroraEncoderStatus>";

    auto check = client.revalidate(expected);
    CHECK(waitFor([&] { return server.requests.size() == 1; }));
    server.send(0, http(xml("HOST-A", probing)));
    auto result = results.check(check);
    CHECK(result.status == Status::Complete && result.current && result.current->encoderProbePending);

    check = client.waitForEncoder(expected, {}, 1900);
    CHECK(waitFor([&] { return server.requests.size() == 2; }));
    server.send(1, http({},"503 Unavailable"));
    CHECK(waitFor([&] { return server.requests.size() == 3; }));
    server.send(2, http(xml("Host-A", probing)));
    CHECK(waitFor([&] { return server.requests.size() == 4; }));
    server.send(3, http(xml("host-a", "<AuroraEncoderStatus>failed</AuroraEncoderStatus>")));
    result = results.check(check);
    CHECK(result.status == Status::Complete && result.current && !result.current->encoderProbePending);

    check = client.waitForEncoder(expected);
    CHECK(waitFor([&] { return server.requests.size() == 5; }));
    server.send(4, http(xml("another-host")));
    result = results.check(check);
    CHECK(result.status == Status::IdentityChanged && !result.current);

    check = client.revalidate(expected);
    CHECK(waitFor([&] { return server.requests.size() == 6; }));
    server.send(5, http(xml("another-host")));
    CHECK(results.check(check).status == Status::IdentityChanged);

    check = client.revalidate(expected, {}, RouteRequirement::Power);
    CHECK(waitFor([&] { return server.requests.size() == 7; }));
    server.send(6, http(xml()));
    CHECK(results.check(check).status == Status::PowerUnavailable);

    check = client.revalidate(expected, {}, RouteRequirement::Power);
    CHECK(waitFor([&] { return server.requests.size() == 8; }));
    server.send(7, http(xml("Host-A", "<AuroraPower>1</AuroraPower>")));
    result = results.check(check);
    CHECK(result.status == Status::Complete && result.current && result.current->supportsPower);

    check = client.waitForEncoder(expected, {}, 0);
    CHECK(results.check(check).status == Status::Timeout && server.requests.size() == 8);
}

void embedded()
{
    std::vector<std::unique_ptr<LoopbackServer>> relays;
    QList<Candidate> candidates;
    QList<PresentDevice> devices;
    Client client;
    Results results(client);
    auto network = account();
    for (int i = 0; i < BatchConcurrency + 1; ++i) {
        relays.push_back(std::make_unique<LoopbackServer>());
        const QHostAddress address(QStringLiteral("100.64.4.%1").arg(i + 8));
        candidates.append({address, Endpoint{Loop, relays.back()->port()}});
        devices.append(present(address));
        Device peer;
        peer.id = QString::number(i);
        peer.address = address;
        network.devices.append(peer);
    }
    client.updateDiscovery(1, network, {}, devices, true);
    StreamHost expected{{Tail, BundledSunshinePort}, "host-a", "Studio"};
    expected.viaEmbeddedTailnet = true;
    auto invalid = expected;
    invalid.endpoint.address = Loop;
    CHECK(results.check(client.revalidate(invalid)).status == Status::RejectedTarget);
    CHECK(results.check(client.revalidate(expected, Endpoint{Loop, 0})).status == Status::RejectedTarget);

    const auto batch = client.probeBatch(candidates);
    CHECK(client.probeBatch(candidates) == 0);
    const auto initial = [&] {
        return std::all_of(relays.begin(), relays.end() - 1, [](const auto &relay) { return relay->requests.size() == 1; });
    };
    CHECK(waitFor(initial));
    CHECK(relays.back()->requests.isEmpty());
    CHECK(relays.front()->requests.first().head.toLower().contains("\r\nconnection: close\r\n"));
    relays.front()->send(0, http(xml()));
    CHECK(waitFor([&] { return relays.back()->requests.size() == 1; }));
    for (auto it = relays.begin() + 1; it != relays.end(); ++it)
        (*it)->send(0, http(xml()));
    const auto complete = results.batch(batch);
    CHECK(complete.status == Status::Complete && complete.hosts.size() == BatchConcurrency + 1);
    for (const auto &host : complete.hosts)
        CHECK(host.viaEmbeddedTailnet && !host.endpoint.address.isLoopback() && host.endpoint.port == BundledSunshinePort);

    const auto check = client.revalidate(expected, candidates.first().loopbackRelay);
    CHECK(waitFor([&] { return relays.front()->requests.size() == 2; }));
    relays.front()->send(1, http(xml()));
    const auto result = results.check(check);
    CHECK(result.status == Status::Complete && result.current && result.current->endpoint.address == Tail);
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QHash<QString, void (*)()> fixtures{
        {"xml", xmlFixtures}, {"framing", framing}, {"http", httpFixtures}, {"ipv6", ipv6}, {"lifecycle", lifecycle},
        {"revalidate", revalidation}, {"embedded", embedded}
    };
    try {
        CHECK(app.arguments().size() == 2);
        const auto name = app.arguments().at(1);
        if (name == "ipv6") {
            QTcpServer check;
            check.setProxy(QNetworkProxy::NoProxy);
            if (!check.listen(QHostAddress::LocalHostIPv6, 0))
                return 77;
        }
        const auto run = fixtures.value(name);
        CHECK(run);
        run();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Server-info fixture failed: %s\n", error.what());
        return 1;
    }
}
