// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedStatusClient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace aurora::tailscale {
namespace {

constexpr int requestTimeoutMs = 12000;
constexpr int presenceTimeoutMs = 900;
constexpr int maxPresenceRequests = 8;
constexpr int maxStreamLeases = 32;
constexpr int maxRequestBytes = 256 * 1024;
constexpr int sunshinePort = 48989;
constexpr int presenceSize = 192;
constexpr int nonceOffset = 18;
constexpr int nonceSize = 16;

bool validPresence(const QByteArray &packet, char kind)
{
    return packet.size() == presenceSize && packet.startsWith("AURORA/PRESENCE/1") && packet.at(17) == kind;
}

QByteArray json(QJsonObject object)
{
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact);
}

QByteArray publishParams(const AccountIdentity &account, const QByteArray &packet)
{
    auto encoded = json({{"enabled", true}, {"nodeId", account.nodeId}, {"packet", QString::fromLatin1(packet.toBase64())}});
    // Serialize the Go UserID as a decimal JSON number even on Qt 5.
    encoded.chop(1);
    return encoded + ",\"owner\":" + QByteArray::number(account.ownerId) + '}';
}

bool validLeaseId(const QString &id)
{
    if (id.size() != 32)
        return false;
    return std::all_of(id.cbegin(), id.cend(), [](QChar ch) {
        return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'));
    });
}

} // namespace

struct EmbeddedStatusClient::Reply
{
    Reply(StatusError error = StatusError::None) : error(error) {}

    StatusError error;
    std::optional<Status> status;
    std::optional<bool> loginPending;
    QByteArray packet;
    QString remoteId;
    quint16 port = 0;
};

StreamLease::StreamLease(EmbeddedStatusClient *client, quint64 token, quint16 port)
    : client(client), token(token), relayPort(port)
{
}

StreamLease::~StreamLease()
{
    close();
}

bool StreamLease::isValid() const
{
    if (!client)
        return false;
    Q_ASSERT(client->thread() == QThread::currentThread());
    const auto lease = client->leases.constFind(token);
    return lease != client->leases.cend() && !lease->closing && client->process.state() == QProcess::Running;
}

bool StreamLease::close()
{
    return client && client->requestClose(*this);
}

EmbeddedStatusClient::EmbeddedStatusClient(QString executable, QString stateDirectory, QObject *parent)
    : QObject(parent), executable(std::move(executable)), stateDirectory(QDir::cleanPath(stateDirectory))
{
    qRegisterMetaType<StatusError>();
    qRegisterMetaType<StatusResult>();
    qRegisterMetaType<LoginResult>();
    qRegisterMetaType<PresenceResult>();
    qRegisterMetaType<OpenResult>();
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    clock.start();
    dispatch.setSingleShot(true);
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    connect(&dispatch, &QTimer::timeout, this, &EmbeddedStatusClient::pump);
    connect(&deadline, &QTimer::timeout, this, &EmbeddedStatusClient::expireRequests);
    connect(&process, &QProcess::started, this, &EmbeddedStatusClient::pump);
    connect(&process, &QProcess::readyReadStandardOutput, this, &EmbeddedStatusClient::receive);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(StatusError::StartFailed);
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            fail(StatusError::Transport);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this] {
        stopping = false;
        fail(StatusError::Transport);
    });
}

EmbeddedStatusClient::~EmbeddedStatusClient()
{
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.closeWriteChannel();
        if (!process.waitForFinished(200))
            process.kill();
    }
}

QString EmbeddedStatusClient::bundledExecutable(const QString &applicationDirectory)
{
    if (!QDir::isAbsolutePath(applicationDirectory))
        return {};
    const QDir directory(applicationDirectory);
#ifdef Q_OS_WIN
    return directory.filePath(QStringLiteral("Components/Tailscale/AuroraTailnet.exe"));
#elif defined(Q_OS_MACOS)
    return directory.filePath(QStringLiteral("AuroraTailnet"));
#else
    return {};
#endif
}

bool EmbeddedStatusClient::isAvailable() const
{
    const QFileInfo file(executable);
    return file.isAbsolute() && file.isFile() && file.isExecutable();
}

bool EmbeddedStatusClient::requestStatus(quint64 generation)
{
    if (!startRequest({Operation::Status, generation}))
        return false;
    if (verifiedGeneration != generation)
        verifiedStatus.reset();
    return true;
}

bool EmbeddedStatusClient::requestLogin(quint64 generation)
{
    if (!startRequest({Operation::Login, generation}))
        return false;
    verifiedStatus.reset();
    return true;
}

bool EmbeddedStatusClient::requestLogout(quint64 generation)
{
    if (!startRequest({Operation::Logout, generation}))
        return false;
    verifiedStatus.reset();
    return true;
}

bool EmbeddedStatusClient::matchesAccount(quint64 generation) const
{
    return process.state() == QProcess::Running && verifiedStatus && verifiedGeneration == generation;
}

bool EmbeddedStatusClient::matchesPeer(quint64 generation, const QString &peerId, const QHostAddress &address) const
{
    if (!matchesAccount(generation) || peerId.isEmpty() || address.isNull() || !address.scopeId().isEmpty())
        return false;
    return std::any_of(verifiedStatus->peers.cbegin(), verifiedStatus->peers.cend(), [&](const Peer &peer) {
        return peer.nodeId == peerId && (peer.address.isEqual(address, QHostAddress::StrictConversion)
            || peer.alternateAddress.isEqual(address, QHostAddress::StrictConversion));
    });
}

bool EmbeddedStatusClient::requestHost(quint64 generation, bool enabled)
{
    if (process.state() != QProcess::Running || (enabled && !matchesAccount(generation)))
        return false;
    Request request{Operation::Host, generation};
    request.params = json({{"enabled", enabled}});
    request.enabled = enabled;
    return startRequest(std::move(request)) != 0;
}

bool EmbeddedStatusClient::requestPublish(quint64 generation, bool enabled, std::optional<AccountIdentity> account,
                                          const QByteArray &packet)
{
    if (process.state() != QProcess::Running)
        return false;
    Request request{Operation::Publish, generation};
    request.enabled = enabled;
    if (enabled) {
        if (!matchesAccount(generation) || !account || !(*account == *verifiedStatus->account) || !validPresence(packet, 2))
            return false;
        request.params = publishParams(*account, packet);
    } else {
        request.params = json({{"enabled", false}});
    }
    return startRequest(std::move(request)) != 0;
}

quint64 EmbeddedStatusClient::requestPresence(quint64 generation, const QString &peerId, const QHostAddress &address,
                                              const QByteArray &packet)
{
    if (!matchesPeer(generation, peerId, address) || !validPresence(packet, 1))
        return 0;
    Request request{Operation::Presence, generation};
    request.params = json({{"peerId", peerId}, {"address", address.toString()},
                           {"packet", QString::fromLatin1(packet.toBase64())}});
    request.nonce = packet.mid(nonceOffset, nonceSize);
    return startRequest(std::move(request));
}

quint64 EmbeddedStatusClient::requestOpen(quint64 generation, const QString &peerId, const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol || !matchesPeer(generation, peerId, address)
        || leases.size() >= maxStreamLeases) {
        return 0;
    }
    // The Go RPC checks the current account's peer by address and accepts only this port.
    Request request{Operation::Open, generation};
    request.params = json({{"address", address.toString()}, {"port", sunshinePort}});
    return startRequest(std::move(request));
}

bool EmbeddedStatusClient::cancelOpen(quint64 requestId)
{
    const auto request = requests.find(requestId);
    if (request == requests.end() || request->operation != Operation::Open)
        return false;
    request->cancelled = true;
    dispatch.start(0);
    return true;
}

bool EmbeddedStatusClient::requestClose(const StreamLease &lease)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (lease.client != this || !lease.isValid())
        return false;
    auto &owned = leases[lease.token];
    owned.closing = true;
    enqueueClose(lease.token, owned);
    return true;
}

void EmbeddedStatusClient::enqueueClose(quint64 openRequestId, const Lease &lease)
{
    Request close{Operation::Close, lease.generation};
    close.params = json({{"lease", lease.remoteId}});
    close.expires = clock.elapsed() + requestTimeoutMs;
    close.openRequestId = openRequestId;
    closes.enqueue(std::move(close));
    scheduleDeadline();
    dispatch.start(0);
}

quint64 EmbeddedStatusClient::startRequest(Request request)
{
    // Reserve space for the string ID, method, envelope and newline in Go's scanner limit.
    if (stopping || request.params.size() >= maxRequestBytes - 128)
        return 0;
    const bool presence = request.operation == Operation::Presence;
    if (presence) {
        const auto count = std::count_if(requests.cbegin(), requests.cend(), [](const Request &pending) {
            return pending.operation == Operation::Presence;
        });
        if (count >= maxPresenceRequests)
            return 0;
    } else if (foreground || !closes.isEmpty()) {
        return 0;
    }
    const quint64 id = ++serial;
    request.expires = clock.elapsed() + requestTimeoutMs;
    requests.insert(id, std::move(request));
    if (!presence)
        foreground = id;
    scheduleDeadline();
    dispatch.start(0);
    return id;
}

void EmbeddedStatusClient::pump()
{
    expireRequests();
    if (stopping)
        return;
    const auto active = requests.constFind(foreground);
    if (active != requests.cend() && active->cancelled && !active->sent)
        complete(foreground, StatusError::Cancelled);
    if (!foreground && !closes.isEmpty()) {
        foreground = ++serial;
        requests.insert(foreground, closes.dequeue());
        scheduleDeadline();
    }
    if (requests.isEmpty())
        return;
    if (process.state() == QProcess::NotRunning) {
        if (!isAvailable())
            return fail(StatusError::Unavailable);
        if (!QDir::isAbsolutePath(stateDirectory) || QDir(stateDirectory).isRoot())
            return fail(StatusError::InvalidStateDirectory);
        process.start(executable, {QStringLiteral("--state-dir"), stateDirectory});
        return;
    }
    if (process.state() != QProcess::Running)
        return;

    for (const auto id : requests.keys()) {
        auto request = requests.find(id);
        if (request == requests.end() || request->sent)
            continue;
        const char *method = nullptr;
        switch (request->operation) {
        case Operation::Status: method = "status"; break;
        case Operation::Login: method = "login"; break;
        case Operation::Logout: method = "logout"; break;
        case Operation::Host: method = "host"; break;
        case Operation::Publish: method = "publish"; break;
        case Operation::Presence: method = "presence"; break;
        case Operation::Open: method = "open"; break;
        case Operation::Close: method = "close"; break;
        }
        const auto bytes = "{\"id\":\"" + QByteArray::number(id) + "\",\"method\":\"" + method
            + "\",\"params\":" + request->params + "}\n";
        request->sent = true;
        if (process.write(bytes) != bytes.size())
            return fail(StatusError::Transport);
    }
}

void EmbeddedStatusClient::scheduleDeadline()
{
    qint64 earliest = closes.isEmpty() ? -1 : closes.head().expires;
    for (const auto &request : requests) {
        auto expires = request.expires;
        if (request.operation == Operation::Presence && !request.reportedTimeout)
            expires -= requestTimeoutMs - presenceTimeoutMs;
        earliest = earliest < 0 ? expires : std::min(earliest, expires);
    }
    if (earliest < 0)
        deadline.stop();
    else
        deadline.start(int(std::max<qint64>(0, earliest - clock.elapsed())));
}

void EmbeddedStatusClient::expireRequests()
{
    const auto now = clock.elapsed();
    if (!closes.isEmpty() && closes.head().expires <= now)
        return fail(StatusError::Timeout);
    for (const auto id : requests.keys()) {
        auto request = requests.find(id);
        if (request == requests.end())
            continue;
        if (request->expires <= now)
            return fail(StatusError::Timeout);
        if (request->operation != Operation::Presence || request->reportedTimeout
            || now < request->expires - (requestTimeoutMs - presenceTimeoutMs)) {
            continue;
        }
        if (!request->sent) {
            complete(id, StatusError::Timeout);
        } else {
            // Hold the slot until Go's reply arrives, even though the caller's probe timed out.
            request->reportedTimeout = true;
            emit presenceFinished(request->generation, id, {StatusError::Timeout, {}});
        }
    }
    scheduleDeadline();
}

void EmbeddedStatusClient::receive()
{
    expireRequests();
    while (!stopping && process.bytesAvailable() > 0) {
        const auto sent = std::count_if(requests.cbegin(), requests.cend(), [](const Request &request) { return request.sent; });
        if (!sent)
            return fail(StatusError::InvalidResponse);
        const auto primary = requests.constFind(foreground);
        const int limit = primary != requests.cend() && primary->sent && primary->operation == Operation::Status
            ? maxStatusReplyBytes : maxActionReplyBytes;
        output += process.readLine(limit + 2 - output.size());
        if (!output.endsWith('\n')) {
            if (output.size() > limit)
                fail(StatusError::ResponseTooLarge);
            return;
        }
        if (output.size() > limit + 1)
            return fail(StatusError::ResponseTooLarge);
        auto line = std::exchange(output, {});
        line.chop(1);

        const auto frame = QJsonDocument::fromJson(line).object();
        const auto echoed = frame.value("id").toString();
        const auto id = echoed.toULongLong();
        const auto request = requests.constFind(id);
        if (!id || echoed != QString::number(id) || request == requests.cend() || !request->sent
            || (sent == 1 && process.bytesAvailable() != 0)) {
            return fail(StatusError::InvalidResponse);
        }
        if (request->operation != Operation::Status && line.size() > maxActionReplyBytes)
            return fail(StatusError::ResponseTooLarge);

        auto reply = decode(*request, echoed, line, frame);
        if (clock.elapsed() >= request->expires)
            return fail(StatusError::Timeout);
        if (reply.error == StatusError::InvalidResponse || reply.error == StatusError::ResponseTooLarge
            || (request->operation == Operation::Close && reply.error != StatusError::None)) {
            return fail(reply.error);
        }
        complete(id, std::move(reply));
    }
}

EmbeddedStatusClient::Reply EmbeddedStatusClient::decode(const Request &request, const QString &id,
                                                         const QByteArray &line, const QJsonObject &frame)
{
    if (request.operation == Operation::Status) {
        auto result = decodeStatusReply(line, id);
        Reply reply(result.error);
        reply.status = std::move(result.status);
        return reply;
    }
    if (request.operation == Operation::Login) {
        const auto result = decodeLoginReply(line, id);
        Reply reply(result.error);
        reply.loginPending = result.pending;
        return reply;
    }
    if (request.operation == Operation::Logout || request.operation == Operation::Close)
        return decodeLogoutReply(line, id);

    const auto ok = frame.value("ok");
    const auto error = frame.value("error");
    const auto result = frame.value("result");
    if (frame.size() != 4 || !ok.isBool())
        return StatusError::InvalidResponse;
    if (!ok.toBool())
        return result.isNull() && error.isString() ? StatusError::HelperRejected : StatusError::InvalidResponse;
    if (!error.isNull())
        return StatusError::InvalidResponse;

    Reply reply;
    if (request.operation == Operation::Presence) {
        const auto encoded = result.toString().toLatin1();
        if (encoded.size() != presenceSize / 3 * 4)
            return StatusError::InvalidResponse;
        reply.packet = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!validPresence(reply.packet, 2) || reply.packet.toBase64() != encoded
            || reply.packet.mid(nonceOffset, nonceSize) != request.nonce) {
            return StatusError::InvalidResponse;
        }
    } else if (request.operation == Operation::Open) {
        const auto lease = result.toObject();
        const auto port = lease.value("port");
        const int number = port.toInt();
        reply.remoteId = lease.value("lease").toString();
        if (lease.size() != 2 || !validLeaseId(reply.remoteId) || port.toDouble() != number || number <= 5
            || number > 65535 - 21 || std::any_of(leases.cbegin(), leases.cend(), [&](const Lease &owned) {
                return owned.remoteId == reply.remoteId;
            })) {
            return StatusError::InvalidResponse;
        }
        reply.port = quint16(number);
    } else {
        const auto state = result.toObject();
        const auto enabled = state.value("enabled");
        if (state.size() != 1 || !enabled.isBool() || enabled.toBool() != request.enabled)
            return StatusError::InvalidResponse;
    }
    return reply;
}

void EmbeddedStatusClient::complete(quint64 id, Reply reply)
{
    const auto completed = requests.take(id);
    if (foreground == id)
        foreground = 0;
    scheduleDeadline();
    if (!closes.isEmpty())
        dispatch.start(0);
    notify(id, completed, std::move(reply));
}

void EmbeddedStatusClient::notify(quint64 id, const Request &completed, Reply reply)
{
    switch (completed.operation) {
    case Operation::Status:
        if (reply.status && reply.status->state == BackendState::Connected && reply.status->account
            && !reply.status->localAddress.isNull()) {
            verifiedStatus = reply.status;
            verifiedGeneration = completed.generation;
        } else {
            verifiedStatus.reset();
        }
        emit statusFinished(completed.generation, {reply.error, std::move(reply.status)});
        break;
    case Operation::Login:
        emit loginFinished(completed.generation, {reply.error, reply.loginPending});
        break;
    case Operation::Logout:
        if (reply.error == StatusError::None)
            invalidateLeases();
        emit logoutFinished(completed.generation, reply.error);
        break;
    case Operation::Host:
        emit hostFinished(completed.generation, reply.error);
        break;
    case Operation::Publish:
        emit publishFinished(completed.generation, reply.error);
        break;
    case Operation::Presence:
        if (!completed.reportedTimeout)
            emit presenceFinished(completed.generation, id, {reply.error, std::move(reply.packet)});
        break;
    case Operation::Open: {
        OpenResult result{reply.error, {}};
        if (reply.error == StatusError::None) {
            const Lease lease{completed.generation, std::move(reply.remoteId), completed.cancelled};
            leases.insert(id, lease);
            if (completed.cancelled)
                enqueueClose(id, lease);
            else
                result.lease.reset(new StreamLease(this, id, reply.port));
        }
        if (completed.cancelled)
            result.error = StatusError::Cancelled;
        emit openFinished(completed.generation, id, result);
        break;
    }
    case Operation::Close:
        leases.remove(completed.openRequestId);
        emit closeFinished(completed.generation, completed.openRequestId, reply.error);
        break;
    }
}

void EmbeddedStatusClient::invalidateLeases()
{
    if (leases.isEmpty())
        return;
    leases.clear();
    emit leasesInvalidated();
}

void EmbeddedStatusClient::fail(StatusError error)
{
    const auto pending = std::exchange(requests, {});
    const auto cleanup = std::exchange(closes, {});
    foreground = 0;
    output.clear();
    verifiedStatus.reset();
    dispatch.stop();
    deadline.stop();
    if (!stopping && process.state() != QProcess::NotRunning) {
        stopping = true;
        process.kill();
    }
    invalidateLeases();
    for (auto entry = pending.cbegin(); entry != pending.cend(); ++entry)
        notify(entry.key(), entry.value(), error);
    for (const auto &close : cleanup)
        notify(0, close, error);
}

} // namespace aurora::tailscale
