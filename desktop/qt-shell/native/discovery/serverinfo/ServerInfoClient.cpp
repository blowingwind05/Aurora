// SPDX-License-Identifier: GPL-3.0-only
#include "ServerInfoClient.h"
#include "HttpBody.h"

#include <QDeadlineTimer>
#include <QNetworkProxy>
#include <QQueue>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace Aurora::Discovery::ServerInfo {
namespace {

constexpr int MaxBatchTimeoutMs = 5000;

bool literalUnicast(const QHostAddress &address)
{
    return (address.protocol() == QAbstractSocket::IPv4Protocol ||
            address.protocol() == QAbstractSocket::IPv6Protocol) &&
        address.scopeId().isEmpty() && !address.isNull() && !address.isMulticast() &&
        address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6 &&
        address != QHostAddress::Broadcast;
}

QString endpointOrder(const Endpoint &endpoint)
{
    const auto address = endpoint.address.toString();
    const auto host = endpoint.address.protocol() == QAbstractSocket::IPv6Protocol
        ? QLatin1Char('[') + address + QLatin1Char(']') : address;
    return host + QLatin1Char(':') + QString::number(endpoint.port);
}

class Exchange final : public QObject
{
public:
    using Completion = std::function<void(Exchange *, CheckResult)>;

    Exchange(const Candidate &candidate, quint16 port, int timeoutMs, Completion completion, QObject *parent)
        : QObject(parent), remote_{candidate.address, port}, embedded_(candidate.loopbackRelay.has_value()),
          deadline_(timeoutMs, Qt::PreciseTimer), socket_(this), alarm_(this), completion_(std::move(completion))
    {
        const auto target = candidate.loopbackRelay.value_or(remote_);
        // A literal TCP endpoint and fixed request keep authentication and redirect handling out of this exchange.
        socket_.setProxy(QNetworkProxy::NoProxy);
        socket_.setReadBufferSize(16384);
        connect(&socket_, &QTcpSocket::connected, this, [this, target] {
            const auto request = "GET /serverinfo HTTP/1.1\r\nHost: " + endpointOrder(target).toLatin1() +
                "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
            if (socket_.write(request) != request.size())
                finish(Status::Unavailable);
        });
        connect(&socket_, &QTcpSocket::readyRead, this, [this] { receive(false); });
        connect(&socket_, &QTcpSocket::disconnected, this, [this] { receive(true); });
        connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
            if (error != QAbstractSocket::RemoteHostClosedError)
                finish(Status::Unavailable);
        });
        alarm_.setSingleShot(true);
        alarm_.setTimerType(Qt::PreciseTimer);
        connect(&alarm_, &QTimer::timeout, this, [this] { finish(Status::Timeout); });
        alarm_.start(static_cast<int>(deadline_.remainingTime()));
        // Defer connectToHost so an immediate failure cannot complete before the caller stores this exchange.
        QTimer::singleShot(0, this, [this, target] {
            if (completion_)
                socket_.connectToHost(target.address, target.port);
        });
    }

    ~Exchange() override { stop(); }

    void stop()
    {
        completion_ = {};
        alarm_.stop();
        disconnect(&socket_, nullptr, this, nullptr);
        socket_.abort();
    }

private:
    void receive(bool eof)
    {
        if (deadline_.hasExpired()) {
            finish(Status::Timeout);
            return;
        }
        auto state = body_.append(socket_.readAll());
        if (eof)
            state = body_.end();
        if (state == Detail::HttpBody::Result::Invalid) {
            finish(Status::Unavailable);
        } else if (state == Detail::HttpBody::Result::Complete) {
            auto parsed = parse(body_.body(), remote_, embedded_);
            const auto status = parsed ? Status::Complete : Status::Unavailable;
            finish(status, std::move(parsed));
        }
    }

    void finish(Status status, std::optional<StreamHost> current = {})
    {
        auto completion = std::move(completion_);
        stop();
        deleteLater();
        if (completion)
            completion(this, {status, std::move(current)});
    }

    const Endpoint remote_;
    const bool embedded_;
    const QDeadlineTimer deadline_;
    QTcpSocket socket_;
    QTimer alarm_;
    Detail::HttpBody body_;
    Completion completion_;
};

} // namespace

struct Client::Batch : QObject
{
    Batch(quint64 id, quint64 revision, int timeoutMs, QObject *parent)
        : QObject(parent), id(id), revision(revision), deadline(timeoutMs, Qt::PreciseTimer), alarm(this)
    {
        alarm.setSingleShot(true);
        alarm.setTimerType(Qt::PreciseTimer);
    }

    const quint64 id;
    const quint64 revision;
    const QDeadlineTimer deadline;
    QTimer alarm;
    QQueue<Candidate> pending;
    QSet<Exchange *> running;
    QList<StreamHost> hosts;
};

struct Client::Check : QObject
{
    Check(quint64 id, quint64 revision, const StreamHost &expected, const Candidate &candidate,
          RouteRequirement requirement, bool wait, int timeoutMs, QObject *parent)
        : QObject(parent), id(id), revision(revision), expected(expected), candidate(candidate),
          requirement(requirement), wait(wait), deadline(timeoutMs, Qt::PreciseTimer), alarm(this)
    {
        alarm.setSingleShot(true);
        alarm.setTimerType(Qt::PreciseTimer);
    }

    const quint64 id;
    const quint64 revision;
    const StreamHost expected;
    const Candidate candidate;
    const RouteRequirement requirement;
    const bool wait;
    const QDeadlineTimer deadline;
    QTimer alarm;
    Exchange *running = nullptr;
};

Client::Client(QObject *parent, quint16 sunshinePort)
    : QObject(parent), port_(sunshinePort)
{
    qRegisterMetaType<BatchResult>();
    qRegisterMetaType<CheckResult>();
}

void Client::updateDiscovery(quint64 generation, const NetworkSnapshot &network,
                             const QList<PresentDevice> &lan, const QList<PresentDevice> &tailnet, bool embedded)
{
    if (generation < generation_)
        return;

    Allowed direct;
    Allowed relayed;
    qint64 owner = 0;
    QString localId;
    QString localAddress;
    if (network.isAuthenticated() && literalUnicast(network.localAddress)) {
        owner = network.owner;
        localId = network.localId;
        localAddress = network.localAddress.toString();

        const auto collect = [&localAddress](Allowed &allowed, const QList<PresentDevice> &devices,
                                              const NetworkSnapshot *network) {
            QSet<QString> conflicts;
            for (const auto &device : devices) {
                if (!device.hosting || device.id.isNull() || !literalUnicast(device.address))
                    continue;
                const auto key = device.address.toString();
                if (key == localAddress || conflicts.contains(key))
                    continue;
                QString peerId;
                if (network) {
                    const auto *peer = findTailnetPeer(network, device.address);
                    if (!peer)
                        continue;
                    peerId = peer->id;
                }
                const auto identity = qMakePair(peerId, device.id);
                const auto entry = allowed.constFind(key);
                if (entry != allowed.cend() && *entry != identity) {
                    allowed.remove(key);
                    conflicts.insert(key);
                } else {
                    allowed.insert(key, identity);
                }
            }
        };
        collect(direct, lan, nullptr);
        collect(embedded ? relayed : direct, tailnet, &network);
    }

    if (generation == generation_ && owner == owner_ && localId == localId_ && localAddress == localAddress_ &&
        embedded == embeddedMode_ && direct == direct_ && relayed == embedded_)
        return;
    generation_ = generation;
    owner_ = owner;
    localId_ = localId;
    localAddress_ = localAddress;
    embeddedMode_ = embedded;
    direct_ = std::move(direct);
    embedded_ = std::move(relayed);
    ++revision_;
    if (batch_)
        finishBatch(batch_, Status::AccessChanged);
    if (check_)
        finishCheck(check_, Status::AccessChanged);
}

bool Client::allowed(const Candidate &candidate) const
{
    if (!owner_ || !port_ || !literalUnicast(candidate.address))
        return false;
    const auto key = candidate.address.toString();
    if (candidate.loopbackRelay) {
        const auto &relay = *candidate.loopbackRelay;
        return embedded_.contains(key) && relay.port && literalUnicast(relay.address) && relay.address.isLoopback();
    }
    return direct_.contains(key);
}

quint64 Client::probeBatch(const QList<Candidate> &candidates, int timeoutMs)
{
    if (batch_)
        return 0;
    auto *batch = new Batch(++nextRequest_, revision_, std::clamp(timeoutMs, 0, MaxBatchTimeoutMs), this);
    batch_ = batch;
    const quint64 request = batch->id;
    QSet<QString> seen;
    for (const auto &candidate : candidates) {
        const auto key = candidate.address.toString();
        if (allowed(candidate) && !seen.contains(key)) {
            batch->pending.enqueue(candidate);
            seen.insert(key);
        }
    }
    connect(&batch->alarm, &QTimer::timeout, batch, [this, batch] { finishBatch(batch, Status::Timeout); });
    batch->alarm.start(static_cast<int>(batch->deadline.remainingTime()));
    QTimer::singleShot(0, batch, [this, batch] { pumpBatch(batch); });
    return request;
}

void Client::pumpBatch(Batch *batch)
{
    if (batch_ != batch)
        return;
    const auto remaining = static_cast<int>(batch->deadline.remainingTime());
    if (!remaining) {
        finishBatch(batch, Status::Timeout);
        return;
    }
    if (batch->pending.isEmpty() && batch->running.isEmpty()) {
        finishBatch(batch, Status::Complete);
        return;
    }

    while (batch->running.size() < BatchConcurrency && !batch->pending.isEmpty()) {
        auto *exchange = new Exchange(batch->pending.dequeue(), port_, std::min(remaining, ProbeTimeoutMs),
                                      [this, batch](Exchange *completed, CheckResult result) {
            if (batch_ != batch)
                return;
            batch->running.remove(completed);
            if (result.current && !batch->deadline.hasExpired())
                batch->hosts.append(std::move(*result.current));
            pumpBatch(batch);
        }, batch);
        batch->running.insert(exchange);
    }
}

void Client::finishBatch(Batch *batch, Status status)
{
    if (batch_ != batch)
        return;
    batch_ = nullptr;
    batch->alarm.stop();
    for (auto *exchange : std::as_const(batch->running))
        exchange->stop();
    auto hosts = std::move(batch->hosts);
    std::sort(hosts.begin(), hosts.end(), [](const StreamHost &first, const StreamHost &second) {
        return endpointOrder(first.endpoint) < endpointOrder(second.endpoint);
    });
    QTimer::singleShot(0, this, [this, id = batch->id, revision = batch->revision, result = BatchResult{status, std::move(hosts)}] {
        if (revision != revision_)
            emit batchFinished(id, {Status::AccessChanged, {}});
        else
            emit batchFinished(id, result);
    });
    batch->deleteLater();
}

quint64 Client::revalidate(const StreamHost &expected, const std::optional<Endpoint> &relay,
                           RouteRequirement requirement, int timeoutMs)
{
    return beginCheck(expected, relay, requirement, false, std::clamp(timeoutMs, 0, ProbeTimeoutMs));
}

quint64 Client::waitForEncoder(const StreamHost &expected, const std::optional<Endpoint> &relay, int timeoutMs)
{
    return beginCheck(expected, relay, RouteRequirement::Stream, true, std::clamp(timeoutMs, 0, EncoderTimeoutMs));
}

quint64 Client::beginCheck(const StreamHost &expected, const std::optional<Endpoint> &relay,
                           RouteRequirement requirement, bool wait, int timeoutMs)
{
    if (check_)
        return 0;
    auto *check = new Check(++nextRequest_, revision_, expected, {expected.endpoint.address, relay},
                            requirement, wait, timeoutMs, this);
    check_ = check;
    const quint64 request = check->id;
    if (expected.endpoint.port != port_ || expected.viaEmbeddedTailnet != relay.has_value() ||
        expected.id.trimmed().isEmpty() || expected.id.size() > 128 || !allowed(check->candidate)) {
        finishCheck(check, Status::RejectedTarget);
        return request;
    }

    connect(&check->alarm, &QTimer::timeout, check, [this, check] { finishCheck(check, Status::Timeout); });
    check->alarm.start(static_cast<int>(check->deadline.remainingTime()));
    QTimer::singleShot(wait ? EncoderIntervalMs : 0, check, [this, check] { pollCheck(check); });
    return request;
}

void Client::pollCheck(Check *check)
{
    if (check_ != check)
        return;
    const auto remaining = static_cast<int>(check->deadline.remainingTime());
    if (!remaining) {
        finishCheck(check, Status::Timeout);
        return;
    }

    check->running = new Exchange(check->candidate, port_, std::min(remaining, ProbeTimeoutMs),
                                  [this, check](Exchange *, CheckResult result) {
        if (check_ != check)
            return;
        check->running = nullptr;
        if (check->deadline.hasExpired()) {
            finishCheck(check, Status::Timeout);
        } else if (result.current && !sameHostIdentity(check->expected.id, result.current->id)) {
            finishCheck(check, Status::IdentityChanged);
        } else if (check->wait && (!result.current || result.current->encoderProbePending)) {
            QTimer::singleShot(EncoderIntervalMs, check, [this, check] { pollCheck(check); });
        } else if (result.current && check->requirement == RouteRequirement::Power && !result.current->supportsPower) {
            finishCheck(check, Status::PowerUnavailable);
        } else {
            finishCheck(check, result.status, result.current);
        }
    }, check);
}

void Client::finishCheck(Check *check, Status status, const std::optional<StreamHost> &current)
{
    if (check_ != check)
        return;
    check_ = nullptr;
    check->alarm.stop();
    if (check->running)
        check->running->stop();
    QTimer::singleShot(0, this, [this, id = check->id, revision = check->revision, result = CheckResult{status, current}] {
        if (revision != revision_)
            emit checkFinished(id, {Status::AccessChanged, {}});
        else
            emit checkFinished(id, result);
    });
    check->deleteLater();
}

void Client::cancel(quint64 request)
{
    if (batch_ && batch_->id == request)
        finishBatch(batch_, Status::Cancelled);
    else if (check_ && check_->id == request)
        finishCheck(check_, Status::Cancelled);
}

} // namespace Aurora::Discovery::ServerInfo
