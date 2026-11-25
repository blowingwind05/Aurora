// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Status.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QTimer>

#include <memory>

namespace aurora::tailscale {

class EmbeddedStatusClient;

// Use and release on the helper client's event-loop thread.
class StreamLease final
{
public:
    ~StreamLease();
    QHostAddress address() const { return QHostAddress::LocalHost; }
    quint16 port() const { return relayPort; }
    bool isValid() const;
    bool close();

private:
    friend class EmbeddedStatusClient;
    StreamLease(EmbeddedStatusClient *client, quint64 token, quint16 port);
    Q_DISABLE_COPY(StreamLease)

    const QPointer<EmbeddedStatusClient> client;
    const quint64 token;
    const quint16 relayPort;
};

// A host lease has no returned port. A dial lease listens only on IPv4 loopback.
class PairingLease final
{
public:
    ~PairingLease();
    QHostAddress address() const { return QHostAddress::LocalHost; }
    std::optional<quint16> localPort() const { return dialPort; }
    bool isValid() const;
    bool close();

private:
    friend class EmbeddedStatusClient;
    PairingLease(EmbeddedStatusClient *client, quint64 token, std::optional<quint16> port);
    Q_DISABLE_COPY(PairingLease)

    const QPointer<EmbeddedStatusClient> client;
    const quint64 token;
    const std::optional<quint16> dialPort;
};

struct PairingLeaseResult
{
    StatusError error = StatusError::InvalidResponse;
    std::shared_ptr<PairingLease> lease;
};

struct PairSourceResult
{
    StatusError error = StatusError::InvalidResponse;
    bool verified = false;
};

struct PresenceResult
{
    StatusError error = StatusError::InvalidResponse;
    QByteArray packet;
};

struct OpenResult
{
    StatusError error = StatusError::InvalidResponse;
    std::shared_ptr<StreamLease> lease;
};

// Own one helper for one state directory, on the coordinator's event-loop thread.
class EmbeddedStatusClient final : public QObject
{
    Q_OBJECT

public:
    EmbeddedStatusClient(QString executable, QString stateDirectory, QObject *parent = nullptr);
    ~EmbeddedStatusClient() override;

    // Windows package root or the macOS bundle's Contents/MacOS directory.
    static QString bundledExecutable(const QString &applicationDirectory);
    bool isAvailable() const;

    // False or zero means nothing was queued. Accepted requests complete asynchronously.
    bool requestStatus(quint64 generation);
    bool requestLogin(quint64 generation);
    bool requestLogout(quint64 generation);

    bool requestHost(quint64 generation, bool enabled);
    bool requestPublish(quint64 generation, bool enabled, std::optional<AccountIdentity> account = {},
                        const QByteArray &packet = {});
    quint64 requestPresence(quint64 generation, const QString &peerId, const QHostAddress &address,
                            const QByteArray &packet);
    quint64 requestOpen(quint64 generation, const QString &peerId, const QHostAddress &address);
    bool cancelOpen(quint64 requestId);
    bool requestClose(const StreamLease &lease);

    quint64 requestPairHost(quint64 generation, const AccountIdentity &account, int localPort, const QByteArray &key);
    quint64 requestPairOpen(quint64 generation, const QString &peerId, const QHostAddress &address, const QByteArray &key);
    // Zero revalidates the authenticated control peer before a client grant.
    // A legacy PIN request also matches the forwarded GameStream source port.
    quint64 requestPairSource(quint64 generation, const AccountIdentity &account, const QString &peerId,
                              const QHostAddress &address, int sourcePort);
    bool cancelPairing(quint64 requestId);
    bool requestClose(const PairingLease &lease);

signals:
    void statusFinished(quint64 generation, const aurora::tailscale::StatusResult &result);
    void loginFinished(quint64 generation, const aurora::tailscale::LoginResult &result);
    void logoutFinished(quint64 generation, aurora::tailscale::StatusError error);
    void hostFinished(quint64 generation, aurora::tailscale::StatusError error);
    void publishFinished(quint64 generation, aurora::tailscale::StatusError error);
    void presenceFinished(quint64 generation, quint64 requestId, const aurora::tailscale::PresenceResult &result);
    void openFinished(quint64 generation, quint64 requestId, const aurora::tailscale::OpenResult &result);
    // The ID is the original open request ID, including canceled allocations.
    void closeFinished(quint64 generation, quint64 openRequestId, aurora::tailscale::StatusError error);
    void pairHostFinished(quint64 generation, quint64 requestId, const aurora::tailscale::PairingLeaseResult &result);
    void pairOpenFinished(quint64 generation, quint64 requestId, const aurora::tailscale::PairingLeaseResult &result);
    void pairSourceFinished(quint64 generation, quint64 requestId, const aurora::tailscale::PairSourceResult &result);
    void pairCloseFinished(quint64 generation, quint64 allocationRequestId, aurora::tailscale::StatusError error);
    void leasesInvalidated();

private:
    friend class StreamLease;
    friend class PairingLease;
    enum class Operation { Status, Login, Logout, Host, Publish, Presence, Open, Close, PairHost, PairOpen, PairSource, PairClose };
    struct PairingContext
    {
        AccountIdentity account;
        QHostAddress localAddress;
        QString peerId;
        QHostAddress peerAddress;
    };
    struct Request
    {
        Operation operation;
        quint64 generation;
        qint64 expires = 0;
        QByteArray params = "{}";
        QByteArray nonce;
        std::optional<PairingContext> pairing;
        quint64 openRequestId = 0;
        bool enabled = false;
        bool sent = false;
        bool cancelled = false;
        bool reportedTimeout = false;
    };
    struct Lease
    {
        quint64 generation;
        QString remoteId;
        bool closing = false;
        std::optional<PairingContext> pairing;
    };
    struct Reply;

    bool matchesAccount(quint64 generation) const;
    bool matchesPeer(quint64 generation, const QString &peerId, const QHostAddress &address) const;
    std::optional<PairingContext> pairingContext(quint64 generation, const QString &peerId = {},
                                                 const QHostAddress &address = {}) const;
    bool matchesPairingContext(quint64 generation, const PairingContext &context) const;
    bool matchesPairingLease(const Lease &lease) const;
    void retireStalePairingLeases();
    bool leaseIsLive(quint64 token, bool auxiliary) const;
    bool closeOwnedLease(quint64 token, bool auxiliary);
    int leaseCount(bool auxiliary) const;
    quint64 startRequest(Request request);
    void enqueueClose(quint64 openRequestId, const Lease &lease);
    void pump();
    void scheduleDeadline();
    void expireRequests();
    void receive();
    Reply decode(const Request &request, const QString &id, const QByteArray &line, const QJsonObject &frame);
    void complete(quint64 id, Reply reply);
    void notify(quint64 id, const Request &completed, Reply reply);
    void fail(StatusError error);
    void invalidateLeases();

    const QString executable;
    const QString stateDirectory;
    QProcess process;
    QTimer dispatch;
    QTimer deadline;
    QElapsedTimer clock;
    QByteArray output;
    QMap<quint64, Request> requests;
    QQueue<Request> closes;
    QHash<quint64, Lease> leases;
    std::optional<Status> verifiedStatus;
    std::optional<PairingContext> verifiedPairingHost;
    quint64 verifiedGeneration = 0;
    quint64 foreground = 0;
    quint64 serial = 0;
    bool stopping = false;
};

} // namespace aurora::tailscale

Q_DECLARE_METATYPE(aurora::tailscale::PresenceResult)
Q_DECLARE_METATYPE(aurora::tailscale::OpenResult)
Q_DECLARE_METATYPE(aurora::tailscale::PairingLeaseResult)
Q_DECLARE_METATYPE(aurora::tailscale::PairSourceResult)
