// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MoonlightCommand.h"
#include "ServerInfoClient.h"

#include <QDeadlineTimer>
#include <QProcess>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

namespace aurora::tailscale { class StreamLease; }
namespace Aurora::Stream { class MoonlightProcess; }

namespace Aurora::Stream::Control {

enum class Operation { Restart, Shutdown, PrivacyStatus, PrivacyOn, PrivacyOff };
enum class PrivacyState { Off, On, Unavailable };
enum class Status {
    Accepted, PrivacyReported, PairingRequired, Unconfirmed, InvalidAuthorization,
    RouteChanged, CapabilityUnavailable, RuntimeUnavailable, RelayProbeFailed, StartFailed,
    WorkerCrashed, TimedOut, Cancelled, OutputLimitExceeded
};

struct Result
{
    Status status = Status::InvalidAuthorization;
    std::optional<PrivacyState> privacy;
};

// The caller issues this for one operation after its authorization and matching
// route check. For a relay, checkedRelay is the endpoint used in that check.
struct Authorization
{
    QUuid token;
    Operation operation = Operation::PrivacyStatus;
    QString executable;
    Discovery::StreamHost selected;
    Discovery::ServerInfo::CheckResult revalidated{Discovery::ServerInfo::Status::RejectedTarget, {}};
    std::optional<Discovery::Endpoint> checkedRelay;
    std::shared_ptr<aurora::tailscale::StreamLease> lease;
    std::function<bool(const QUuid &)> stillCurrent;
};

// Operate on the caller's Qt event-loop thread, shared with any stream lease.
class Adapter final : public QObject
{
    Q_OBJECT

public:
    explicit Adapter(QObject *parent = nullptr);
    ~Adapter() override;

    // One operation at a time. True promises a completion while this object lives.
    // stillCurrent is a read-only predicate on this thread. Revoke via cancel(token).
    bool start(Authorization authorization);
    bool cancel(const QUuid &token);
    bool busy() const { return authorization_.has_value(); }

signals:
    void finished(const QUuid &token, const Aurora::Stream::Control::Result &result);

private:
    void begin();
    void launch(const Command &command);
    bool drainOutput();
    void workerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    bool checkCurrent();
    void stop(Status status);
    void finish(Result result);

    QTimer watchdog_{this};
    QDeadlineTimer deadline_;
    QDeadlineTimer relayDeadline_;
    std::optional<Authorization> authorization_;
    std::optional<Status> stopped_;
    std::optional<Command> pairedCommand_;
    MoonlightProcess *worker_ = nullptr;
    QByteArray output_;
    bool probing_ = false;
};

} // namespace Aurora::Stream::Control

Q_DECLARE_METATYPE(Aurora::Stream::Control::Result)
