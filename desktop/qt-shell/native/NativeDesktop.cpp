// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "discovery/KnownDevices.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenuBar>
#include <QTimer>

namespace {
using Aurora::Preferences::SleepMode;
using Aurora::Platform::HostSleepMode;
using Aurora::Platform::StartupInitialization;
using Aurora::Platform::StartupRegistration;
using Aurora::Platform::StartupResult;
using Aurora::Platform::StartupState;
using aurora::tailscale::BackendState;
using aurora::tailscale::EmbeddedStatusClient;

QString sleepModeName(SleepMode mode)
{
    switch (mode) {
    case SleepMode::OnAc: return QStringLiteral("OnAc");
    case SleepMode::Always: return QStringLiteral("Always");
    case SleepMode::System: return QStringLiteral("System");
    }
    return QStringLiteral("System");
}

void quitDesktop()
{
    auto *application = QCoreApplication::instance();
    QTimer::singleShot(0, application, [] { QCoreApplication::exit(0); });
}
}

NativeDesktop::NativeDesktop(const QString &version, const QString &dataDirectory)
    : directory(dataDirectory), preferences(directory),
      sleepPrevention(Aurora::Platform::parseHostSleepMode(sleepModeName(preferences.sleepMode()))
                          .value_or(HostSleepMode::System)),
      snapshot{{QStringLiteral("applicationVersion"), version},
               {QStringLiteral("deviceAccess"), QStringLiteral("loading")},
               {QStringLiteral("managedWindow"), true},
               {QStringLiteral("windowVisible"), false}}
{
}

NativeDesktop::~NativeDesktop() = default;

void NativeDesktop::start()
{
    initializeStartup();
    initializeTray();
#ifdef Q_OS_MACOS
    applicationMenu = std::make_unique<QMenuBar>();
    auto *application = applicationMenu->addMenu(tr("Aurora"));
    auto *settings = application->addAction(tr("设置…"), this, [this] { activate(QStringLiteral("settings")); });
    settings->setMenuRole(QAction::PreferencesRole);
    settings->setShortcut(QKeySequence::Preferences);
    auto *quit = application->addAction(tr("退出 Aurora"), this, &quitDesktop);
    quit->setMenuRole(QAction::QuitRole);
    quit->setShortcut(QKeySequence::Quit);
#endif
    if (!directory.isEmpty()) {
        QDir applicationDirectory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
        if (applicationDirectory.dirName().compare(QStringLiteral("Moonlight"), Qt::CaseInsensitive) == 0)
            applicationDirectory = QDir(applicationDirectory.absoluteFilePath(QStringLiteral("../..")));
        QFile chosenProvider(QDir(directory).filePath(QStringLiteral("tailnet-provider")));
        embeddedSelected = chosenProvider.open(QIODevice::ReadOnly) &&
            chosenProvider.read(16).trimmed() == QByteArrayLiteral("Embedded");
#elif defined(Q_OS_MACOS)
        embeddedSelected = true;
#endif
        tailnet = std::make_unique<EmbeddedStatusClient>(EmbeddedStatusClient::bundledExecutable(applicationDirectory.path()),
                                                       QDir(directory).filePath(QStringLiteral("Tailnet")));
        connect(tailnet.get(), &EmbeddedStatusClient::statusFinished, this,
                [this](quint64 generation, const aurora::tailscale::StatusResult &result) {
                    if (generation != tailnetGeneration)
                        return;
                    tailnetStatus = result.status.value_or(aurora::tailscale::Status{});
                    publishTailnetStatus(tailnetStatus);
                });
        readTailnet();
    }
    activate();
}

void NativeDesktop::initializeStartup()
{
    const auto launcher = QCoreApplication::applicationFilePath();
    // Register only the executable users can launch directly. During migration
    // the companion binary needs an explicit --native argument.
    if (directory.isEmpty() ||
        QFileInfo(launcher).completeBaseName().compare(QStringLiteral("Aurora"), Qt::CaseInsensitive) != 0)
        return;
    startup = std::make_unique<StartupRegistration>([this] {
        switch (preferences.claimStartupInitialization()) {
        case Aurora::Preferences::StartupClaim::Claimed: return StartupInitialization::Claimed;
        case Aurora::Preferences::StartupClaim::AlreadyInitialized: return StartupInitialization::AlreadyClaimed;
        case Aurora::Preferences::StartupClaim::Failed: return StartupInitialization::Failed;
        }
        return StartupInitialization::Failed;
    }, launcher);
    startup->initializeDefault();
}

void NativeDesktop::readTailnet()
{
    if (!tailnet || !embeddedSelected || !tailnet->isAvailable()) {
        publishTailnetStatus(tailnetStatus);
        return;
    }
    // Reading status starts tsnet. A fresh installation waits for an explicit login.
    if (!QFileInfo(QDir(directory).filePath(QStringLiteral("Tailnet"))).isDir()) {
        tailnetStatus.state = BackendState::Stopped;
        publishTailnetStatus(tailnetStatus);
        return;
    }
    const auto next = tailnetGeneration + 1;
    if (tailnet->requestStatus(next)) {
        tailnetGeneration = next;
        publishTailnetStatus(tailnetStatus, true);
    }
}

void NativeDesktop::publishTailnetStatus(const aurora::tailscale::Status &status, bool refreshing)
{
    QString label;
    QString state;
    switch (status.state) {
    case BackendState::Connected:
        label = tr("已连接"); state = QStringLiteral("Connected");
        break;
    case BackendState::NeedsLogin:
        label = tr("未登录"); state = QStringLiteral("NeedsLogin");
        break;
    case BackendState::NeedsApproval:
        label = tr("等待批准"); state = QStringLiteral("NeedsApproval");
        break;
    case BackendState::Starting:
        label = tr("正在启动"); state = QStringLiteral("Starting");
        break;
    case BackendState::Stopped:
        label = tr("已停止"); state = QStringLiteral("Stopped");
        break;
    case BackendState::Unknown:
        label = tr("状态未知"); state = QStringLiteral("Unknown");
        break;
    }

    const bool refreshAvailable = tailnet && embeddedSelected && tailnet->isAvailable();
    const auto name = status.displayName.isEmpty() ? status.loginName : status.displayName;
    snapshot.insert(QStringLiteral("accountName"), name);
    snapshot.insert(QStringLiteral("accountSubtitle"), status.loginName);
    snapshot.insert(QStringLiteral("refreshAvailable"), refreshAvailable && !refreshing);
    snapshot.insert(QStringLiteral("refreshing"), refreshing);
    snapshot.insert(QStringLiteral("tailscaleAccount"), QVariantMap{
        {QStringLiteral("mode"), embeddedSelected ? QStringLiteral("embedded") : QStringLiteral("system")},
        {QStringLiteral("modeLabel"), embeddedSelected ? tr("内置 Tailscale") : tr("系统 Tailscale")},
        {QStringLiteral("supportsEmbedded"), refreshAvailable},
        {QStringLiteral("state"), state},
        {QStringLiteral("status"), label},
        {QStringLiteral("displayName"), name.isEmpty() ? QStringLiteral("Tailscale") : name},
        {QStringLiteral("loginName"), status.loginName},
        {QStringLiteral("tailnetName"), status.tailnetName},
        {QStringLiteral("localAddress"), status.localAddress.toString()},
        {QStringLiteral("refreshAvailable"), refreshAvailable},
        {QStringLiteral("refreshing"), refreshing},
        {QStringLiteral("busy"), refreshing},
        {QStringLiteral("revision"), QVariant::fromValue(tailnetGeneration)},
    });
    updateDeviceCatalog(status);
    emit stateChanged(snapshot);
}

void NativeDesktop::updateDeviceCatalog(const aurora::tailscale::Status &status)
{
    if (!(catalogAccount == status.account)) {
        catalogAccount = status.account;
        ++deviceEpoch;
    }
    QVariantList rows;
    if (status.account) {
        Aurora::Discovery::NetworkSnapshot network{status.localAddress, status.account->ownerId, status.account->nodeId, {}};
        for (const auto &peer : status.peers) {
            Aurora::Discovery::Device device;
            device.id = peer.nodeId;
            device.name = peer.name;
            device.platform = peer.platform;
            device.address = peer.address;
            device.alternateAddress = peer.alternateAddress;
            device.online = peer.online;
            network.devices.append(device);
        }
        Aurora::Discovery::TailnetSnapshot cached;
        cached.known = Aurora::Discovery::readKnownDevices(directory, network);
        for (const auto &device : Aurora::Discovery::visibleDevices(&network, nullptr, &cached))
            rows.append(Aurora::Discovery::qtDeviceRow(device).toVariantMap());
    }
    snapshot.insert(QStringLiteral("deviceAccess"), status.account ? QStringLiteral("ready")
        : status.state == BackendState::NeedsLogin || status.state == BackendState::Stopped
            ? QStringLiteral("signedOut") : QStringLiteral("loading"));
    snapshot.insert(QStringLiteral("devices"), rows);
    snapshot.insert(QStringLiteral("deviceEpoch"), deviceEpoch);
}

QString NativeDesktop::bundledEntryPoint()
{
    const QDir app(QCoreApplication::applicationDirPath());
    const QString entry = QStringLiteral("QtPreview/desktop/qt-shell/bridge/Live.qml");
#ifdef Q_OS_MACOS
    return app.absoluteFilePath(QStringLiteral("../Resources/") + entry);
#else
    const auto adjacent = app.absoluteFilePath(entry);
    if (QFileInfo::exists(adjacent))
        return adjacent;
    return app.absoluteFilePath(QStringLiteral("../../") + entry);
#endif
}

void NativeDesktop::initializeTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

#ifdef Q_OS_MACOS
    QIcon icon(QStringLiteral(":/aurora/brand/AuroraMenuBarTemplate.png"));
    icon.setIsMask(true);
#else
    const auto icon = QApplication::windowIcon();
#endif
    if (icon.isNull())
        return;

    trayMenu.addAction(tr("打开 Aurora"), this, [this] { activate(); });
    trayMenu.addAction(tr("设置"), this, [this] { activate(QStringLiteral("settings")); });
    trayMenu.addSeparator();
    trayMenu.addAction(tr("退出 Aurora"), this, &quitDesktop);
    tray.setContextMenu(&trayMenu);
    tray.setIcon(icon);
    tray.setToolTip(tr("Aurora 远程桌面"));
#ifdef Q_OS_WIN
    connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            activate();
    });
#endif
    tray.setVisible(preferences.closeToTray());
}

void NativeDesktop::activate(const QString &page)
{
    snapshot.insert(QStringLiteral("windowPage"), page);
    snapshot.insert(QStringLiteral("windowVisible"), true);
    snapshot.insert(QStringLiteral("windowActivation"),
                    snapshot.value(QStringLiteral("windowActivation")).toInt() + 1);
    emit stateChanged(snapshot);
}

void NativeDesktop::handleRequest(const QJsonObject &request)
{
    const auto action = request.value(QStringLiteral("action")).toString();
    if (action == QLatin1String("refresh") || action == QLatin1String("account") ||
        action == QLatin1String("tailscale.enter") || action == QLatin1String("tailscale.refresh")) {
        readTailnet();
        return;
    }
    if (action != QLatin1String("window-close")) {
        refreshPreferences(request);
        return;
    }
    if (tray.isVisible()) {
        snapshot.insert(QStringLiteral("windowVisible"), false);
        emit stateChanged(snapshot);
    } else {
        quitDesktop();
    }
}

void NativeDesktop::refreshPreferences(const QJsonObject &request)
{
    const int id = request.value(QStringLiteral("preferenceRequestId")).toInt();
    if (id <= preferenceRequestId)
        return;
    const auto action = request.value(QStringLiteral("action")).toString();
    const auto value = request.value(QStringLiteral("preferenceValue"));
    const bool trayAvailable = !tray.icon().isNull() && QSystemTrayIcon::isSystemTrayAvailable();
    QString closeFeedback;
    QString startupFeedback;
    QString sleepFeedback;
    if (action == QLatin1String("set-close-to-tray")) {
        if (!trayAvailable || !value.isBool() || !preferences.setCloseToTray(value.toBool()))
            closeFeedback = tr("无法更改关闭窗口的设置。");
    } else if (action == QLatin1String("set-sleep-mode")) {
        const auto platformMode = Aurora::Platform::parseHostSleepMode(value.toString());
        if (platformMode) {
            const auto mode = *platformMode == HostSleepMode::System ? SleepMode::System
                : *platformMode == HostSleepMode::OnAc ? SleepMode::OnAc : SleepMode::Always;
            if (!preferences.setSleepMode(mode))
                sleepFeedback = tr("无法保存自动休眠设置。");
            else
                sleepPrevention.applyMode(*platformMode);
        } else {
            sleepFeedback = tr("无法保存自动休眠设置。");
        }
    } else if (action == QLatin1String("set-startup-enabled")) {
        if (!startup || !value.isBool() || startup->change(value.toBool()) != StartupResult::Ok)
            startupFeedback = tr("无法更改开机自启动。");
    } else if (action != QLatin1String("read-general-settings")) {
        return;
    }

    const auto startupState = startup ? startup->read() : StartupState::Unavailable;
    const bool startupEnabled = startupState == StartupState::On || startupState == StartupState::NeedsApproval ||
        startupState == StartupState::OtherLocation;
    const bool startupAvailable = startupState != StartupState::Unavailable && startupState != StartupState::ReadFailed;
    if (startupFeedback.isEmpty()) {
        switch (startupState) {
        case StartupState::NeedsApproval:
            startupFeedback = tr("请在系统设置的登录项中允许 Aurora。"); break;
        case StartupState::OtherLocation:
            startupFeedback = tr("启动项指向另一个位置的 Aurora。关闭后重新开启可更新路径。"); break;
        case StartupState::Unavailable:
            startupFeedback = tr("此系统或应用包无法登记开机自启动。"); break;
        case StartupState::ReadFailed:
            startupFeedback = tr("无法读取开机自启动状态。"); break;
        default:
            if (startup && startup->initializationFailed())
                startupFeedback = tr("无法登记开机自启动。");
            break;
        }
    }

    preferenceRequestId = id;
    tray.setVisible(trayAvailable && preferences.closeToTray());
    snapshot.insert(QStringLiteral("generalSettings"), QVariantMap{
        {QStringLiteral("acknowledgedRequestId"), id},
        {QStringLiteral("closeToTray"), tray.isVisible()},
        {QStringLiteral("trayAvailable"), trayAvailable},
        {QStringLiteral("startupEnabled"), startupEnabled},
        {QStringLiteral("startupAvailable"), startupAvailable},
        {QStringLiteral("sleepMode"), sleepModeName(preferences.sleepMode())},
        {QStringLiteral("closeFeedback"), closeFeedback},
        {QStringLiteral("startupFeedback"), startupFeedback},
        {QStringLiteral("sleepFeedback"), sleepFeedback},
    });
    emit stateChanged(snapshot);
}
