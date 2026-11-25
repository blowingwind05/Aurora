// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "DesktopTray.h"
#include "NativeDevicePresence.h"
#include "pairing/client/PairingClient.h"
#include "pairing/host/EmbeddedPairingHost.h"
#include "preferences/DesktopPreferences.h"
#include "platform/HostSleepPrevention.h"
#include "platform/StartupRegistration.h"
#include "session/SessionSupervisor.h"
#include "sunshine/lifecycle/LifecycleController.h"
#include "stream/MoonlightProcess.h"
#include "tailscale/EmbeddedStatusClient.h"
#include "tailscale/avatar/AvatarLoader.h"
#include "wallpaper/host/HostWallpaper.h"
#include "wallpaper/client/WallpaperClient.h"
#ifdef Q_OS_WIN
#include "tailscale/system/SystemPairingNc.h"
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
class QTcpSocket;

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
    QString pairingHostConfirmation() const;
    QString pairingHostFeedback() const;
    void readManualPairings();
    void approveManualPairing(const QJsonObject &request);
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
    void refreshAccountAvatar(const aurora::tailscale::Status &status);
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
    void initializePairingClient();
    bool beginPairing();
    bool canPairOverRoute(const Aurora::Discovery::StreamHost &route) const;
#ifdef Q_OS_WIN
    void startSystemPairingExchange(quint64 request, QIODevice *channel);
#endif
    void requestEmbeddedPairing();
    void receivePairingLease(quint64 generation, quint64 request, const aurora::tailscale::PairingLeaseResult &result);
    void startPairingExchange();
    void pairingFinished(quint64 request, const Aurora::Pairing::Client::Result &result);
    void releasePairingTransport();
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
        bool pairingAttempted = false;
        QQueue<Aurora::Discovery::StreamHost> routes;
        std::optional<Aurora::Discovery::StreamHost> selected;
        std::optional<Aurora::Discovery::StreamHost> preferAfterPairing;
        std::optional<Aurora::Session::PreparedSession> prepared;
        QUuid attempt;
        quint64 probe = 0;
        QString failure;
    };

    const QString directory;
    Aurora::Preferences::GeneralPreferences preferences;
    Aurora::Preferences::StreamPreferences streamPreferences;
    Aurora::Platform::HostSleepPrevention sleepPrevention;
    QVariantMap snapshot;
    DesktopTray tray;
    std::unique_ptr<QMenuBar> applicationMenu;
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
    int preferenceRequestId = 0;
    std::unique_ptr<aurora::tailscale::EmbeddedStatusClient> tailnet;
    std::optional<Aurora::Pairing::EmbeddedHostContext> pairingHostContext;
    std::unique_ptr<Aurora::Pairing::EmbeddedPairingHost> embeddedPairingHost;
    QElapsedTimer pairingHostRetry;
#ifdef Q_OS_WIN
    std::unique_ptr<aurora::tailscale::system::SystemTailscaleClient> systemTailnet;
    QVector<aurora::tailscale::system::Profile> profiles;
    quint64 profilesGeneration = 0;
    std::optional<quint64> profilesRequest;
    std::optional<quint64> systemAction;
    bool wantSystemProfiles = false;
#endif
    aurora::tailscale::Status tailnetStatus;
    std::unique_ptr<aurora::tailscale::AvatarLoader> accountAvatar;
    bool avatarVisible = false;
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
    struct PairingAttempt
    {
        quint64 generation;
        QByteArray key;
        QDeadlineTimer connectDeadline{6000};
        quint64 allocation = 0;
        quint64 exchange = 0;
        std::shared_ptr<aurora::tailscale::PairingLease> lease;
        QPointer<QTcpSocket> socket;
        std::optional<QString> cancellation;
#ifdef Q_OS_WIN
        quint64 systemRequest = 0;
#endif
    };
    std::unique_ptr<Aurora::Pairing::Client::Coordinator> pairingClient;
#ifdef Q_OS_WIN
    std::unique_ptr<aurora::tailscale::system::SystemPairingNc> systemPairing;
#endif
    std::optional<PairingAttempt> pairingAttempt;
    QTimer pairingConnectRetry;
    QTimer connectionStatusRetry;
    QPointer<QQmlEngine> sessionEngine;
    QUrl sessionWindowSource;
    std::unique_ptr<QWindow> sessionWindow;
    QUuid sessionWindowAttempt;
    std::unique_ptr<Aurora::Stream::MoonlightProcess> trustCheck;
    QTimer trustDeadline;
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
