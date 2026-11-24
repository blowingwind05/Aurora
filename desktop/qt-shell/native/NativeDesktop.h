// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "preferences/DesktopPreferences.h"
#include "platform/HostSleepPrevention.h"
#include "platform/StartupRegistration.h"
#include "tailscale/EmbeddedStatusClient.h"

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
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
    void readTailnet();
    void publishTailnetStatus(const aurora::tailscale::Status &status, bool refreshing = false);
    void updateDeviceCatalog(const aurora::tailscale::Status &status);
    void refreshPreferences(const QJsonObject &request);

    const QString directory;
    Aurora::Preferences::GeneralPreferences preferences;
    Aurora::Platform::HostSleepPrevention sleepPrevention;
    QVariantMap snapshot;
    QMenu trayMenu;
    QSystemTrayIcon tray;
    std::unique_ptr<QMenuBar> applicationMenu;
    std::unique_ptr<Aurora::Platform::StartupRegistration> startup;
    int preferenceRequestId = 0;
    std::unique_ptr<aurora::tailscale::EmbeddedStatusClient> tailnet;
    aurora::tailscale::Status tailnetStatus;
    std::optional<aurora::tailscale::AccountIdentity> catalogAccount;
    quint64 tailnetGeneration = 0;
    int deviceEpoch = 0;
    bool embeddedSelected = false;
};
