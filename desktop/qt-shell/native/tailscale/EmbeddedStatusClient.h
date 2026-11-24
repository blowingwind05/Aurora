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
    void leasesInvalidated();

private:
    friend class StreamLease;
    enum class Operation { Status, Login, Logout, Host, Publish, Presence, Open, Close };
    struct Request
    {
        Operation operation;
        quint64 generation;
        qint64 expires = 0;
        QByteArray params = "{}";
        QByteArray nonce;
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
    };
    struct Reply;

    bool matchesAccount(quint64 generation) const;
    bool matchesPeer(quint64 generation, const QString &peerId, const QHostAddress &address) const;
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
    quint64 verifiedGeneration = 0;
    quint64 foreground = 0;
    quint64 serial = 0;
    bool stopping = false;
};

} // namespace aurora::tailscale

Q_DECLARE_METATYPE(aurora::tailscale::PresenceResult)
Q_DECLARE_METATYPE(aurora::tailscale::OpenResult)
