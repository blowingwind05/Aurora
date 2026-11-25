// SPDX-License-Identifier: GPL-3.0-only
#include "TailnetExchange.h"

#include "EmbeddedStatusClient.h"
#ifdef Q_OS_WIN
#include "SystemPairingNc.h"
#endif

#include <QDeadlineTimer>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>

#include <array>
#include <utility>

namespace Aurora::Pairing::Client {
namespace {
using aurora::tailscale::EmbeddedStatusClient;
using aurora::tailscale::PairingLease;
using aurora::tailscale::PairingLeaseResult;
using aurora::tailscale::StatusError;
}

struct TailnetExchange::Attempt
{
    quint64 id;
    Request request;
    Discovery::NetworkSnapshot network;
    aurora::tailscale::AccountIdentity account;
    bool embedded;
    StillCurrent stillCurrent;
    QDeadlineTimer deadline;
    QByteArray key;
    quint64 allocation = 0;
    quint64 protocol = 0;
    std::shared_ptr<PairingLease> lease;
    QPointer<QTcpSocket> socket;
#ifdef Q_OS_WIN
    quint64 relay = 0;
#endif
    bool cancelled = false;
};

TailnetExchange::TailnetExchange(EmbeddedStatusClient *tailnet, QObject *parent)
    : QObject(parent), tailnet_(tailnet)
{
    retry_.setInterval(120);
    connect(&retry_, &QTimer::timeout, this, &TailnetExchange::requestEmbeddedChannel);
    if (tailnet_) {
        connect(tailnet_, &EmbeddedStatusClient::pairOpenFinished, this, &TailnetExchange::embeddedChannelOpened);
    }
    connect(&coordinator_, &Coordinator::finished, this, [this](quint64 id, const Result &result) {
        if (attempt_ && id == attempt_->protocol)
            finish(result);
    });
#ifdef Q_OS_WIN
    using aurora::tailscale::system::SystemPairingNc;
    system_ = std::make_unique<SystemPairingNc>();
    connect(system_.get(), &SystemPairingNc::ready, this, &TailnetExchange::startStdio);
    connect(system_.get(), &SystemPairingNc::finished, this, [this](quint64 id, const auto &) {
        if (attempt_ && id == attempt_->relay && !attempt_->protocol)
            finish({Status::TransportError, {}});
    });
#endif
}

TailnetExchange::~TailnetExchange()
{
    release();
}

quint64 TailnetExchange::start(Request request, Discovery::NetworkSnapshot network,
    aurora::tailscale::AccountIdentity account, bool embedded, StillCurrent stillCurrent)
{
    if (attempt_ || !stillCurrent || !stillCurrent() || (embedded && !tailnet_))
        return 0;
#ifndef Q_OS_WIN
    if (!embedded)
        return 0;
#endif
    const auto id = ++nextExchange_;
    const int deadlineMs = request.kind == RequestKind::HostRoutes ? 2500 : 6000;
    attempt_.reset(new Attempt{id, std::move(request), std::move(network), std::move(account), embedded,
        std::move(stillCurrent), QDeadlineTimer(deadlineMs)});
    coordinator_.setAuthorization(attempt_->request.epoch, true);
    QTimer::singleShot(0, this, [this, id] {
        if (attempt_ && attempt_->id == id)
            open();
    });
    return id;
}

void TailnetExchange::open()
{
    if (!current())
        return finish({Status::AuthorizationChanged, {}});
    if (attempt_->embedded) {
        std::array<quint32, BridgeKeySize / sizeof(quint32)> words;
        QRandomGenerator::system()->fillRange(words.data(), words.size());
        attempt_->key = QByteArray(reinterpret_cast<const char *>(words.data()), BridgeKeySize);
        retry_.start();
        requestEmbeddedChannel();
        return;
    }
#ifdef Q_OS_WIN
    const auto &request = attempt_->request;
    system_->setAuthorization(request.epoch, attempt_->account);
    attempt_->relay = system_->start({request.epoch, attempt_->account,
        request.peerId, request.verifiedHost.endpoint.address});
    if (!attempt_->relay)
        finish({Status::TransportError, {}});
#endif
}

void TailnetExchange::requestEmbeddedChannel()
{
    if (!current())
        return finish({Status::AuthorizationChanged, {}});
    if (attempt_->deadline.hasExpired() || !tailnet_)
        return finish({Status::TransportError, {}});
    if (!attempt_->allocation && !attempt_->lease) {
        const auto &request = attempt_->request;
        attempt_->allocation = tailnet_->requestPairOpen(request.epoch, request.peerId,
            request.verifiedHost.endpoint.address, attempt_->key);
    }
}

void TailnetExchange::embeddedChannelOpened(quint64 generation, quint64 id, const PairingLeaseResult &result)
{
    if (!attempt_ || id != attempt_->allocation)
        return;
    attempt_->allocation = 0;
    if (!current() || generation != attempt_->request.epoch)
        return finish({Status::AuthorizationChanged, {}});
    if (result.error != StatusError::None || !result.lease || !result.lease->isValid() || !result.lease->localPort())
        return finish({Status::TransportError, {}});

    attempt_->lease = result.lease;
    auto *socket = new QTcpSocket(this);
    attempt_->socket = socket;
    socket->setProxy(QNetworkProxy::NoProxy);
    connect(socket, &QTcpSocket::connected, this, &TailnetExchange::startLoopback);
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        if (attempt_ && attempt_->socket == socket && !attempt_->protocol)
            finish({Status::TransportError, {}});
    });
    socket->connectToHost(result.lease->address(), *result.lease->localPort());
}

void TailnetExchange::startLoopback()
{
    if (!current())
        return finish({Status::AuthorizationChanged, {}});
    const auto &lease = attempt_->lease;
    if (!lease || !lease->isValid() || !lease->localPort() || attempt_->deadline.hasExpired())
        return finish({Status::LeaseClosed, {}});
    const auto started = coordinator_.startLoopback(attempt_->network, attempt_->request, attempt_->socket,
        *lease->localPort(), std::exchange(attempt_->key, {}));
    attempt_->protocol = started.request;
    if (!started.request)
        return finish({Status::TransportError, {}});
    retry_.stop();
}

#ifdef Q_OS_WIN
void TailnetExchange::startStdio(quint64 relay, QIODevice *channel)
{
    if (!attempt_ || relay != attempt_->relay)
        return;
    if (!current())
        return finish({Status::AuthorizationChanged, {}});
    const auto started = coordinator_.startStdio(attempt_->network, attempt_->request, channel, channel);
    attempt_->protocol = started.request;
    if (!started.request)
        finish({Status::TransportError, {}});
}
#endif

bool TailnetExchange::current() const
{
    return attempt_ && attempt_->request.leaseLifetime && attempt_->stillCurrent();
}

void TailnetExchange::cancel(quint64 id)
{
    if (!attempt_ || attempt_->id != id || attempt_->cancelled)
        return;
    attempt_->cancelled = true;
    if (attempt_->protocol)
        coordinator_.cancel(attempt_->protocol);
    else
        finish({Status::Cancelled, {}});
}

void TailnetExchange::finish(Result result)
{
    if (!attempt_)
        return;
    if (attempt_->cancelled)
        result = {Status::Cancelled, {}};
    else if (!current())
        result = {Status::AuthorizationChanged, {}};
    else if (attempt_->lease && !attempt_->lease->isValid())
        result = {Status::LeaseClosed, {}};
    const auto id = attempt_->id;
    release();
    QTimer::singleShot(0, this, [this, id, result = std::move(result)] { emit finished(id, result); });
}

void TailnetExchange::release()
{
    auto attempt = std::move(attempt_);
    if (!attempt)
        return;
    retry_.stop();
    coordinator_.setAuthorization(attempt->request.epoch, false);
    if (tailnet_ && attempt->allocation)
        tailnet_->cancelPairing(attempt->allocation);
    if (attempt->socket) {
        disconnect(attempt->socket, nullptr, this, nullptr);
        attempt->socket->abort();
        attempt->socket->deleteLater();
    }
    if (attempt->lease)
        attempt->lease->close();
#ifdef Q_OS_WIN
    if (!attempt->embedded)
        system_->setAuthorization(attempt->request.epoch, std::nullopt);
#endif
    attempt->key.fill('\0');
}

} // namespace Aurora::Pairing::Client
