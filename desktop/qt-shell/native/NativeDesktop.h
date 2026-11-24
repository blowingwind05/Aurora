// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "DesktopTray.h"
#include "NativeDevicePresence.h"
#include "preferences/DesktopPreferences.h"
#include "platform/HostSleepPrevention.h"
#include "platform/StartupRegistration.h"
#include "sunshine/lifecycle/LifecycleController.h"
#include "tailscale/EmbeddedStatusClient.h"
#ifdef Q_OS_WIN
#include "tailscale/system/SystemTailscaleClient.h"
#endif

#include <QObject>
#include <QVariantMap>

class QJsonObject;
class QMenuBar;

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

signals:
    void stateChanged(const QVariantMap &next);

private:
    void initializeStartup();
    void initializeTray();
    void initializeHost();
    bool handleHostAction(const QString &action);
    void publishHostState();
    void requestExit();
    void readTailnet(bool showRefresh = true);
    void receiveTailnetStatus(quint64 generation, const aurora::tailscale::StatusResult &result);
    bool handleTailnetAction(const QString &action, const QJsonObject &request);
    void openTailnetLogin();
    void syncEmbeddedHost();
    void publishTailnetStatus(const aurora::tailscale::Status &status, bool refreshing = false);
    void updateDeviceCatalog(const aurora::tailscale::Status &status);
    void refreshPreferences(const QJsonObject &request);

    const QString directory;
    Aurora::Preferences::GeneralPreferences preferences;
    Aurora::Platform::HostSleepPrevention sleepPrevention;
    QVariantMap snapshot;
    DesktopTray tray;
    std::unique_ptr<QMenuBar> applicationMenu;
    std::unique_ptr<Aurora::Platform::StartupRegistration> startup;
    std::unique_ptr<Aurora::Sunshine::LifecycleController> host;
    std::unique_ptr<NativeDevicePresence> localPresence;
    QString hostPreferenceFeedback;
    int preferenceRequestId = 0;
    std::unique_ptr<aurora::tailscale::EmbeddedStatusClient> tailnet;
#ifdef Q_OS_WIN
    std::unique_ptr<aurora::tailscale::system::SystemTailscaleClient> systemTailnet;
#endif
    aurora::tailscale::Status tailnetStatus;
    QTimer tailnetPoll;
    QString tailnetFeedback;
    std::optional<aurora::tailscale::AccountIdentity> catalogAccount;
    std::optional<aurora::tailscale::AccountIdentity> logoutAccount;
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
    int logoutInvitation = 0;
    bool embeddedSelected = false;
    bool hasSystemRuntime = false;
    bool tailnetBusy = false;
    bool openLoginAfterStatus = false;
};
