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

enum class Operation { Restart, Shutdown, PrivacyStatus, PrivacyOn, PrivacyOff, PrivacyDisplayOff,
                       BitrateStatus, BitrateSet, AppearanceRead, AppearanceSave, AppearanceReset, DisplayControl };
enum class PrivacyState { Off, On, DisplayOff, Unavailable, Unsupported };
enum class Status {
    Accepted, PrivacyReported, BitrateReported, BitrateApplied, BitrateStale, BitrateBusy,
    BitratePending, BitrateExpired, BitrateRejected, BitrateFailed,
    AppearanceReported, DisplayReported, InvalidImage, PairingRequired, Unconfirmed, InvalidAuthorization,
    RouteChanged, CapabilityUnavailable, RuntimeUnavailable, RelayProbeFailed, StartFailed,
    WorkerCrashed, TimedOut, Cancelled, OutputLimitExceeded
};

struct VideoBitrate
{
    QString session;
    int requestedKbps;
    int encoderKbps;
    int maxRequestedKbps;
    quint32 revision;
};

struct Result
{
    Status status = Status::InvalidAuthorization;
    std::optional<PrivacyState> privacy;
    std::optional<VideoBitrate> bitrate;
    QByteArray appearanceImage;
    QByteArray displaySnapshot;
};

// An active, certificate-pinned public playback supplies its authenticated route.
// Other callers supply a discovery check, with its loopback endpoint for a relay.
struct Authorization
{
    QUuid token;
    Operation operation = Operation::PrivacyStatus;
    QString executable;
    Discovery::StreamHost selected;
    std::optional<Discovery::ServerInfo::CheckResult> revalidated;
    std::optional<Discovery::Endpoint> checkedRelay;
    std::shared_ptr<aurora::tailscale::StreamLease> lease;
    std::function<bool(const QUuid &)> stillCurrent;
    std::optional<BitrateChange> bitrateChange;
    QByteArray certificateSha256;
    QString appearanceFile;
    DisplayChange display;
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
