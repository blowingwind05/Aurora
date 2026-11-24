// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "discovery/KnownDevices.h"
#include "tailscale/ProviderSelection.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
using aurora::tailscale::StatusError;

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
#elif defined(Q_OS_MACOS)
        embeddedSelected = true;
#endif
        tailnet = std::make_unique<EmbeddedStatusClient>(EmbeddedStatusClient::bundledExecutable(applicationDirectory.path()),
                                                       QDir(directory).filePath(QStringLiteral("Tailnet")));
        connect(tailnet.get(), &EmbeddedStatusClient::statusFinished, this, &NativeDesktop::receiveTailnetStatus);
#ifdef Q_OS_WIN
        hasSystemRuntime = !aurora::tailscale::system::inspectWindowsInstallation().executable.isEmpty();
        systemTailnet = std::make_unique<aurora::tailscale::system::SystemTailscaleClient>();
        QFile chosenProvider(QDir(directory).filePath(QStringLiteral("tailnet-provider")));
        const auto preference = chosenProvider.open(QIODevice::ReadOnly)
            ? QString::fromLatin1(chosenProvider.read(16)) : QString();
        embeddedSelected = aurora::tailscale::selectWindowsProvider(preference, hasSystemRuntime, tailnet->isAvailable()) ==
            aurora::tailscale::ProviderMode::Embedded;
        connect(systemTailnet.get(), &aurora::tailscale::system::SystemTailscaleClient::statusFinished,
                this, &NativeDesktop::receiveTailnetStatus);
#endif
        connect(tailnet.get(), &EmbeddedStatusClient::loginFinished, this,
                [this](quint64 generation, const aurora::tailscale::LoginResult &result) {
                    if (generation != tailnetGeneration)
                        return;
                    tailnetBusy = false;
                    if (result.error != StatusError::None) {
                        openLoginAfterStatus = false;
                        tailnetFeedback = tr("无法启动 Tailscale 登录，请刷新状态后重试。");
                        publishTailnetStatus(tailnetStatus);
                        return;
                    }
                    readTailnet();
                });
        connect(tailnet.get(), &EmbeddedStatusClient::logoutFinished, this,
                [this](quint64 generation, StatusError error) {
                    if (generation != tailnetGeneration)
                        return;
                    tailnetBusy = false;
                    if (error != StatusError::None)
                        tailnetFeedback = tr("无法退出 Tailscale，请刷新状态后重试。");
                    readTailnet();
                });
        connect(&tailnetPoll, &QTimer::timeout, this, [this] {
            if (snapshot.value(QStringLiteral("windowVisible")).toBool() &&
                (tailnetStatus.state == BackendState::Starting || tailnetStatus.state == BackendState::NeedsLogin ||
                 tailnetStatus.state == BackendState::NeedsApproval))
                readTailnet();
        });
        tailnetPoll.start(3000);
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
    if (tailnetBusy || logoutAccount)
        return;

    const auto next = tailnetGeneration + 1;
    bool accepted = false;
    if (embeddedSelected && tailnet && tailnet->isAvailable()) {
        // Reading status starts tsnet. A fresh installation waits for an explicit login.
        if (QFileInfo(QDir(directory).filePath(QStringLiteral("Tailnet"))).isDir())
            accepted = tailnet->requestStatus(next);
        else
            tailnetStatus.state = BackendState::Stopped;
    }
#ifdef Q_OS_WIN
    else if (!embeddedSelected && systemTailnet)
        accepted = systemTailnet->requestStatus(next);
#endif
    if (accepted) {
        tailnetGeneration = next;
        tailnetBusy = true;
    }
    publishTailnetStatus(tailnetStatus, accepted);
}

void NativeDesktop::receiveTailnetStatus(quint64 generation, const aurora::tailscale::StatusResult &result)
{
    if (generation != tailnetGeneration)
        return;
    tailnetBusy = false;
    tailnetStatus = result.status.value_or(aurora::tailscale::Status{});
    if (openLoginAfterStatus) {
        openLoginAfterStatus = false;
        if (snapshot.value(QStringLiteral("windowVisible")).toBool() && tailnetStatus.state == BackendState::NeedsLogin)
            openTailnetLogin();
    }
    publishTailnetStatus(tailnetStatus);
}

void NativeDesktop::openTailnetLogin()
{
    // The typed status decoder only exposes the official, HTTPS Tailscale login URL.
    if (tailnetStatus.loginUrl.isEmpty())
        tailnetFeedback = tr("Tailscale 登录链接尚未就绪，请稍后重试。");
    else if (QDesktopServices::openUrl(tailnetStatus.loginUrl))
        tailnetFeedback = tr("请在浏览器完成 Tailscale 登录，Aurora 会自动刷新。");
    else
        tailnetFeedback = tr("无法打开 Tailscale 登录链接。");
}

bool NativeDesktop::handleTailnetAction(const QString &action, const QJsonObject &request)
{
    if (action == QLatin1String("refresh") || action == QLatin1String("account") ||
        action == QLatin1String("tailscale.enter") || action == QLatin1String("tailscale.refresh")) {
        if (action == QLatin1String("refresh") || action == QLatin1String("tailscale.refresh"))
            tailnetFeedback.clear();
        readTailnet();
        return true;
    }
    if (!action.startsWith(QLatin1String("tailscale.")))
        return false;
    if (!tailnet || !embeddedSelected || !tailnet->isAvailable() || tailnetBusy ||
        !snapshot.value(QStringLiteral("windowVisible")).toBool())
        return true;

    if (action == QLatin1String("tailscale.confirm") || action == QLatin1String("tailscale.cancel")) {
        if (!logoutAccount || request.value(QStringLiteral("accountTarget")).toString() != QString::number(logoutInvitation))
            return true;
        const auto previous = logoutAccount;
        logoutAccount.reset();
        const auto next = tailnetGeneration + 1;
        if (action == QLatin1String("tailscale.confirm") && previous == tailnetStatus.account &&
            tailnet->requestLogout(next)) {
            tailnetGeneration = next;
            tailnetBusy = true;
            tailnetStatus = {};
            tailnetFeedback.clear();
        }
        publishTailnetStatus(tailnetStatus);
        return true;
    }

    const auto currentRevision = request.value(QStringLiteral("accountRevision")).toInt(-1);
    if (logoutAccount || currentRevision < 0 || quint64(currentRevision) != tailnetGeneration)
        return true;
    if (action == QLatin1String("tailscale.logout") && tailnetStatus.account) {
        logoutAccount = tailnetStatus.account;
        ++logoutInvitation;
        publishTailnetStatus(tailnetStatus);
    } else if (action == QLatin1String("tailscale.primary") &&
               (tailnetStatus.state == BackendState::NeedsLogin || tailnetStatus.state == BackendState::Stopped)) {
        if (tailnetStatus.state == BackendState::NeedsLogin && !tailnetStatus.loginUrl.isEmpty()) {
            openTailnetLogin();
            publishTailnetStatus(tailnetStatus);
        } else {
            const auto next = tailnetGeneration + 1;
            if (tailnet->requestLogin(next)) {
                tailnetGeneration = next;
                tailnetBusy = true;
                openLoginAfterStatus = tailnetStatus.state == BackendState::NeedsLogin;
                tailnetStatus.state = BackendState::Starting;
                tailnetFeedback.clear();
                publishTailnetStatus(tailnetStatus);
            }
        }
    }
    return true;
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

    const bool embeddedAvailable = tailnet && tailnet->isAvailable();
    const bool refreshAvailable = embeddedSelected ? embeddedAvailable : hasSystemRuntime;
    const bool needsLogin = status.state == BackendState::NeedsLogin;
    const bool startAvailable = status.state == BackendState::Stopped;
    const auto primaryAction = needsLogin && embeddedSelected ? tr("登录 Tailscale")
        : startAvailable && embeddedSelected ? tr("启动内置 Tailscale") : QString();
    const QVariant confirmation = logoutAccount ? QVariant(QVariantMap{
        {QStringLiteral("id"), logoutInvitation},
        {QStringLiteral("title"), tr("要退出登录吗？")},
        {QStringLiteral("body"), tr("退出后本机会断开 Tailscale 网络，重新登录后才能连接设备。")},
        {QStringLiteral("label"), tr("退出登录")},
        {QStringLiteral("destructive"), true},
    }) : QVariant{};
    const auto name = status.displayName.isEmpty() ? status.loginName : status.displayName;
    snapshot.insert(QStringLiteral("accountName"), name);
    snapshot.insert(QStringLiteral("accountSubtitle"), status.loginName);
    snapshot.insert(QStringLiteral("refreshAvailable"), refreshAvailable && !tailnetBusy && !logoutAccount);
    snapshot.insert(QStringLiteral("refreshing"), refreshing);
    snapshot.insert(QStringLiteral("tailscaleAccount"), QVariantMap{
        {QStringLiteral("mode"), embeddedSelected ? QStringLiteral("embedded") : QStringLiteral("system")},
        {QStringLiteral("modeLabel"), embeddedSelected ? tr("内置 Tailscale") : tr("系统 Tailscale")},
        {QStringLiteral("supportsEmbedded"), embeddedAvailable},
        {QStringLiteral("hasSystemRuntime"), hasSystemRuntime},
        {QStringLiteral("state"), state},
        {QStringLiteral("status"), label},
        {QStringLiteral("displayName"), name.isEmpty() ? QStringLiteral("Tailscale") : name},
        {QStringLiteral("loginName"), status.loginName},
        {QStringLiteral("tailnetName"), status.tailnetName},
        {QStringLiteral("localAddress"), status.localAddress.toString()},
        {QStringLiteral("refreshAvailable"), refreshAvailable},
        {QStringLiteral("refreshing"), refreshing},
        {QStringLiteral("busy"), tailnetBusy || logoutAccount.has_value()},
        {QStringLiteral("primaryAction"), primaryAction},
        {QStringLiteral("canOpenPrimaryAction"), refreshAvailable && !tailnetBusy && !primaryAction.isEmpty()},
        {QStringLiteral("feedback"), tailnetFeedback},
        {QStringLiteral("confirmation"), confirmation},
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
    if (!tray.initialize())
        return;
    connect(&tray, &DesktopTray::openRequested, this, [this] { activate(); });
    connect(&tray, &DesktopTray::settingsRequested, this, [this] { activate(QStringLiteral("settings")); });
    connect(&tray, &DesktopTray::quitRequested, this, &quitDesktop);
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
    if (handleTailnetAction(action, request))
        return;
    if (action != QLatin1String("window-close")) {
        refreshPreferences(request);
        return;
    }
    openLoginAfterStatus = false;
    logoutAccount.reset();
    if (tray.isVisible()) {
        snapshot.insert(QStringLiteral("windowVisible"), false);
        publishTailnetStatus(tailnetStatus);
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
    const bool trayAvailable = tray.isAvailable();
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
