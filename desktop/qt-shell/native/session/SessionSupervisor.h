// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MoonlightCommand.h"
#include "StreamSurface.h"
#include "EmbeddedStatusClient.h"

#include <QQueue>
#include <QUuid>
#include <QVariantMap>

#include <variant>

namespace Aurora::Stream { class MoonlightProcess; }

namespace Aurora::Session {

struct DirectRoute
{
    QHostAddress address;
    quint16 port = 0;
};

struct EmbeddedRoute
{
    QString peerId;
    QHostAddress address;
};

struct SessionCandidate
{
    quint64 generation = 0;
    QString hostId;
    QString deviceName;
    std::variant<DirectRoute, EmbeddedRoute> route;
};

struct PreparedSession
{
    QUuid attempt;
    quint64 generation = 0;
    QString executable;
    Stream::Route route;
};

// The coordinator issues this for a PreparedSession after current-route and
// existing pairing/trust checks succeed for its exact executable and endpoint.
struct SessionAuthorization
{
    QUuid attempt;
    quint64 generation;
};

struct SessionView
{
    QPointer<QWindow> owner;
    QRect viewport;
    bool startFullScreen = false;
    bool suppressSurface = false;
    std::function<QStringList(Stream::PreferenceDefaults)> preferenceArguments;
    QJsonObject sessionPreferences;
};

enum class SessionEnd { Exited, Cancelled, InvalidCandidate, RuntimeUnavailable, RuntimeProbeFailed,
                        RelayUnavailable, RelayLost, AttachmentFailed, SurfaceLost,
                        InvalidLaunch, WorkerStartFailed, WorkerCrashed };

struct SessionResult
{
    SessionEnd reason = SessionEnd::InvalidCandidate;
    std::optional<int> exitCode;
    bool forced = false;
    Stream::CommandError commandError = Stream::CommandError::None;
    aurora::tailscale::StatusError relayError = aurora::tailscale::StatusError::None;
};

// One attempt at a time, on the GUI thread shared with the tailnet client.
class SessionSupervisor final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Phase phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(bool videoReady READ videoReady NOTIFY phaseChanged)
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)
    Q_PROPERTY(QVariantMap connectionMetrics READ connectionMetrics NOTIFY connectionChanged)
    Q_PROPERTY(bool networkPoor READ networkPoor NOTIFY connectionChanged)

public:
    enum class Phase { Idle, Probing, OpeningRelay, AwaitingAuthorization, Starting,
                       Connecting, Streaming, Standalone, Stopping };
    Q_ENUM(Phase)

    explicit SessionSupervisor(QString bundledExecutable, aurora::tailscale::EmbeddedStatusClient *tailnet = nullptr,
                               QObject *parent = nullptr);
    ~SessionSupervisor() override;

    // A null token means an attempt is already owned by this supervisor.
    QUuid prepare(SessionCandidate candidate);
    // A grant is accepted once, and only for the current prepared attempt.
    bool start(const SessionAuthorization &authorization, SessionView view = {});
    bool cancel(const QUuid &attempt);

    Phase phase() const { return phase_; }
    bool videoReady() const { return surface_.videoReady(); }
    bool fullScreen() const;
    QVariantMap connectionMetrics() const { return metrics_; }
    bool networkPoor() const { return networkPoor_; }

    Q_INVOKABLE void setViewport(const QRect &viewport, bool suppressed);
    Q_INVOKABLE void setFullScreen(bool enabled);
    Q_INVOKABLE void requestStop();

signals:
    void prepared(const Aurora::Session::PreparedSession &session);
    void finished(const QUuid &attempt, quint64 generation, const Aurora::Session::SessionResult &result);
    void phaseChanged();
    void fullScreenChanged();
    void connectionChanged();
    void clientPixelSizeChanged(const QSize &size);
    // The preference owner validates the value and handles persistence.
    void preferenceChangeRequested(const QString &option, const QString &value);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void beginPreparation();
    void nextProbe();
    void resolveRoute();
    void publishPrepared();
    void launchPlayback();
    void launchWorker(const Stream::Command &command, bool probing, bool bindSurface);
    void drainOutput();
    void handleLine(QByteArray line);
    void workerFinished(int code, QProcess::ExitStatus status);
    void retireWorker();
    void beginStop(SessionEnd reason);
    void finish(SessionResult result);
    void setPhase(Phase phase);

    const QString executable_;
    const Stream::RuntimePlatform platform_;
    QPointer<aurora::tailscale::EmbeddedStatusClient> tailnet_;
    StreamSurface surface_{this};
    QTimer startupDeadline_{this};
    QTimer stopDeadline_{this};
    Stream::MoonlightProcess *worker_ = nullptr;
    QQueue<Stream::Probe> probes_;
    Stream::Probe currentProbe_ = Stream::Probe::Runtime;
    QByteArray output_;
    bool probing_ = false;
    bool discardLine_ = false;
    bool forcedStop_ = false;

    QUuid attempt_;
    Phase phase_ = Phase::Idle;
    SessionEnd stopReason_ = SessionEnd::Cancelled;
    SessionCandidate candidate_;
    Stream::Route resolved_;
    quint64 pendingOpen_ = 0;
    std::shared_ptr<aurora::tailscale::StreamLease> lease_;
    SessionView view_;
    QMetaObject::Connection ownerStateChanged_;
    QMetaObject::Connection ownerDestroyed_;
    Qt::WindowState previousWindowState_ = Qt::WindowNoState;
    QVariantMap metrics_;
    bool networkPoor_ = false;
};

} // namespace Aurora::Session

Q_DECLARE_METATYPE(Aurora::Session::PreparedSession)
Q_DECLARE_METATYPE(Aurora::Session::SessionResult)
