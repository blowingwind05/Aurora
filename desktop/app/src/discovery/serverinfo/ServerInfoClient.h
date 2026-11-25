// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ServerInfo.h"

#include <QObject>
#include <QPair>

namespace Aurora::Discovery::ServerInfo {

struct Candidate
{
    QHostAddress address;
    // Keep the authenticated embedded stream lease alive through the operation.
    std::optional<Endpoint> loopbackRelay;
};

enum class Status {
    Complete, RejectedTarget, Unavailable, Timeout, IdentityChanged, PowerUnavailable, Cancelled, AccessChanged
};

struct BatchResult
{
    Status status;
    QList<StreamHost> hosts;
};

struct CheckResult
{
    Status status;
    std::optional<StreamHost> current;
};

// Use on the owning Qt event-loop thread. One batch and one route operation may be active at once.
class Client final : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr, quint16 sunshinePort = BundledSunshinePort);

    // Supply only verified presence from the same current account generation.
    void updateDiscovery(quint64 generation, const NetworkSnapshot &network,
                         const QList<PresentDevice> &lan, const QList<PresentDevice> &tailnet, bool embedded);

    // Request IDs are nonzero and completions are asynchronous. Zero means that lane is busy.
    quint64 probeBatch(const QList<Candidate> &candidates, int timeoutMs = 4000);
    quint64 revalidate(const StreamHost &expected, const std::optional<Endpoint> &loopbackRelay = {},
                       RouteRequirement requirement = RouteRequirement::Stream, int timeoutMs = ProbeTimeoutMs);
    quint64 waitForEncoder(const StreamHost &expected, const std::optional<Endpoint> &loopbackRelay = {},
                           int timeoutMs = EncoderTimeoutMs);
    void cancel(quint64 request);

signals:
    void batchFinished(quint64 request, const Aurora::Discovery::ServerInfo::BatchResult &result);
    void checkFinished(quint64 request, const Aurora::Discovery::ServerInfo::CheckResult &result);

private:
    struct Batch;
    struct Check;
    using Allowed = QHash<QString, QPair<QString, QUuid>>;

    bool allowed(const Candidate &candidate) const;
    void pumpBatch(Batch *batch);
    void finishBatch(Batch *batch, Status status);
    quint64 beginCheck(const StreamHost &expected, const std::optional<Endpoint> &loopbackRelay,
                       RouteRequirement requirement, bool wait, int timeoutMs);
    void pollCheck(Check *check);
    void finishCheck(Check *check, Status status, const std::optional<StreamHost> &current = {});

    const quint16 port_;
    Allowed direct_;
    Allowed embedded_;
    qint64 owner_ = 0;
    QString localId_;
    QString localAddress_;
    bool embeddedMode_ = false;
    quint64 generation_ = 0;
    quint64 revision_ = 0;
    quint64 nextRequest_ = 0;
    Batch *batch_ = nullptr;
    Check *check_ = nullptr;
};

} // namespace Aurora::Discovery::ServerInfo

Q_DECLARE_METATYPE(Aurora::Discovery::ServerInfo::BatchResult)
Q_DECLARE_METATYPE(Aurora::Discovery::ServerInfo::CheckResult)
