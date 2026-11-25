// SPDX-License-Identifier: GPL-3.0-only
#include "BundledAdmin.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QQueue>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdlib>
#include <utility>

namespace Aurora::Sunshine {

struct BundledAdminFixture
{
    static auto open(const QString &profile, const QByteArray &password, quint16 port, QString &error)
    {
        return BundledAdmin::openAtPort(profile, password, port, error);
    }

    static quint16 port(const BundledAdmin &admin)
    {
        return admin.port_;
    }
};

} // namespace Aurora::Sunshine

namespace {

using Aurora::Sunshine::BundledAdmin;
using Aurora::Sunshine::BundledAdminFixture;
using Error = BundledAdmin::Error;
using Result = BundledAdmin::Result;

constexpr int bodyLimit = 256 * 1024;
const QByteArray fixturePassword(64, 'A');
const auto statusKey = QStringLiteral("status");
const auto upstreamKey = QStringLiteral("upstream_option");
const auto portKey = QStringLiteral("port");

void require(bool condition, const QString &message)
{
    if (!condition) {
        QTextStream(stderr) << message << '\n';
        std::exit(1);
    }
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot open fixture ") + path);
    return file.readAll();
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(),
            QStringLiteral("Cannot write temporary fixture ") + path);
}

struct Identity
{
    QByteArray pem;
    QSslCertificate certificate;
    QSslKey privateKey;
};

Identity loadIdentity(const QDir &dir, const QString &name)
{
    const auto pem = readFile(dir.filePath(name + QStringLiteral("-cert.pem")));
    const QSslCertificate certificate(pem);
    const QSslKey privateKey(readFile(dir.filePath(name + QStringLiteral("-key.pem"))), QSsl::Rsa);
    require(!certificate.isNull() && !privateKey.isNull(), QStringLiteral("Invalid synthetic TLS identity."));
    return {pem, certificate, privateKey};
}

struct Request
{
    QByteArray startLine;
    QByteArray authorization;
    QByteArray cookie;
    QByteArray body;
};

class HttpsFixture final : public QTcpServer
{
public:
    explicit HttpsFixture(Identity initial) : identity(std::move(initial))
    {
        require(listen(QHostAddress::LocalHost, 0), QStringLiteral("Cannot listen on ephemeral IPv4 loopback."));
    }

    Identity identity;
    QQueue<QByteArray> responses;
    QList<Request> requests;
    qint64 decryptedBytes = 0;
    int connections = 0;
    bool holdHandshake = false;
    qintptr waitingHandshake = -1;

    void releaseHandshake()
    {
        if (waitingHandshake != -1)
            acceptTls(std::exchange(waitingHandshake, -1));
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        ++connections;
        if (holdHandshake) {
            waitingHandshake = descriptor;
            return;
        }
        acceptTls(descriptor);
    }

private:
    void acceptTls(qintptr descriptor)
    {
        auto *socket = new QSslSocket(this);
        require(socket->setSocketDescriptor(descriptor), QStringLiteral("Fixture socket initialization failed."));
        socket->setLocalCertificate(identity.certificate);
        socket->setPrivateKey(identity.privateKey);
        socket->setProtocol(QSsl::TlsV1_2OrLater);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

        connect(socket, &QIODevice::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
            const auto data = socket->readAll();
            decryptedBytes += data.size();
            buffer += data;
            const auto headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;

            const auto lines = buffer.left(headerEnd).split('\n');
            Request request;
            request.startLine = lines.first().trimmed();
            int contentLength = 0;
            for (const auto &line : lines) {
                const auto separator = line.indexOf(':');
                if (separator < 0)
                    continue;
                const auto name = line.left(separator).toLower();
                const auto value = line.mid(separator + 1).trimmed();
                if (name == "authorization")
                    request.authorization = value;
                else if (name == "cookie")
                    request.cookie = value;
                else if (name == "content-length")
                    contentLength = value.toInt();
            }
            if (buffer.size() < headerEnd + 4 + contentLength)
                return;

            request.body = buffer.mid(headerEnd + 4, contentLength);
            requests.append(std::move(request));
            buffer.clear();
            require(!responses.isEmpty(), QStringLiteral("Unexpected fixture request."));
            const auto response = responses.dequeue();
            if (!response.isNull()) {
                socket->write(response);
                socket->disconnectFromHost();
            }
        });
        socket->startServerEncryption();
    }
};

QByteArray reply(int status, const QByteArray &body, const QByteArray &extraHeaders = {})
{
    return QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " Test\r\nConnection: close\r\n"
        "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" +
        extraHeaders + "\r\n" + body;
}

std::unique_ptr<BundledAdmin> openFixture(const QString &profile, const HttpsFixture &server)
{
    QString error;
    auto admin = BundledAdminFixture::open(profile, fixturePassword, server.serverPort(), error);
    require(admin != nullptr, QStringLiteral("Cannot open fixture client. ") + error);
    return admin;
}

template <typename T = Result, typename Start>
T waitFor(Start start)
{
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    std::optional<T> result;
    require(start([&](T value) {
        require(!result, QStringLiteral("Completion ran twice."));
        result = std::move(value);
        loop.quit();
    }), QStringLiteral("Client refused to start fixture operation."));
    require(!result, QStringLiteral("Completion must be asynchronous."));
    watchdog.start(6500);
    loop.exec();
    require(result.has_value(), QStringLiteral("No completion from admin client."));
    return std::move(*result);
}

Result read(BundledAdmin &admin)
{
    return waitFor([&](BundledAdmin::Completion completion) {
        return admin.readConfig(std::move(completion));
    });
}

void expect(const Result &result, Error code, int status, const QString &name)
{
    require(result.error == code && result.httpStatus == status,
            QStringLiteral("%1 returned error %2 HTTP %3. %4")
                .arg(name).arg(int(result.error)).arg(result.httpStatus).arg(result.message));
    require(result.message.isEmpty() == (code == Error::None), name + QStringLiteral(" error message"));
}

void spinFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void testOpen(const QString &profile, const QString &certPath, const Identity &pinned)
{
    QString error;
    require(!BundledAdmin::open(profile, fixturePassword, error) && !error.isEmpty(),
            QStringLiteral("Missing profile certificate must fail."));
    writeFile(certPath, QByteArray(64 * 1024 + 1, 'x'));
    require(!BundledAdmin::open(profile, fixturePassword, error) && !error.isEmpty(),
            QStringLiteral("Oversized certificate must fail."));
    writeFile(certPath, "not a certificate");
    require(!BundledAdmin::open(profile, fixturePassword, error) && !error.isEmpty(),
            QStringLiteral("Invalid certificate must fail."));

    writeFile(certPath, pinned.pem);
    require(!BundledAdmin::open(profile, QByteArray(64, 'z'), error) && !error.isEmpty(),
            QStringLiteral("Non-hex launch credentials must fail."));
    require(!BundledAdmin::open(profile, fixturePassword.left(63), error) && !error.isEmpty(),
            QStringLiteral("Truncated launch credentials must fail."));
    require(!BundledAdmin::open(QStringLiteral("relative"), fixturePassword, error) && !error.isEmpty(),
            QStringLiteral("Relative profile must fail."));
    require(!BundledAdmin::open(QStringLiteral("//example.invalid/share"), fixturePassword, error) && !error.isEmpty(),
            QStringLiteral("Network profile must fail before accessing it."));

    const auto admin = BundledAdmin::open(profile, fixturePassword, error);
    require(admin && error.isEmpty() && BundledAdminFixture::port(*admin) == 48990,
            QStringLiteral("Production factory must use the bundled port."));
}

void testReadAndErrors(const QString &profile, const Identity &pinned, const Identity &other)
{
    HttpsFixture server(pinned);
    HttpsFixture redirectTarget(other);
    auto admin = openFixture(profile, server);
    const QByteArray good = R"({"status":true,"port":"48989"})";
    server.responses.enqueue(reply(200, good));
    const auto originalProxy = QNetworkProxy::applicationProxy();
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"),
                                                      redirectTarget.serverPort()));
    auto result = waitFor([&](BundledAdmin::Completion completion) {
        const bool started = admin->readConfig(std::move(completion));
        require(!admin->readConfig([](Result) { require(false, QStringLiteral("Concurrent request ran.")); }),
                QStringLiteral("Overlapping request must be refused."));
        return started;
    });
    QNetworkProxy::setApplicationProxy(originalProxy);
    expect(result, Error::None, 200, QStringLiteral("Read config"));
    require(redirectTarget.connections == 0, QStringLiteral("The process proxy received the private request."));
    require(result.config.value(portKey).toString() == QStringLiteral("48989"),
            QStringLiteral("Read returned the wrong configuration."));
    require(server.requests.size() == 1 && server.requests.first().startLine == "GET /api/config HTTP/1.1" &&
            server.requests.first().authorization == "Basic " + (QByteArray("aurora-local:") + fixturePassword).toBase64(),
            QStringLiteral("Unexpected config route or Basic authorization."));

    const auto location = "Location: https://127.0.0.1:" + QByteArray::number(redirectTarget.serverPort()) + "/api/config\r\n";
    server.responses.enqueue(reply(307, good, location));
    expect(read(*admin), Error::HttpStatus, 307, QStringLiteral("Manual redirect"));
    require(redirectTarget.connections == 0, QStringLiteral("Redirect was followed."));

    server.responses.enqueue(reply(401, good, "WWW-Authenticate: Basic realm=\"test\"\r\n"));
    expect(read(*admin), Error::Unauthorized, 401, QStringLiteral("Unauthorized"));
    server.responses.enqueue(reply(503, good));
    expect(read(*admin), Error::HttpStatus, 503, QStringLiteral("HTTP error"));
    server.responses.enqueue(reply(200, "{"));
    expect(read(*admin), Error::InvalidResponse, 200, QStringLiteral("Malformed JSON"));
    server.responses.enqueue(reply(200, "[true]"));
    expect(read(*admin), Error::InvalidResponse, 200, QStringLiteral("Object required"));
    server.responses.enqueue(reply(200, R"({"status":"true"})"));
    expect(read(*admin), Error::Rejected, 200, QStringLiteral("Boolean status required"));
    server.responses.enqueue(reply(200, R"({"status":false})"));
    expect(read(*admin), Error::Rejected, 200, QStringLiteral("Application error"));
    server.responses.enqueue(reply(200, "{}"));
    expect(read(*admin), Error::Rejected, 200, QStringLiteral("Missing status"));
    server.responses.enqueue("HTTP/1.1 200 Test\r\nContent-Length: 64\r\nConnection: close\r\n\r\n" + good);
    expect(read(*admin), Error::Network, 200, QStringLiteral("Truncated body"));
    server.responses.enqueue(reply(200, good, "Content-Encoding: gzip\r\n"));
    expect(read(*admin), Error::InvalidResponse, 200, QStringLiteral("Unexpected content encoding"));

    server.responses.enqueue(reply(200, good.leftJustified(bodyLimit)));
    expect(read(*admin), Error::None, 200, QStringLiteral("Exact body limit"));
    server.responses.enqueue(reply(200, good.leftJustified(bodyLimit + 1)));
    expect(read(*admin), Error::ResponseTooLarge, 200, QStringLiteral("Declared oversized body"));
    server.responses.enqueue(QByteArray("HTTP/1.1 200 Test\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n") +
        QByteArray::number(bodyLimit + 1, 16) + "\r\n" + good.leftJustified(bodyLimit + 1) + "\r\n0\r\n\r\n");
    expect(read(*admin), Error::ResponseTooLarge, 200, QStringLiteral("Chunked oversized body"));

    // The fixture stays connected and sends no response for this exchange.
    server.responses.enqueue({});
    QElapsedTimer elapsed;
    elapsed.start();
    expect(read(*admin), Error::Timeout, 0, QStringLiteral("Deadline"));
    require(elapsed.elapsed() >= 4900 && elapsed.elapsed() < 6300, QStringLiteral("Unexpected timeout duration."));
    server.responses.enqueue(reply(200, good));
    expect(read(*admin), Error::None, 200, QStringLiteral("Recovery after timeout"));
}

void testSave(const QString &profile, const Identity &pinned)
{
    HttpsFixture server(pinned);
    auto admin = openFixture(profile, server);
    const QByteArray before = R"({"status":true,"port":"48989","upstream_option":{"keep":7}})";
    const QByteArray after = R"({"status":true,"port":"48989","upstream_option":{"keep":7},"encoder":"new"})";
    const QByteArray ack = R"({"status":true})";
    const auto encoder = QStringLiteral("encoder");

    const auto prepare = [&](const QJsonObject &current, QString &) -> std::optional<QJsonObject> {
        require(current.contains(upstreamKey), QStringLiteral("Save must use the freshly fetched configuration."));
        auto payload = current;
        payload.remove(statusKey);
        payload.insert(encoder, QStringLiteral("new"));
        return payload;
    };
    const auto verify = [&](const QJsonObject &actual) {
        return actual.value(encoder).toString() == QStringLiteral("new") ? QString{} : QStringLiteral("Readback differs.");
    };
    const auto save = [&](BundledAdmin::PreparePayload build) {
        return waitFor([&](BundledAdmin::Completion completion) {
            return admin->saveConfig(std::move(build), verify, std::move(completion));
        });
    };

    server.responses.enqueue(reply(200, before, "Set-Cookie: never=forward\r\n"));
    server.responses.enqueue(reply(200, ack));
    server.responses.enqueue(reply(200, after));
    const auto result = save(prepare);
    expect(result, Error::None, 200, QStringLiteral("Verified save"));
    require(result.config.value(encoder).toString() == QStringLiteral("new") && server.requests.size() == 3 &&
            server.connections == 3, QStringLiteral("Save must use three separately pinned exchanges."));
    require(server.requests.at(0).startLine.startsWith("GET ") && server.requests.at(1).startLine.startsWith("POST ") &&
            server.requests.at(2).startLine.startsWith("GET "), QStringLiteral("Expected fresh GET, POST, readback GET."));
    require(server.requests.at(1).cookie.isEmpty() && server.requests.at(2).cookie.isEmpty(),
            QStringLiteral("Fixture cookie was forwarded."));
    const auto payload = QJsonDocument::fromJson(server.requests.at(1).body).object();
    require(!payload.contains(statusKey) && payload.value(upstreamKey).toObject().value(QStringLiteral("keep")).toInt() == 7,
            QStringLiteral("Prepared payload lost unknown configuration or gained response metadata."));

    server.responses.enqueue(reply(200, before));
    server.responses.enqueue(reply(200, ack));
    server.responses.enqueue(reply(200, before));
    expect(save(prepare), Error::ReadbackMismatch, 200, QStringLiteral("Acknowledged but unchanged"));

    server.responses.enqueue(reply(200, before));
    server.responses.enqueue(reply(200, R"({"status":false})"));
    expect(save(prepare), Error::Rejected, 200, QStringLiteral("Rejected write"));

    server.responses.enqueue(reply(200, before));
    expect(save([](const QJsonObject &, QString &error) -> std::optional<QJsonObject> {
        error = QStringLiteral("Rejected by config validation.");
        return std::nullopt;
    }), Error::InvalidPayload, 0, QStringLiteral("Invalid prepared payload"));

    server.responses.enqueue(reply(200, before));
    expect(save([&](const QJsonObject &, QString &) -> std::optional<QJsonObject> {
        return QJsonObject{{encoder, QString(bodyLimit, QLatin1Char('x'))}};
    }), Error::InvalidPayload, 0, QStringLiteral("Oversized prepared payload"));
    require(server.requests.size() == 10 && server.responses.isEmpty(),
            QStringLiteral("A denied write or readback made an extra request."));
}

void testWrongPinAndCancellation(const QString &profile, const Identity &pinned, const Identity &other)
{
    HttpsFixture wrong(other);
    auto admin = openFixture(profile, wrong);
    expect(read(*admin), Error::Tls, 0, QStringLiteral("Wrong certificate"));
    spinFor(100);
    require(wrong.connections == 1 && wrong.decryptedBytes == 0,
            QStringLiteral("Wrong certificate received HTTP data."));

    HttpsFixture rotated(pinned);
    auto writing = openFixture(profile, rotated);
    rotated.responses.enqueue(reply(200, R"({"status":true})"));
    qint64 readBytes = -1;
    const auto rejected = waitFor([&](BundledAdmin::Completion completion) {
        return writing->saveConfig([&](const QJsonObject &, QString &) -> std::optional<QJsonObject> {
            readBytes = rotated.decryptedBytes;
            rotated.identity = other;
            return QJsonObject{{QStringLiteral("encoder"), QStringLiteral("test")}};
        }, [](const QJsonObject &) { return QString{}; }, std::move(completion));
    });
    expect(rejected, Error::Tls, 0, QStringLiteral("Certificate changed before POST"));
    spinFor(100);
    require(rotated.connections == 2 && rotated.requests.size() == 1 && rotated.decryptedBytes == readBytes,
            QStringLiteral("Rotated certificate received HTTP data."));

    HttpsFixture hanging(pinned);
    hanging.responses.enqueue({});
    auto cancelling = openFixture(profile, hanging);
    require(cancelling->readConfig([](Result) { require(false, QStringLiteral("Destroyed client called back.")); }),
            QStringLiteral("Cancellation test could not start."));
    for (int attempt = 0; attempt < 50 && hanging.requests.isEmpty(); ++attempt)
        spinFor(10);
    require(hanging.requests.size() == 1, QStringLiteral("Cancellation test never reached the fixture."));
    cancelling.reset();
    spinFor(100);

    hanging.responses.enqueue(reply(200, R"({"status":true})"));
    cancelling = openFixture(profile, hanging);
    const auto result = waitFor([&](BundledAdmin::Completion completion) {
        return cancelling->readConfig([&cancelling, completion = std::move(completion)](Result current) {
            cancelling.reset();
            completion(std::move(current));
        });
    });
    expect(result, Error::None, 200, QStringLiteral("Completion can release the client"));
}

void testPairing(const QString &profile, const Identity &pinned, const Identity &other)
{
    using PendingResult = BundledAdmin::PendingResult;
    using ApprovalResult = BundledAdmin::ApprovalResult;
    using Aurora::Sunshine::SupervisorPairingApproval;
    HttpsFixture server(pinned);
    auto admin = openFixture(profile, server);
    const auto id = QStringLiteral("00112233445566778899aAbBcCdDeEfF");
    const auto pin = QStringLiteral("0123");
    const auto name = QStringLiteral("Offline fixture");

    server.responses.enqueue(reply(200, R"json({"pairings":[
        {"id":"00112233445566778899aAbBcCdDeEfF","name":"device 1.0","address":"::ffff:127.0.0.1","sourcePort":65535},
        {"id":"00112233445566778899aAbBcCdDeEfF","name":"other","address":"192.0.2.10","sourcePort":1},
        {"id":"00112233445566778899aAbBcCdDeEfF","address":"127.1","sourcePort":1e2},
        {"id":"00112233445566778899aAbBcCdDeEfF","address":"127.00.0.1","sourcePort":42.0},
        {"id":"00112233445566778899aAbBcCdDeEfF","address":"2001:db8::1","sourcePort":"443"},
        {"id":"00112233445566778899aAbBcCdDeEfF","address":"127.0.0.1:55","sourcePort":65536},
        {"id":"00112233445566778899aAbBcCdDeEfF","name":42,"address":false,"sourcePort":0},
        {"id":"00112233445566778899aAbBcCdDeEfF","address":"127.0.0.1"},
        {"id":"00112233445566778899aAbBcCdDeEfF","sourcePort":-1},
        {"id":"00112233445566778899aAbBcCdDeEfF","sourcePort":12.5},
        {"id":"00112233445566778899aAbBcCdDeEfF","sourcePort":true},
        {"id":"bad","sourcePort":1234}, false
    ]})json"));
    const auto list = waitFor<PendingResult>([&](BundledAdmin::PendingCompletion done) {
        return admin->readPendingPairings(std::move(done));
    });
    require(bool(list) && list.pairings.size() == 11 && server.requests.size() == 1 &&
            server.requests.first().startLine == "GET /api/pin HTTP/1.1",
            QStringLiteral("Pending GET did not return the legacy array, or it wrote to the host."));
    require(list.pairings.at(0).id == id && list.pairings.at(0).name == QStringLiteral("device 1.0") &&
            list.pairings.at(0).sourceIpv4 == QHostAddress(QHostAddress::LocalHost) && list.pairings.at(0).sourcePort == 65535 &&
            list.pairings.at(1).sourceIpv4 == QHostAddress(QStringLiteral("192.0.2.10")) && list.pairings.at(1).sourcePort == 1,
            QStringLiteral("Strict IPv4, mapped IPv4, duplicate IDs, or integer boundary ports were lost."));
    for (int i = 2; i < list.pairings.size(); ++i)
        require(!list.pairings.at(i).sourcePort, QStringLiteral("An invalid or missing source port was trusted."));
    require(!list.pairings.at(2).sourceIpv4 && !list.pairings.at(3).sourceIpv4 &&
            !list.pairings.at(4).sourceIpv4 && list.pairings.at(4).address == QStringLiteral("2001:db8::1") &&
            !list.pairings.at(5).sourceIpv4 && list.pairings.at(6).address.isEmpty() && list.pairings.at(6).name.isEmpty(),
            QStringLiteral("An ambiguous address or wrong JSON field type was promoted to a typed IPv4 source."));

    const auto getPending = [&] {
        return waitFor<PendingResult>([&](BundledAdmin::PendingCompletion done) {
            return admin->readPendingPairings(std::move(done));
        });
    };
    server.responses.enqueue(reply(200, R"({"status":true,"pairings":{}})"));
    require(getPending().error == Error::InvalidResponse, QStringLiteral("Pending lookup accepted a non-array."));
    server.responses.enqueue(reply(401, R"({"pairings":[]})"));
    require(getPending().error == Error::Unauthorized, QStringLiteral("Pending lookup accepted missing authorization."));

    const auto approve = [&](QString pairingId, QString enteredPin, QString deviceName) {
        return waitFor<ApprovalResult>([&](BundledAdmin::ApprovalCompletion done) {
            return admin->approvePairing(SupervisorPairingApproval::authorized(pairingId, enteredPin, deviceName),
                                         std::move(done));
        });
    };
    const auto beforeApproval = server.requests.size();
    for (const auto &badPin : {QStringLiteral("123"), QStringLiteral("12345"), QStringLiteral(" 123"), QStringLiteral("\uff11\uff12\uff13\uff14")})
        require(approve(id, badPin, name).error == Error::InvalidPayload, QStringLiteral("Invalid PIN was accepted."));
    require(approve(id + QStringLiteral("\n"), pin, name).error == Error::InvalidPayload &&
            approve(id, pin, QStringLiteral("\u3000")).error == Error::InvalidPayload &&
            approve(id, pin, QStringLiteral("name#")).error == Error::InvalidPayload &&
            approve(id, pin, QStringLiteral("name\r")).error == Error::InvalidPayload &&
            approve(id, pin, QString(43, QChar(0x754c))).error == Error::InvalidPayload &&
            server.requests.size() == beforeApproval, QStringLiteral("An invalid approval reached the wire."));

    server.responses.enqueue(reply(200, R"({"status":true})"));
    require(bool(approve(id, pin, name)), QStringLiteral("The synthetic approval was refused."));
    const auto request = server.requests.last();
    const auto expected = QJsonObject{{QStringLiteral("pairing_id"), id}, {QStringLiteral("pin"), pin},
                                      {QStringLiteral("name"), name}};
    require(request.startLine == "POST /api/pin HTTP/1.1" && QJsonDocument::fromJson(request.body).object() == expected &&
            request.authorization == "Basic " + (QByteArray("aurora-local:") + fixturePassword).toBase64(),
            QStringLiteral("Approval must submit the exact ID, PIN, and name over pinned authenticated HTTPS."));

    server.responses.enqueue(reply(200, R"({"status":false})"));
    require(approve(id, pin, name).error == Error::Rejected, QStringLiteral("An unacknowledged approval succeeded."));
    server.responses.enqueue(reply(200, R"({"status":"true"})"));
    require(approve(id, pin, name).error == Error::Rejected, QStringLiteral("A non-boolean status acknowledged approval."));

    server.responses.enqueue(reply(401, R"({"status":true})"));
    require(approve(id, pin, name).error == Error::Unauthorized, QStringLiteral("An unauthorized response acknowledged approval."));

    HttpsFixture held(pinned);
    held.holdHandshake = true;
    auto delayed = openFixture(profile, held);
    bool authorized = true;
    std::optional<ApprovalResult> outcome;
    require(delayed->approvePairing(SupervisorPairingApproval::authorized(id, pin, name),
        [&](ApprovalResult result) { outcome = std::move(result); },
        [&] { return authorized; }), QStringLiteral("Approval with a test guard did not start."));
    for (int i = 0; i < 100 && held.waitingHandshake == -1; ++i)
        spinFor(5);
    require(held.waitingHandshake != -1, QStringLiteral("Approval did not reach the held TLS handshake."));
    authorized = false;
    held.releaseHandshake();
    for (int i = 0; i < 100 && !outcome; ++i)
        spinFor(5);
    require(outcome && outcome->error == Error::Cancelled && held.decryptedBytes == 0,
            QStringLiteral("TLS guard returned error %1 with %2 HTTP bytes received.")
                .arg(outcome ? int(outcome->error) : -1).arg(held.decryptedBytes));

    HttpsFixture wrong(other);
    auto wrongAdmin = openFixture(profile, wrong);
    const auto refused = waitFor<ApprovalResult>([&](BundledAdmin::ApprovalCompletion done) {
        return wrongAdmin->approvePairing(SupervisorPairingApproval::authorized(id, pin, name), std::move(done));
    });
    require(refused.error == Error::Tls && wrong.decryptedBytes == 0,
            QStringLiteral("A wrong pin received approval data."));
}


} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    require(app.arguments().size() == 2, QStringLiteral("Expected a path to the TLS fixtures."));
    require(QSslSocket::supportsSsl(), QStringLiteral("Qt TLS backend unavailable. Build backend: ") +
            QSslSocket::sslLibraryBuildVersionString());
    const QDir fixtures(app.arguments().at(1));
    const auto pinned = loadIdentity(fixtures, QStringLiteral("pinned"));
    const auto other = loadIdentity(fixtures, QStringLiteral("other"));

    QTemporaryDir profile(QDir::current().filePath(QStringLiteral("sunshine-test-profile-XXXXXX")));
    require(profile.isValid() && QDir(profile.path()).mkdir(QStringLiteral("credentials")),
            QStringLiteral("Could not create isolated fixture profile."));
    const auto certPath = QDir(profile.path()).filePath(QStringLiteral("credentials/cacert.pem"));
    testOpen(profile.path(), certPath, pinned);
    testReadAndErrors(profile.path(), pinned, other);
    testSave(profile.path(), pinned);
    testWrongPinAndCancellation(profile.path(), pinned, other);
    testPairing(profile.path(), pinned, other);
    QTextStream(stdout) << "PASS: loopback TLS pin and credential isolation, bounded HTTPS config read/save, "
                          "readback, timeout, cancellation, and exact pairing approval\n";
}
