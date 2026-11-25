// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../PairingWire.h"
#include "../../discovery/Discovery.h"
#include "../../adaptors/moonlight/MoonlightCommand.h"

#include <QObject>
#include <QPointer>

class QIODevice;
class QTcpSocket;

namespace Aurora::Pairing::Client {

inline constexpr int BridgeTimeoutMs = 3000;
inline constexpr int OfferTimeoutMs = 10000;
inline constexpr int OverallTimeoutMs = 38000;

struct Timeouts
{
    int bridgeMs = BridgeTimeoutMs;
    int offerMs = OfferTimeoutMs;
    int overallMs = OverallTimeoutMs;
};

struct Request
{
    quint64 epoch = 0;
    QString peerId;
    Discovery::StreamHost verifiedHost;
    Stream::Route leasedRoute;
    // The route is the GameStream lease for embedded mode or the exact verified endpoint for stdio.
    // The supervisor owns the route lifetime and cancels on explicit revocation.
    QPointer<QObject> leaseLifetime;
    QString moonlightProgram;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    RequestKind kind = RequestKind::Pair;
};

enum class StartError { None, Busy, Unauthorized, InvalidRoute, InvalidTransport, InvalidRuntime };

struct StartResult
{
    quint64 request = 0;
    StartError error = StartError::None;
};

enum class Status {
    Paired, Cancelled, AuthorizationChanged, LeaseClosed, TransportError,
    BridgeTimeout, OfferTimeout, Timeout, Rejected, InvalidOffer,
    RuntimeFailed, TrustMissing, TrustFailed, HostIdentityVerified, ClientGranted, GrantUnsupported
};

struct Result
{
    Status status;
    // From the authenticated host identity reply or a verified post-pair certificate read.
    QByteArray pinnedSha256;
    QList<QHostAddress> publicIpv6;
    bool supportsClientGrant = false;
    QByteArray grantInstance;
    QByteArray clientCertificateSha256;
};

class Coordinator final : public QObject
{
    Q_OBJECT

public:
    explicit Coordinator(QObject *parent = nullptr, Timeouts timeouts = {});
    ~Coordinator() override;

    // Use on the owning event-loop thread. Advance the epoch on account or provider transitions.
    void setAuthorization(quint64 epoch, bool enabled);
    bool busy() const;

    // The socket is already connected to the supervisor's validated loopback pair lease.
    // An accepted operation closes the socket when done. The caller still owns the object.
    StartResult startLoopback(const Discovery::NetworkSnapshot &network, const Request &request,
                              QTcpSocket *socket, quint16 bridgePort, QByteArray bridgeKey);

    // The reader and writer are borrowed, already-open async stdio devices. The supervisor
    // owns their relay process and closes it after completion or destruction.
    StartResult startStdio(const Discovery::NetworkSnapshot &network, const Request &request,
                           QIODevice *reader, QIODevice *writer);

    void cancel(quint64 request);

signals:
    void finished(quint64 request, const Aurora::Pairing::Client::Result &result);

private:
    struct Attempt;

    StartError validate(const Discovery::NetworkSnapshot &network, const Request &request, bool embedded) const;
    StartResult start(const Request &request, QIODevice *reader, QIODevice *writer,
                      QTcpSocket *socket, QByteArray bridgeKey);
    void complete(Attempt *attempt, const Result &result);

    const Timeouts timeouts_;
    Attempt *active_ = nullptr;
    quint64 epoch_ = 0;
    quint64 revision_ = 0;
    quint64 nextRequest_ = 0;
    bool authorized_ = false;
};

} // namespace Aurora::Pairing::Client

Q_DECLARE_METATYPE(Aurora::Pairing::Client::Result)
