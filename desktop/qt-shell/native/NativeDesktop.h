// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "DesktopTray.h"
#include "NativeDevicePresence.h"
#include "preferences/DesktopPreferences.h"
#include "platform/HostSleepPrevention.h"
#include "platform/StartupRegistration.h"
#include "session/SessionSupervisor.h"
#include "sunshine/lifecycle/LifecycleController.h"
#include "stream/MoonlightProcess.h"
#include "tailscale/EmbeddedStatusClient.h"
#ifdef Q_OS_WIN
#include "tailscale/system/SystemTailscaleClient.h"
#endif

#include <QObject>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QUrl>
#include <QVariantMap>

class QJsonObject;
class QMenuBar;
class QQmlEngine;

class NativeDesktop final : public QObject
{
    Q_OBJECT

public:
    NativeDesktop(const QString &version, const QString &dataDirectory);
    ~NativeDesktop() override;
    static QString bundledEntryPoint();
    const QVariantMap &state() const { return snapshot; }

    void start();
    void activate(const QString &page = {});
    void handleRequest(const QJsonObject &request);
    void setSessionWindowSource(QQmlEngine &engine, const QUrl &source);

signals:
    void stateChanged(const QVariantMap &next);

private:
    void initializeStartup();
    void initializeTray();
    void initializeHost();
    bool handleHostAction(const QString &action);
    bool handleSunshineAction(const QString &action, const QJsonObject &request);
    void readHostSettings();
    void publishHostState();
    void requestExit();
    void readTailnet(bool showRefresh = true);
    void receiveTailnetStatus(quint64 generation, const aurora::tailscale::StatusResult &result);
    bool handleTailnetAction(const QString &action, const QJsonObject &request);
    enum class TailnetChange { Logout, Provider, Profile };
    struct TailnetIntent
    {
        TailnetChange change;
        QString target;
        std::optional<aurora::tailscale::AccountIdentity> account;
        int revision;
    };
    void clearTailnetContext();
    bool beginVerifiedTailnetChange(const TailnetIntent &intent);
    void changeTailnetProvider(bool embedded);
#ifdef Q_OS_WIN
    void readSystemProfiles();
    void receiveSystemProfiles(quint64 generation, const aurora::tailscale::system::ProfilesResult &result);
    void receiveSystemAction(quint64 generation, const aurora::tailscale::system::ActionResult &result);
#endif
    void openTailnetLogin();
    void syncEmbeddedHost();
    void resumeTailnetAfterRetirement();
    void publishTailnetStatus(const aurora::tailscale::Status &status, bool refreshing = false);
    void updateDeviceCatalog(const aurora::tailscale::Status &status);
    void refreshPreferences(const QJsonObject &request);
    void initializeStreamSessions(const QString &applicationDirectory);
    bool handleConnectAction(const QString &action, const QJsonObject &request);
    void requestConnectionStatus();
    void connectionAccountReady();
    void prepareNextRoute();
    void receivePrepared(const Aurora::Session::PreparedSession &session);
    void routeChecked(quint64 request, const Aurora::Discovery::ServerInfo::CheckResult &result);
    void verifyMoonlightTrust();
    void trustCheckFinished(bool trusted);
    void launchPreparedSession();
    void sessionFinished(const QUuid &attempt, quint64 generation, const Aurora::Session::SessionResult &result);
    void cancelConnection(const QString &reason = {});
    void failRoute(const QString &reason, bool tryNext);
    bool ownsConnection() const;
    void reportConnection(const QString &message, bool failed);

    struct Connection
    {
        QString peerId;
        QString name;
        aurora::tailscale::AccountIdentity account;
        bool embedded;
        QDeadlineTimer statusDeadline{6000};
        std::optional<quint64> verificationGeneration;
        bool routesVerified = false;
        bool waitingForEncoder = false;
        bool launched = false;
        bool retryRoute = false;
        QQueue<Aurora::Discovery::StreamHost> routes;
        std::optional<Aurora::Discovery::StreamHost> selected;
        std::optional<Aurora::Session::PreparedSession> prepared;
        QUuid attempt;
        quint64 probe = 0;
        QString failure;
    };

    const QString directory;
    Aurora::Preferences::GeneralPreferences preferences;
    Aurora::Platform::HostSleepPrevention sleepPrevention;
    QVariantMap snapshot;
    DesktopTray tray;
    std::unique_ptr<QMenuBar> applicationMenu;
    std::unique_ptr<Aurora::Platform::StartupRegistration> startup;
    std::unique_ptr<Aurora::Sunshine::LifecycleController> host;
    std::unique_ptr<NativeDevicePresence> localPresence;
    QString hostSettingsSession;
    std::optional<Aurora::Sunshine::SettingsSnapshot> hostSettings;
    QJsonObject hostSettingsReceipt;
    QString hostSettingsFeedback;
    QString hostPreferenceFeedback;
    int preferenceRequestId = 0;
    std::unique_ptr<aurora::tailscale::EmbeddedStatusClient> tailnet;
#ifdef Q_OS_WIN
    std::unique_ptr<aurora::tailscale::system::SystemTailscaleClient> systemTailnet;
    QVector<aurora::tailscale::system::Profile> profiles;
    quint64 profilesGeneration = 0;
    std::optional<quint64> profilesRequest;
    std::optional<quint64> systemAction;
    bool wantSystemProfiles = false;
#endif
    aurora::tailscale::Status tailnetStatus;
    QTimer tailnetPoll;
    QString tailnetFeedback;
    std::optional<aurora::tailscale::AccountIdentity> catalogAccount;
    std::optional<TailnetIntent> tailnetConfirmation;
    std::optional<TailnetIntent> tailnetVerification;
    struct EmbeddedAdvertisement {
        std::optional<aurora::tailscale::AccountIdentity> account;
        bool hosting = false;
        bool operator==(const EmbeddedAdvertisement &other) const
        {
            return account == other.account && hosting == other.hosting;
        }
    };
    EmbeddedAdvertisement advertisedHost;
    std::optional<EmbeddedAdvertisement> pendingAdvertisement;
    quint64 tailnetGeneration = 0;
    int deviceEpoch = 0;
    int accountRevision = 1;
    int tailnetInvitation = 0;
    bool embeddedSelected = false;
    bool hasSystemRuntime = false;
    bool tailnetBusy = false;
    bool tailnetRefreshing = false;
    bool openLoginAfterStatus = false;
    bool embeddedHostBound = false;
    bool refreshAfterRetirement = false;
    std::unique_ptr<Aurora::Session::SessionSupervisor> sessionSupervisor;
    std::optional<Connection> connection;
    QTimer connectionStatusRetry;
    QPointer<QQmlEngine> sessionEngine;
    QUrl sessionWindowSource;
    std::unique_ptr<QWindow> sessionWindow;
    QUuid sessionWindowAttempt;
    std::unique_ptr<Aurora::Stream::MoonlightProcess> trustCheck;
    QTimer trustDeadline;
    bool exitAfterSession = false;
};
