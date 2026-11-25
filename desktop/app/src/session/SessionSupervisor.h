// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ControlAdapter.h"
#include "MoonlightCommand.h"
#include "ServerInfoClient.h"
#include "StreamSurface.h"
#include "EmbeddedStatusClient.h"

#include <QQueue>
#include <QElapsedTimer>
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
    bool verifiesHostIdentity = false;
};

// The selected host for this prepared attempt. A public route uses the authenticated
// playback endpoint. Other routes carry the discovery check (and any checked relay).
struct SessionControlRoute
{
    Discovery::StreamHost selected;
    std::optional<Discovery::ServerInfo::CheckResult> revalidated;
    std::optional<Discovery::Endpoint> checkedRelay;
};

// The coordinator issues this for a PreparedSession after current-route and
// existing pairing/trust checks succeed for its exact executable and endpoint.
struct SessionAuthorization
{
    QUuid attempt;
    quint64 generation;
    std::optional<SessionControlRoute> controls;
    QByteArray certificateSha256;
    bool resumeOnly = false;
};

struct SessionPlaybackSettings
{
    std::function<QStringList(Stream::PreferenceDefaults)> preferenceArguments;
    QJsonObject sessionPreferences;
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
                        RelayUnavailable, RelayLost, TransportLost, RouteSwitched, AttachmentFailed, SurfaceLost,
                        InvalidLaunch, WorkerStartFailed, WorkerCrashed };

struct SessionResult
{
    SessionEnd reason = SessionEnd::InvalidCandidate;
    std::optional<int> exitCode;
    bool forced = false;
    Stream::CommandError commandError = Stream::CommandError::None;
    aurora::tailscale::StatusError relayError = aurora::tailscale::StatusError::None;
    bool canResumeAfterLoss = false;
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
    Q_PROPERTY(QVariant privacyScreen READ privacyScreen NOTIFY privacyScreenChanged)
    Q_PROPERTY(QString privacyScreenMessage READ privacyScreenMessage NOTIFY privacyScreenChanged)
    Q_PROPERTY(QVariantMap videoSettings READ videoSettings NOTIFY videoSettingsChanged)
    Q_PROPERTY(bool videoSettingsPending READ videoSettingsPending NOTIFY videoSettingsChanged)
    Q_PROPERTY(QString videoSettingsMessage READ videoSettingsMessage NOTIFY videoSettingsChanged)

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
    bool canChangeRoute() const;
    bool requestRouteSwitch(const QUuid &attempt);

    Phase phase() const { return phase_; }
    bool videoReady() const
    {
        return platform_ == Stream::RuntimePlatform::MacOS ? phase_ == Phase::Streaming :
            surface_.videoReady() && (!videoSettingsPending() || phase_ == Phase::Streaming);
    }
    bool fullScreen() const;
    QVariantMap connectionMetrics() const { return metrics_; }
    bool networkPoor() const { return networkPoor_; }
    qint64 networkPoorDurationMs() const
    {
        return networkPoorSince_.isValid() ? networkPoorSince_.elapsed() : 0;
    }
    QVariant privacyScreen() const;
    QString privacyScreenMessage() const;
    QVariantMap videoSettings() const;
    bool videoSettingsPending() const { return videoChange_.has_value(); }
    QString videoSettingsMessage() const { return videoMessage_; }
    // True accepts an asynchronous change. Completion follows encoder confirmation or video attachment.
    bool reconfigureVideo(SessionPlaybackSettings settings);

    Q_INVOKABLE void setViewport(const QRect &viewport, bool suppressed);
    Q_INVOKABLE void setFullScreen(bool enabled);
    Q_INVOKABLE void requestStop();
    Q_INVOKABLE void refreshPrivacyScreen();
    Q_INVOKABLE void setPrivacyScreen(bool enabled);
    Q_INVOKABLE void setPrivacyMode(const QString &mode);
    Q_INVOKABLE void requestVideoSetting(QString option, QString value);
    Q_INVOKABLE void refreshVideoBitrate();

signals:
    void prepared(const Aurora::Session::PreparedSession &session);
    void finished(const QUuid &attempt, quint64 generation, const Aurora::Session::SessionResult &result);
    void routeSwitchDenied(const QUuid &attempt);
    void phaseChanged();
    void fullScreenChanged();
    void connectionChanged();
    void clientPixelSizeChanged(const QSize &size);
    void privacyScreenChanged();
    void videoSettingsChanged();
    void videoSettingsApplied(bool success);
    void videoSettingRequested(QString option, QString value);
    // The preference owner validates the value and handles persistence.
    void preferenceChangeRequested(const QString &option, const QString &value);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class VideoStep { Draining, Applying, Discarding, Restoring, UpdatingBitrate };
    struct VideoChange
    {
        SessionPlaybackSettings requested;
        VideoStep step = VideoStep::Draining;
    };

    void beginPreparation();
    void nextProbe();
    void resolveRoute();
    void publishPrepared();
    void launchPlayback();
    void playbackReady();
    void playbackFailed(SessionResult result);
    void stopPlaybackWorker();
    bool hasWindowsVideo() const;
    bool canKeepApplication() const;
    bool canRestartVideo() const;
    void restartVideo(VideoStep step);
    static SessionPlaybackSettings freezePlayback(SessionPlaybackSettings settings);
    static bool quitsRemoteApp(const QStringList &arguments);
    void launchWorker(const Stream::Command &command, bool probing, bool bindSurface);
    void drainOutput();
    void handleLine(QByteArray line);
    void workerFinished(int code, QProcess::ExitStatus status);
    void retireWorker();
    void beginStop(SessionEnd reason);
    void finish(SessionResult result);
    void completeFinish();
    void setPhase(Phase phase);
    bool matchesControlRoute(const SessionControlRoute &route) const;
    bool canUseSessionControls() const;
    bool canQueryPrivacy() const;
    bool privacyAllowsPlaybackReplacement() const;
    void startPrivacy(Stream::Control::Operation operation);
    void privacyFinished(const QUuid &token, const Stream::Control::Result &result);
    bool startVideoBitrate(Stream::Control::Operation operation, std::optional<Stream::BitrateChange> change = {});
    void videoBitrateFinished(const QUuid &token, const Stream::Control::Result &result);
    void resetVideoBitrate();
    bool sessionControlBusy() const;
    void revokePrivacy();

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
    bool verifiesHostIdentity_ = false;
    bool resumeOnly_ = false;
    bool resumeAfterRelayStop_ = false;
    enum class SwitchState { Unavailable, Ready, Requested };
    SwitchState switchState_ = SwitchState::Unavailable;
    bool discardLine_ = false;
    bool forcedStop_ = false;

    QUuid attempt_;
    Phase phase_ = Phase::Idle;
    SessionEnd stopReason_ = SessionEnd::Cancelled;
    SessionCandidate candidate_;
    Stream::Route resolved_;
    quint64 pendingOpen_ = 0;
    std::shared_ptr<aurora::tailscale::StreamLease> lease_;
    std::unique_ptr<Stream::Control::Adapter> privacyControl_;
    std::optional<SessionControlRoute> controlRoute_;
    bool pinnedControlAvailable_ = false;
    std::optional<Stream::Control::PrivacyState> privacyState_;
    QUuid privacyRequest_;
    std::optional<Stream::Control::PrivacyState> privacyTarget_;
    QString privacyFeedback_;
    std::unique_ptr<Stream::Control::Adapter> bitrateControl_;
    std::optional<Stream::Control::VideoBitrate> liveBitrate_;
    bool bitrateCapabilityReported_ = false;
    QUuid bitrateRequest_;
    std::optional<SessionResult> pendingFinish_;
    SessionView view_;
    std::optional<VideoChange> videoChange_;
    QJsonObject confirmedVideoSettings_;
    QString videoMessage_;
    QMetaObject::Connection ownerStateChanged_;
    QMetaObject::Connection ownerDestroyed_;
    Qt::WindowState previousWindowState_ = Qt::WindowNoState;
    QVariantMap metrics_;
    bool networkPoor_ = false;
    QElapsedTimer networkPoorSince_;
};

} // namespace Aurora::Session

Q_DECLARE_METATYPE(Aurora::Session::PreparedSession)
Q_DECLARE_METATYPE(Aurora::Session::SessionResult)
