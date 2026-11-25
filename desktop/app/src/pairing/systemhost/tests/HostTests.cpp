// SPDX-License-Identifier: GPL-3.0-only
#include <qt_windows.h>

#include "SystemPairingHost.h"
#include "PairingWire.h"
#include "WindowsTransport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

using namespace Aurora::Pairing;
using namespace aurora::tailscale::system;

#define CHECK(expression) \
    do { if (!(expression)) qFatal("Line %d: %s", __LINE__, #expression); } while (false)

namespace {

constexpr quint64 generation = 17;
constexpr qint64 ownerId = 9223372036854775806LL;
const auto owner = QByteArray::number(ownerId);
const auto peerIp = QHostAddress(QStringLiteral("100.80.0.2"));
const auto peerV6 = QHostAddress(QStringLiteral("fd7a:115c:a1e0::2"));
const SystemHostAccount account{{ownerId, QStringLiteral("self"), QStringLiteral("tailnet")},
    QHostAddress(QStringLiteral("100.80.0.1"))};

template<class Predicate>
bool waitFor(Predicate done, int milliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

QByteArray peer(const QByteArray &name = "peer", const QByteArray &user = owner, const QByteArray &extra = {})
{
    return "{\"ID\":\"" + name + "\",\"UserID\":" + user + ",\"HostName\":\"device\","
        "\"TailscaleIPs\":[\"100.80.0.2\",\"fd7a:115c:a1e0::2\"]" + extra + "}";
}

QByteArray status(const QByteArray &peers = "\"a\":" + peer(),
    const QByteArray &tailnet = "tailnet", const QByteArray &local = "100.80.0.1")
{
    return "{\"BackendState\":\"Running\",\"Self\":{\"ID\":\"self\",\"UserID\":" + owner +
        "},\"CurrentTailnet\":{\"StableID\":\"" + tailnet +
        "\"},\"TailscaleIPs\":[\"" + local + "\"],\"Peer\":{" + peers + "}}";
}

QByteArray hello(const QHostAddress &source = peerIp, bool v6 = false)
{
    auto header = QByteArray::fromHex("0d0a0d0a000d0a515549540a21");
    const int length = v6 ? 16 : 4;
    header += v6 ? '\x21' : '\x11';
    QByteArray body(2 * length + 4, '\0');
    if (v6) {
        const auto address = source.toIPv6Address();
        for (int index = 0; index < length; ++index)
            body[index] = char(address[index]);
    } else {
        qToBigEndian(source.toIPv4Address(), body.data());
    }
    qToBigEndian<quint16>(41321, body.data() + 2 * length);
    qToBigEndian<quint16>(Port, body.data() + 2 * length + 2);
    body += QByteArray::fromHex("ea00020102");
    char size[2];
    qToBigEndian<quint16>(quint16(body.size()), size);
    header.append(size, 2);
    return header + body + requestFrame();
}

struct Test
{
    QTemporaryDir dir;
    Installation installation;
    bool allowOwner = true;
    quint32 pid = GetCurrentProcessId();
    quint64 hostId = 0;
    quint64 connection = 0;
    int listening = 0;
    int authorized = 0;
    int rechecked = 0;
    int closed = 0;
    int stopped = 0;
    quint64 stopGeneration = 0;
    SystemHostStop reason = SystemHostStop::Cancelled;
    QTcpSocket *channel = nullptr;
    SystemHostPeer lastPeer;
    SystemPairingHost host;

    explicit Test(const QString &fixture)
        : host([this] { return installation; }, [this](quint16 source, quint16 destination) {
            if (!allowOwner || !SystemHostDetail::tcpOwnedByProcess(GetCurrentProcessId(), source, destination))
                return std::optional<quint32>{};
            return std::optional<quint32>{pid};
        })
    {
        CHECK(dir.isValid());
        const auto path = file("fixture-cli.exe");
        CHECK(QFile::copy(fixture, path));
        installation = {path, ServiceState::Running};
        put("status.json", status());
        put("serve.json", "null\r\n");

        QObject::connect(&host, &SystemPairingHost::listening, &host, [&](quint64 id, quint64 current) {
            CHECK(id == hostId && current == generation);
            ++listening;
        });
        QObject::connect(&host, &SystemPairingHost::authenticated, &host,
            [&](quint64 id, quint64 current, quint64 session, const SystemHostPeer &peer, QTcpSocket *device) {
                CHECK(id == hostId && current == generation && device->bytesAvailable() == 0);
                connection = session;
                channel = device;
                lastPeer = peer;
                ++authorized;
            });
        QObject::connect(&host, &SystemPairingHost::revalidated, &host,
            [&](quint64 id, quint64 current, quint64 session) {
                CHECK(id == hostId && current == generation && session == connection);
                ++rechecked;
            });
        QObject::connect(&host, &SystemPairingHost::connectionClosed, &host, [&](quint64 id, quint64 session) {
            CHECK(id == hostId && session == connection);
            channel = nullptr;
            ++closed;
        });
        QObject::connect(&host, &SystemPairingHost::stopped, &host, [&](quint64 id, quint64 current, SystemHostStop why) {
            CHECK(id == hostId);
            ++stopped;
            stopGeneration = current;
            reason = why;
        });
        host.setAuthorization(generation, account);
    }

    QString file(const char *name) const { return dir.filePath(QString::fromLatin1(name)); }

    void put(const char *name, const QByteArray &bytes = "1")
    {
        QFile stream(file(name));
        CHECK(stream.open(QIODevice::WriteOnly | QIODevice::Truncate));
        CHECK(stream.write(bytes) == bytes.size());
    }

    quint16 port() const
    {
        QFile stream(file("relay-port"));
        CHECK(stream.open(QIODevice::ReadOnly));
        bool ok = false;
        const auto port = stream.readAll().toUShort(&ok);
        CHECK(ok && port);
        return port;
    }

    void start(bool ready = true)
    {
        const auto previous = ready ? listening : stopped;
        hostId = host.start(generation);
        CHECK(hostId && !host.start(generation));
        CHECK(waitFor([&] { return (ready ? listening : stopped) == previous + 1; }));
    }

    void dial(QTcpSocket &socket) const
    {
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.connectToHost(QHostAddress::LocalHost, port());
        CHECK(socket.waitForConnected(2000));
    }
};

bool gone(const QTcpSocket &socket) { return socket.state() == QAbstractSocket::UnconnectedState; }

void transport(const QString &fixture)
{
    Test test(fixture);
    test.start();
    QTcpSocket client;
    test.dial(client);
    CHECK(SystemHostDetail::tcpOwnedByProcess(GetCurrentProcessId(), client.localPort(), test.port()));
    CHECK(!SystemHostDetail::tcpOwnedByProcess(0, client.localPort(), test.port()));
    CHECK(!SystemHostDetail::tcpOwnedByProcess(GetCurrentProcessId(), client.localPort(), client.localPort()));
    const auto bytes = hello(QHostAddress(QStringLiteral("::ffff:100.80.0.2")), true);
    CHECK(client.write(bytes.left(12)) == 12);
    CHECK(waitFor([&] { return client.bytesToWrite() == 0; }));
    CHECK(client.write(bytes.mid(12, 20)) == 20);
    CHECK(waitFor([&] { return client.bytesToWrite() == 0; }));
    CHECK(test.authorized == 0);
    CHECK(client.write(bytes.mid(32)) == bytes.size() - 32);
    CHECK(waitFor([&] { return test.authorized == 1; }));
    CHECK(test.lastPeer.address == peerIp && test.lastPeer.nodeId == QStringLiteral("peer"));
    CHECK(test.channel->write("offer", 5) == 5);
    CHECK(waitFor([&] { return client.bytesAvailable() == 5; }));
    CHECK(client.readAll() == "offer");
    CHECK(test.host.revalidate(test.hostId, test.connection));
    CHECK(!test.host.revalidate(test.hostId, test.connection));
    CHECK(waitFor([&] { return test.rechecked == 1; }));

    QTcpSocket extra;
    test.dial(extra);
    CHECK(extra.write(hello(peerV6, true)) > 0);
    CHECK(waitFor([&] { return gone(extra); }));
    CHECK(extra.readAll() == QByteArray(1, '\0') && test.authorized == 1);
    test.host.closeConnection(test.hostId, test.connection);
    CHECK(test.closed == 1 && waitFor([&] { return gone(client); }));

    QTcpSocket ipv6;
    test.dial(ipv6);
    CHECK(ipv6.write(hello(peerV6, true)) > 0);
    CHECK(waitFor([&] { return test.authorized == 2; }));
    CHECK(test.lastPeer.address == peerV6);
    test.host.stop(test.hostId);
    CHECK(test.stopped == 1 && test.reason == SystemHostStop::Cancelled && test.closed == 2);
    CHECK(waitFor([&] { return gone(ipv6); }));
    QTcpSocket later;
    later.connectToHost(QHostAddress::LocalHost, test.port());
    CHECK(!later.waitForConnected(1000));
}

void startup(const QString &fixture)
{
    for (const auto config : {"{\"TCP\":{\"43826\":{}}}", "{\"AllowFunnel\":{\"node:43826\":true}}",
             "{\"Foreground\":{\"session\":{\"TCP\":{\"43826\":null}}}}"}) {
        Test test(fixture);
        test.put("serve.json", config);
        test.start(false);
        CHECK(test.reason == SystemHostStop::PortOccupied && !QFile::exists(test.file("relay-port")));
    }
    for (const auto config : {"", "[]", "{\"Foreground\":{\"session\":[]}}", "{\"TCP\":false}"}) {
        Test test(fixture);
        test.put("serve.json", config);
        test.start(false);
        CHECK(test.reason == SystemHostStop::PortCheckFailed && !QFile::exists(test.file("relay-port")));
    }
    {
        Test test(fixture);
        test.put("status.json", status({}, "different"));
        test.start(false);
        CHECK(test.reason == SystemHostStop::IdentityMismatch && !QFile::exists(test.file("relay-port")));
    }
    {
        Test test(fixture);
        test.installation.service = ServiceState::Stopped;
        test.start(false);
        CHECK(test.reason == SystemHostStop::InstallationUnavailable);
    }
    for (const auto flag : {"bad-banner", "output-limit"}) {
        Test test(fixture);
        test.put(flag);
        test.start(false);
        CHECK(test.reason == SystemHostStop::RelayFailed && test.listening == 0);
    }
    {
        Test test(fixture);
        test.put("serve.json", "{\"TCP\":{\"443\":{}},\"AllowFunnel\":{\"node:43826\":false}}");
        test.start();
        test.put("exit-relay");
        CHECK(waitFor([&] { return test.stopped == 1; }));
        CHECK(test.reason == SystemHostStop::RelayFailed);
    }
}

void peers(const QString &fixture)
{
    Test test(fixture);
    test.start();
    for (const auto &peers : {"\"a\":" + peer("peer", QByteArray::number(ownerId - 1)),
             "\"a\":" + peer("peer", "9223372036854775806.0"),
             "\"a\":" + peer("peer", owner, ",\"Tags\":[\"tag:server\"]"),
             "\"a\":" + peer("peer", owner, ",\"ShareeNode\":true"),
             "\"a\":" + peer() + ",\"b\":" + peer("other"),
             "\"a\":" + peer() + ",\"b\":" + peer()}) {
        test.put("status.json", status(peers));
        QTcpSocket client;
        test.dial(client);
        CHECK(client.write(hello()) > 0);
        CHECK(waitFor([&] { return gone(client); }));
        CHECK(test.authorized == 0 && client.readAll() == QByteArray(1, '\0'));
    }
    for (const auto &changed : {status({}, "changed"), status({}, "tailnet", "100.80.0.9")}) {
        test.put("status.json", changed);
        QTcpSocket client;
        test.dial(client);
        CHECK(client.write(hello()) > 0);
        CHECK(waitFor([&] { return gone(client); }));
        CHECK(test.authorized == 0 && client.readAll().isEmpty());
    }

    test.put("status.json", status());
    QTcpSocket client;
    test.dial(client);
    CHECK(client.write(hello()) > 0);
    CHECK(waitFor([&] { return test.authorized == 1; }));
    test.put("status.json", status("\"a\":" + peer("replacement")));
    CHECK(test.host.revalidate(test.hostId, test.connection));
    CHECK(waitFor([&] { return test.closed == 1; }));
    CHECK(test.rechecked == 0 && waitFor([&] { return gone(client); }));
}

void cancellation(const QString &fixture)
{
    Test test(fixture);
    CHECK(!test.host.start(generation - 1));
    test.start();
    test.allowOwner = false;
    QTcpSocket impostor;
    test.dial(impostor);
    CHECK(impostor.write(hello()) > 0);
    CHECK(waitFor([&] { return gone(impostor); }));
    CHECK(test.authorized == 0 && impostor.readAll().isEmpty());
    test.allowOwner = true;

    auto wrongVersion = hello();
    wrongVersion[12] = '\x20';
    auto hugeLength = hello();
    hugeLength[14] = '\x7f';
    auto wrongRequest = hello();
    wrongRequest[wrongRequest.size() - 1] = '!';
    for (const auto &data : {requestFrame().left(8), wrongVersion, hugeLength, wrongRequest, hello() + "extra"}) {
        QTcpSocket client;
        test.dial(client);
        CHECK(client.write(data) > 0);
        CHECK(waitFor([&] { return gone(client); }));
        CHECK(client.readAll().isEmpty() && test.authorized == 0);
    }

    QTcpSocket client;
    test.dial(client);
    CHECK(client.write(hello()) > 0);
    CHECK(waitFor([&] { return test.authorized == 1; }));
    ++test.pid;
    CHECK(!test.host.revalidate(test.hostId, test.connection));
    CHECK(waitFor([&] { return gone(client); }));
    CHECK(test.closed == 1 && test.rechecked == 0);

    QTcpSocket active;
    test.dial(active);
    CHECK(active.write(hello()) > 0);
    CHECK(waitFor([&] { return test.authorized == 2; }));
    test.put("delay-status");
    CHECK(test.host.revalidate(test.hostId, test.connection));
    CHECK(waitFor([&] { return QFile::exists(test.file("status-started")); }));
    test.host.setAuthorization(generation - 1, std::nullopt);
    CHECK(test.stopped == 0);
    test.host.setAuthorization(generation + 1, std::nullopt);
    CHECK(test.stopped == 1 && test.reason == SystemHostStop::AuthorizationChanged && test.closed == 2);
    CHECK(test.stopGeneration == generation && !test.host.start(generation));
    CHECK(test.rechecked == 0 && waitFor([&] { return gone(active); }));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CHECK(app.arguments().size() == 3);
    qputenv("AURORA_PAIR_HOST_TEST_SENTINEL", "must not be inherited");
    const auto scenario = app.arguments().at(1);
    const auto fixture = app.arguments().at(2);
    if (scenario == QLatin1String("transport")) transport(fixture);
    else if (scenario == QLatin1String("startup")) startup(fixture);
    else if (scenario == QLatin1String("peers")) peers(fixture);
    else if (scenario == QLatin1String("cancellation")) cancellation(fixture);
    else CHECK(false);
}
