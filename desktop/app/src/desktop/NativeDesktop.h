// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "NativeDevicePresence.h"
#include "pairing/client/TailnetExchange.h"
#include "pairing/client/ClientGrantCache.h"
#include "pairing/host/EmbeddedPairingHost.h"
#include "preferences/DesktopPreferences.h"
#include "platform/DesktopTray.h"
#include "platform/HostSleepPrevention.h"
#include "platform/StartupRegistration.h"
#include "session/SessionSupervisor.h"
#include "adaptors/sunshine/lifecycle/LifecycleController.h"
#include "adaptors/moonlight/MoonlightProcess.h"
#include "adaptors/tailscale/EmbeddedStatusClient.h"
#include "ui/avatar/AvatarLoader.h"
#include "wallpaper/host/HostWallpaper.h"
#include "wallpaper/client/WallpaperClient.h"
#ifdef Q_OS_WIN
#include "pairing/systemhost/SystemPairingCoordinator.h"
#include "adaptors/tailscale/system/SystemTailscaleClient.h"
#endif

#include <QObject>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QUrl>
#include <QVariantMap>

class QJsonObject;
class QTcpSocket;
class StreamSessionWindow;

class NativeDesktop final : public QObject
{
    Q_OBJECT

public:
    NativeDesktop(const QString &version, const QString &dataDirectory, StreamSessionWindow &streamWindow);
    ~NativeDesktop() override;
    const QVariantMap &state() const { return snapshot; }

    void start(bool packaged);
    void activate(const QString &page = {});
    void requestExit();
    void handleRequest(const QJsonObject &request);

signals:
    void stateChanged(const QVariantMap &next);

private:
    void initializeStartup();
    void initializeTray();
    void initializeHost();
    void refreshHostWallpaper();
    void initializeDeviceWallpapers(const QString &runtime);
    QString cachedDeviceWallpaper(const Aurora::Discovery::Device &device) const;
    void refreshDeviceWallpapers();
    void tryWallpaperRoute();
    void checkWallpaperRoute();
    void releaseWallpaperPreview();
    void finishWallpaperPreview(bool updated);
    bool wallpaperRouteCurrent() const;
    void initializePairingHost();
    void refreshPairingHost(bool confirmed = false);
    void retirePairingHost();
    void scheduleHostGrantExpiration(const QString &session);
    void expireHostClientGrants();
    QString pairingHostConfirmation() const;
    QString pairingHostFeedback() const;
    void readManualPairings();
    void approveManualPairing(const QJsonObject &request);
    bool handleHostAction(const QString &action);
    bool handleSunshineAction(const QString &action, const QJsonObject &request);
    bool readHostSettings(bool reportRefresh = false);
    void publishHostState();
    enum class HostRefresh { Idle, Starting, Reading };
    void finishHostRefresh(const QString &text, bool failed);
    enum class RefreshTarget { Devices, Account };
    struct TailnetRefresh {
        RefreshTarget target;
        quint64 generation;
        std::optional<QDeadlineTimer> deviceDeadline;
    };
    void startTailnetRefresh(RefreshTarget target);
    void finishTailnetRefresh(const QString &text, bool failed);
    void checkDeviceRefresh();
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
    struct TailnetSwitch
    {
        TailnetIntent intent;
        QString name;
        QString login;
        std::optional<QDeadlineTimer> statusDeadline;
        bool failed = false;
        bool verifyLogin = false;
        bool waitingForProviderStatus = false;
    };
    void startTailnetSwitch(const TailnetIntent &intent, const QString &name = {}, const QString &login = {});
    void finishTailnetSwitch(bool confirmed);
    void beginTailnetProviderSwitch();
    void verifyTailnetProfile(TailnetIntent intent);
    void clearTailnetContext();
    bool beginVerifiedTailnetChange(const TailnetIntent &intent);
    void changeTailnetProvider(bool embedded, const aurora::tailscale::StatusResult &verified);
#ifdef Q_OS_WIN
    void readSystemProfiles();
    void receiveSystemProfiles(quint64 generation, const aurora::tailscale::system::ProfilesResult &result);
    void receiveSystemAction(quint64 generation, const aurora::tailscale::system::ActionResult &result);
#endif
    void openTailnetLogin();
    void syncEmbeddedHost();
    void resumeTailnetAfterRetirement();
    void publishTailnetStatus(const aurora::tailscale::Status &status, bool refreshing = false);
    void refreshAccountAvatar(const aurora::tailscale::Status &status);
    void updateDeviceCatalog(const aurora::tailscale::Status &status);
    void refreshPreferences(const QJsonObject &request);
    void initializeStreamSessions(const QString &applicationDirectory);
    void initializeDevicePower(const QString &runtime);
    bool handleDevicePower(const QString &action, const QJsonObject &request);
    void tryPowerRoute();
    void checkPowerRoute();
    void settleDevicePower(const QString &message = {}, bool failed = false);
    bool ownsDevicePower() const;
    bool handleConnectAction(const QString &action, const QJsonObject &request);
    void requestConnectionStatus();
    void connectionAccountReady();
    void prepareNextRoute();
    void initializeRouteQuality();
    void updateRouteQuality();
    void beginRouteQuality();
    bool beginRouteRecovery();
    void beginRecoveryRouteQuality();
    bool probeSessionLanForRouteQuality();
    void routeLanProbed(quint64 request, const QList<QHostAddress> &addresses);
    void startRouteComparison();
    void probeNextRoute();
    void routeProbeFinished(std::optional<Aurora::Stream::RouteQuality> sample);
    bool ownsRouteQuality() const;
    void launchRouteProbe(const Aurora::Stream::Route &route);
    void endRouteQuality(bool comparisonFinished = false);
    bool canMeasureRouteQuality() const;
    void receivePrepared(const Aurora::Session::PreparedSession &session);
    void routeChecked(quint64 request, const Aurora::Discovery::ServerInfo::CheckResult &result);
    void verifyMoonlightTrust();
    void trustCheckFinished(bool trusted);
    void initializePairingClient();
    bool beginPairing();
    bool beginHostIdentity(Aurora::Pairing::RequestKind kind = Aurora::Pairing::RequestKind::HostRoutes);
    void renewClientGrant();
    void scheduleClientGrantRenewal(int delayMs);
    bool canQueryPublicFallback() const;
    bool beginTailscaleExchange(const QHostAddress &peerAddress, Aurora::Pairing::RequestKind kind);
    bool canPairOverRoute(const Aurora::Discovery::StreamHost &route) const;
    void pairingFinished(quint64 request, const Aurora::Pairing::Client::Result &result);
    void launchPreparedSession();
    Aurora::Preferences::StreamPreferences deviceStreamPreferences() const;
    void changeVideoSetting(const QString &option, const QString &value);
    void finishVideoSetting(bool applied);
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
        bool streamingConfirmed = false;
        bool retryRoute = false;
        bool pairingAttempted = false;
        bool clientGrantRequired = false;
        bool clientGranted = false;
        QDeadlineTimer clientGrantDue{QDeadlineTimer::Forever};
        QQueue<Aurora::Discovery::StreamHost> routes;
        std::optional<Aurora::Discovery::StreamHost> pendingRouteSwitch;
        std::optional<Aurora::Discovery::StreamHost> selected;
        std::optional<Aurora::Discovery::StreamHost> preferredLan;
        std::optional<Aurora::Discovery::StreamHost> tailnetFallback;
        std::optional<Aurora::Discovery::StreamHost> preferAfterPairing;
        std::optional<Aurora::Session::PreparedSession> prepared;
        QByteArray certificateSha256;
        QByteArray resumeCertificateSha256;
        QByteArray publicCertificateSha256;
        QList<Aurora::Discovery::StreamHost> publicRoutes;
        bool requestedHostRoutes = false;
        bool lookingForPublicFallback = false;
        bool recoveringRoute = false;
        bool comparedPublicRoutes = false;
        QDeadlineTimer routeSwitchCooldown;
        std::optional<Aurora::Session::SessionControlRoute> controls;
        Aurora::Preferences::StreamPreferenceSnapshot playbackPreferences;
        std::optional<Aurora::Preferences::SessionControlChange> pendingVideoSetting;
        QUuid attempt;
        quint64 probe = 0;
        QString failure;
    };

    const QString directory;
    StreamSessionWindow &sessionWindow;
    Aurora::Preferences::GeneralPreferences preferences;
    Aurora::Preferences::StreamPreferences streamPreferences;
    Aurora::Platform::HostSleepPrevention sleepPrevention;
    QVariantMap snapshot;
    DesktopTray tray;
    std::unique_ptr<Aurora::Platform::StartupRegistration> startup;
    std::unique_ptr<Aurora::Sunshine::LifecycleController> host;
    std::unique_ptr<Aurora::Wallpaper::HostWallpaperUpdater> hostWallpaper;
    bool devicesForeground = false;
    std::unique_ptr<NativeDevicePresence> localPresence;
    QString hostSettingsSession;
    std::optional<Aurora::Sunshine::SettingsSnapshot> hostSettings;
    QJsonObject hostSettingsReceipt;
    QString hostSettingsFeedback;
    QList<Aurora::Sunshine::PendingPairing> manualPairings;
    QString hostPreferenceFeedback;
    HostRefresh hostRefresh = HostRefresh::Idle;
    QString hostRefreshFeedback;
    bool hostRefreshFailed = false;
    int hostRefreshRevision = 0;
    int preferenceRequestId = 0;
    std::unique_ptr<aurora::tailscale::EmbeddedStatusClient> tailnet;
    std::optional<Aurora::Pairing::EmbeddedHostContext> pairingHostContext;
    std::unique_ptr<Aurora::Pairing::EmbeddedPairingHost> embeddedPairingHost;
    QElapsedTimer pairingHostRetry;
    QTimer hostGrantExpirationRetry;
    QString pendingHostGrantExpiration;
    bool hostGrantExpirationInFlight = false;
#ifdef Q_OS_WIN
    std::unique_ptr<Aurora::Pairing::SystemPairingCoordinator> systemPairingHost;
    std::unique_ptr<aurora::tailscale::system::SystemTailscaleClient> systemTailnet;
    QVector<aurora::tailscale::system::Profile> profiles;
    quint64 profilesGeneration = 0;
    std::optional<quint64> profilesRequest;
    std::optional<quint64> systemAction;
    bool wantSystemProfiles = false;
#endif
    aurora::tailscale::Status tailnetStatus;
    std::unique_ptr<Aurora::Ui::AvatarLoader> accountAvatar;
    bool avatarVisible = false;
    QTimer tailnetPoll;
    QTimer tailnetSwitchRetry;
    QTimer deviceRefreshPoll;
    std::optional<TailnetRefresh> manualRefresh;
    QString accountRefreshFeedback;
    bool accountRefreshFailed = false;
    int accountRefreshRevision = 0;
    QString tailnetFeedback;
    QString tailnetNotice;
    int tailnetNoticeVersion = 0;
    int tailnetFocusVersion = 0;
    bool tailnetRuntimeNeedsLogin = false;
    bool tailnetBrowserPending = false;
    bool tailnetSwitchRetryAvailable = false;
    std::optional<TailnetSwitch> tailnetSwitch;
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

    enum class RouteCheckMode { Playing, Recovery };

    struct RouteQualityCheck
    {
        QUuid attempt;
        quint64 generation;
        RouteCheckMode mode = RouteCheckMode::Playing;
        bool testingLan = false;
        bool comparingWeakRoute = false;
        bool testedPublicRoute = false;
        QQueue<Aurora::Discovery::StreamHost> remaining;
        std::optional<Aurora::Discovery::StreamHost> candidate;
        std::optional<Aurora::Discovery::StreamHost> best;
        std::optional<Aurora::Stream::RouteQuality> bestQuality;
        std::optional<Aurora::Stream::RouteQuality> current;
        quint64 lanProbe = 0;
        quint64 opening = 0;
        std::shared_ptr<aurora::tailscale::StreamLease> lease;
        std::unique_ptr<Aurora::Stream::MoonlightProcess> worker;
        QByteArray output;
    };
    QTimer routeQualityDelay;
    QTimer routeQualityDeadline;
    QTimer routeQualityBudget;
    std::optional<RouteQualityCheck> routeQuality;

    struct PairingAttempt
    {
        quint64 generation;
        Aurora::Pairing::RequestKind kind = Aurora::Pairing::RequestKind::Pair;
        quint64 exchange = 0;
        std::optional<QString> cancellation;
        bool renewingGrant = false;
        std::optional<Aurora::Session::SessionResult> interruptedPlayback;
    };
    std::unique_ptr<Aurora::Pairing::Client::TailnetExchange> pairingClient;
    std::optional<PairingAttempt> pairingAttempt;
    QTimer clientGrantRenewal;
    Aurora::Pairing::Client::ClientGrantCache clientGrantCache;
    QTimer connectionStatusRetry;
    std::unique_ptr<Aurora::Stream::MoonlightProcess> trustCheck;
    QTimer trustDeadline;
    struct DevicePowerAttempt
    {
        QUuid token;
        QString deviceId;
        aurora::tailscale::AccountIdentity account;
        int epoch;
        Aurora::Stream::Control::Operation operation;
        QQueue<Aurora::Discovery::StreamHost> routes;
        std::optional<Aurora::Discovery::StreamHost> selected;
        quint64 relayRequest = 0;
        quint64 check = 0;
        std::shared_ptr<aurora::tailscale::StreamLease> relay;
    };
    std::unique_ptr<Aurora::Stream::Control::Adapter> powerAdapter;
    QString powerRuntime;
    std::optional<DevicePowerAttempt> devicePower;
    struct WallpaperAttempt
    {
        QString deviceId;
        QString cachePath;
        int epoch;
        quint64 authorization;
        QQueue<Aurora::Discovery::StreamHost> routes;
        std::optional<Aurora::Discovery::StreamHost> selected;
        quint64 relayRequest = 0;
        quint64 check = 0;
        quint64 fetch = 0;
        std::shared_ptr<aurora::tailscale::StreamLease> relay;
        std::unique_ptr<QObject> leaseLifetime;
    };
    std::unique_ptr<Aurora::Wallpaper::PreviewClient> wallpaperClient;
    QString wallpaperMoonlight;
    QElapsedTimer wallpaperClock;
    QHash<QString, qint64> wallpaperChecked;
    std::optional<WallpaperAttempt> wallpaperAttempt;
    bool exitAfterSession = false;
};
