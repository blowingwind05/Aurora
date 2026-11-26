// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "adaptors/moonlight/control/ControlAdapter.h"
#include "preferences/ProviderSelection.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace {
using Aurora::Preferences::SleepMode;
using Aurora::Platform::HostSleepMode;
using Aurora::Platform::StartupInitialization;
using Aurora::Platform::StartupRegistration;
using Aurora::Platform::StartupResult;
using Aurora::Platform::StartupState;
using Aurora::Sunshine::HostLayout;
using Aurora::Sunshine::LifecycleController;
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

QString hostAddress(const QHostAddress &address)
{
    if (address.isNull())
        return {};
    const auto literal = address.toString();
    const auto host = address.protocol() == QAbstractSocket::IPv6Protocol
        ? QLatin1Char('[') + literal + QLatin1Char(']') : literal;
    return host + QLatin1Char(':') + QString::number(Aurora::Discovery::BundledSunshinePort);
}
}

NativeDesktop::NativeDesktop(const QString &version, const QString &dataDirectory, StreamSessionWindow &streamWindow)
    : directory(dataDirectory), sessionWindow(streamWindow), preferences(directory), streamPreferences(directory),
      sleepPrevention(Aurora::Platform::parseHostSleepMode(sleepModeName(preferences.sleepMode()))
                          .value_or(HostSleepMode::System)),
      snapshot{{QStringLiteral("applicationVersion"), version},
               {QStringLiteral("deviceAccess"), QStringLiteral("loading")},
               {QStringLiteral("managedWindow"), true},
               {QStringLiteral("windowVisible"), false}}
{
}

NativeDesktop::~NativeDesktop() = default;

void NativeDesktop::start(bool packaged)
{
    if (packaged)
        initializeStartup();
    initializeTray();
    if (!directory.isEmpty()) {
        accountAvatar = std::make_unique<Aurora::Ui::AvatarLoader>();
        connect(accountAvatar.get(), &Aurora::Ui::AvatarLoader::dataUriChanged, this, [this] {
            snapshot.insert(QStringLiteral("accountAvatar"), accountAvatar->dataUri());
            emit stateChanged(snapshot);
        });
        localPresence = std::make_unique<NativeDevicePresence>(directory);
        connect(localPresence.get(), &NativeDevicePresence::changed, this, [this] {
            updateDeviceCatalog(tailnetStatus);
            emit stateChanged(snapshot);
            refreshDeviceWallpapers();
        });
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
        embeddedSelected = Aurora::Preferences::selectWindowsProvider(preference, hasSystemRuntime, tailnet->isAvailable()) ==
            Aurora::Preferences::ProviderMode::Embedded;
        connect(systemTailnet.get(), &aurora::tailscale::system::SystemTailscaleClient::statusFinished,
                this, &NativeDesktop::receiveTailnetStatus);
        connect(systemTailnet.get(), &aurora::tailscale::system::SystemTailscaleClient::profilesFinished,
                this, &NativeDesktop::receiveSystemProfiles);
        connect(systemTailnet.get(), &aurora::tailscale::system::SystemTailscaleClient::actionFinished,
                this, &NativeDesktop::receiveSystemAction);
#endif
        initializeStreamSessions(applicationDirectory.path());
        connect(tailnet.get(), &EmbeddedStatusClient::loginFinished, this,
                [this](quint64 generation, const aurora::tailscale::LoginResult &result) {
                    if (generation != tailnetGeneration)
                        return;
                    tailnetBusy = false;
                    tailnetRefreshing = false;
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
                    tailnetRefreshing = false;
                    if (error != StatusError::None)
                        tailnetFeedback = tr("无法退出 Tailscale，请刷新状态后重试。");
                    readTailnet();
                });
        connect(tailnet.get(), &EmbeddedStatusClient::hostFinished, this,
                [this](quint64 generation, StatusError error) {
                    if (!pendingAdvertisement)
                        return;
                    const auto next = *pendingAdvertisement;
                    if (error == StatusError::None)
                        embeddedHostBound = next.hosting;
                    if (error != StatusError::None || generation != tailnetGeneration ||
                        !tailnet->requestPublish(generation, next.account.has_value(), next.account,
                            next.account && localPresence ? localPresence->announcementPacket(next.hosting) : QByteArray{})) {
                        pendingAdvertisement.reset();
                        resumeTailnetAfterRetirement();
                    }
                });
        connect(tailnet.get(), &EmbeddedStatusClient::publishFinished, this,
                [this](quint64 generation, StatusError error) {
                    if (!pendingAdvertisement)
                        return;
                    const auto completed = *pendingAdvertisement;
                    pendingAdvertisement.reset();
                    if (error == StatusError::None && generation == tailnetGeneration) {
                        advertisedHost = completed;
                        publishHostState();
                    }
                    resumeTailnetAfterRetirement();
                });
        tailnetSwitchRetry.setSingleShot(true);
        connect(&tailnetSwitchRetry, &QTimer::timeout, this, [this] { readTailnet(false); });
        deviceRefreshPoll.setInterval(80);
        connect(&deviceRefreshPoll, &QTimer::timeout, this, &NativeDesktop::checkDeviceRefresh);
        connect(&tailnetPoll, &QTimer::timeout, this, [this] {
            if (tailnetSwitch || manualRefresh || (connection && !connection->launched))
                return;
            if (localPresence && localPresence->scanning())
                return;
            const bool available = embeddedSelected ? tailnet && tailnet->isAvailable() : hasSystemRuntime;
            if (snapshot.value(QStringLiteral("windowVisible")).toBool() && available &&
                (tailnetStatus.state != BackendState::Stopped || !embeddedSelected))
                readTailnet(false);
        });
        tailnetPoll.start(3000);
        readTailnet();
        initializeHost();
    }
    activate();
}

void NativeDesktop::initializeStartup()
{
    const auto launcher = QCoreApplication::applicationFilePath();
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

void NativeDesktop::initializeHost()
{
    const auto layout = HostLayout::installed();
    if (layout && layout->hasPayload()) {
        host = std::make_unique<LifecycleController>(*layout);
        hostWallpaper = std::make_unique<Aurora::Wallpaper::HostWallpaperUpdater>(layout->profileDirectory());
        connect(host.get(), &LifecycleController::stateChanged, this, &NativeDesktop::publishHostState);
        connect(host.get(), &LifecycleController::shutdownFinished, this, &quitDesktop);
        initializePairingHost();
        if (preferences.managedHostAllowed())
            host->start();
    }
    publishHostState();
}

void NativeDesktop::refreshHostWallpaper()
{
    if (hostWallpaper && devicesForeground && snapshot.value(QStringLiteral("windowVisible")).toBool())
        hostWallpaper->refreshIfAllowed(host->state().phase == LifecycleController::Phase::Ready);
}

void NativeDesktop::publishHostState()
{
    using Phase = LifecycleController::Phase;
    const auto current = host ? host->state() : LifecycleController::State{};
    const bool ready = current.phase == Phase::Ready;
    refreshPairingHost();
    const bool settingsSessionChanged = hostSettingsSession != current.settingsSession;
    if (settingsSessionChanged) {
        hostSettingsSession = current.settingsSession;
        hostSettings.reset();
        hostSettingsReceipt = {};
        hostSettingsFeedback.clear();
        manualPairings.clear();
    }
    const bool allowed = preferences.managedHostAllowed();
    const bool busy = current.phase == Phase::Starting || current.phase == Phase::Restarting || current.phase == Phase::Stopping;
    QString status;
    switch (current.phase) {
    case Phase::Ready: status = QStringLiteral("running"); break;
    case Phase::Starting: status = QStringLiteral("starting"); break;
    case Phase::Restarting: status = QStringLiteral("preparing"); break;
    case Phase::Stopping: status = QStringLiteral("stopping"); break;
    case Phase::Stopped: status = QStringLiteral("disabled"); break;
    case Phase::Failed: status = QStringLiteral("notReady"); break;
    }
    if (hostRefresh == HostRefresh::ApplyingPermissions && !busy)
        finishHostRefresh(ready ? tr("本机被控已重新就绪。")
                               : current.error.isEmpty() ? tr("本机被控未能就绪，请检查 macOS 权限后重试。") : current.error,
                          !ready);
    else if (hostRefresh == HostRefresh::Starting && !busy)
        finishHostRefresh(ready ? tr("检测完成，本机被控正在运行。") : tr("未能启动本机被控，请检查服务状态。"), !ready);
    else if (hostRefresh == HostRefresh::Reading && (!ready || settingsSessionChanged))
        finishHostRefresh(tr("本机被控状态已变化，请重新检测。"), true);

    const auto hint = !hostPreferenceFeedback.isEmpty() ? hostPreferenceFeedback : current.error;
    const bool tailnetReachable = tailnetStatus.account && (!embeddedSelected ||
        (advertisedHost.hosting && advertisedHost.account == tailnetStatus.account));
    const auto tailnetAddress = ready && tailnetReachable ? hostAddress(tailnetStatus.localAddress) : QString();
    const auto lanAddress = ready && localPresence ? hostAddress(localPresence->lan().address) : QString();
    QVariantMap hostState{
        {QStringLiteral("available"), bool(host)},
        {QStringLiteral("managed"), bool(host)},
        {QStringLiteral("allowed"), allowed},
        {QStringLiteral("busy"), busy},
        {QStringLiteral("status"), status},
        {QStringLiteral("toggleAvailable"), bool(host)},
        {QStringLiteral("refreshAvailable"), bool(host) && !busy},
        {QStringLiteral("refreshing"), hostRefresh != HostRefresh::Idle && !hostRefreshForPermissions},
        {QStringLiteral("refreshFeedback"), hostRefreshForPermissions ? QString() : hostRefreshFeedback},
        {QStringLiteral("refreshFailed"), !hostRefreshForPermissions && hostRefreshFailed},
        {QStringLiteral("refreshRevision"), hostRefreshRevision},
        {QStringLiteral("retryAvailable"), bool(host) && allowed && current.phase == Phase::Failed},
        {QStringLiteral("lanAddress"), lanAddress},
        {QStringLiteral("tailscaleAddress"), tailnetAddress},
#ifdef Q_OS_MACOS
        {QStringLiteral("permissionsAvailable"), bool(host) && allowed},
        {QStringLiteral("permissionsRetrying"), hostRefresh == HostRefresh::ApplyingPermissions},
        {QStringLiteral("permissionsFeedback"), hostRefreshForPermissions ? hostRefreshFeedback : QString()},
        {QStringLiteral("permissionsFailed"), hostRefreshForPermissions && hostRefreshFailed},
#endif
        {QStringLiteral("hint"), hint},
    };
    if (ready && !hostSettingsSession.isEmpty()) {
        const bool settingsBusy = host->settingsBusy();
        const auto settingsStatus = hostSettings ? QStringLiteral("ready")
            : settingsBusy ? QStringLiteral("loading")
            : hostSettingsFeedback.isEmpty() ? QStringLiteral("idle") : QStringLiteral("unavailable");
        QVariantList pairingRows;
        for (const auto &pending : manualPairings) {
            const auto name = pending.name.trimmed();
            pairingRows.append(QVariantMap{{QStringLiteral("id"), pending.id},
                {QStringLiteral("name"), name.isEmpty() ? pending.address : name}});
        }
        hostState.insert(QStringLiteral("sunshine"), QVariantMap{
            {QStringLiteral("session"), hostSettingsSession},
            {QStringLiteral("status"), settingsStatus},
            {QStringLiteral("busy"), settingsBusy},
            {QStringLiteral("config"), hostSettings ? hostSettings->config.toVariantMap() : QVariantMap{}},
            {QStringLiteral("result"), hostSettingsReceipt.toVariantMap()},
            {QStringLiteral("feedback"), hostSettingsFeedback},
            {QStringLiteral("pairingFeedback"), pairingHostFeedback()},
            {QStringLiteral("reauthorizeAvailable"), !pairingHostConfirmation().isEmpty()},
            {QStringLiteral("pairingConfirmation"), pairingHostConfirmation()},
            {QStringLiteral("pairingRequests"), pairingRows},
        });
    }
    snapshot.insert(QStringLiteral("host"), hostState);
    sleepPrevention.setHostActive(ready);
    if (localPresence)
        localPresence->setHosting(ready);
    refreshHostWallpaper();
    syncEmbeddedHost();
    emit stateChanged(snapshot);
}

bool NativeDesktop::handleHostAction(const QString &action)
{
    if (!action.startsWith(QLatin1String("host")))
        return false;
    if (!host)
        return true;

    using Phase = LifecycleController::Phase;
    const auto phase = host->state().phase;
    if (phase == Phase::Starting || phase == Phase::Restarting || phase == Phase::Stopping)
        return true;
    hostPreferenceFeedback.clear();
    const bool applyPermissions = action == QLatin1String("hostContinuePermissions");
    if (action == QLatin1String("hostRefresh") || applyPermissions) {
        if (hostRefresh != HostRefresh::Idle)
            return true;
        hostRefreshForPermissions = applyPermissions;
        hostRefreshFeedback.clear();
        hostRefreshFailed = false;
        if (applyPermissions) {
            if (!preferences.managedHostAllowed()) {
                finishHostRefresh(tr("请先启用本机被控。"), true);
            } else if (host->settingsBusy()) {
                finishHostRefresh(tr("Sunshine 正在处理其他操作，请稍后重试。"), true);
            } else {
                // Ready confirms the management endpoint. Relaunch Aurora Host so
                // its startup permission checks run against the updated settings.
                hostRefresh = HostRefresh::ApplyingPermissions;
                const bool accepted = phase == Phase::Ready ? host->restart() : host->start();
                if (!accepted)
                    finishHostRefresh(tr("当前无法重试本机被控，请稍后再试。"), true);
            }
        } else if (phase == Phase::Ready) {
            hostRefresh = HostRefresh::Reading;
            if (!readHostSettings(true))
                finishHostRefresh(tr("Sunshine 正在处理其他操作，请稍后重新检测。"), true);
        } else if (preferences.managedHostAllowed() && (phase == Phase::Stopped || phase == Phase::Failed)) {
            hostRefresh = HostRefresh::Starting;
            if (!host->start())
                finishHostRefresh(tr("当前无法启动本机被控，请稍后重试。"), true);
        } else {
            finishHostRefresh(tr("本机被控尚未启用。"), true);
        }
        publishHostState();
        return true;
    }
    if (action == QLatin1String("hostEnable")) {
        if (!preferences.setManagedHostAllowed(true))
            hostPreferenceFeedback = tr("无法保存允许本机被控的设置。");
        else
            host->start();
    } else if (action == QLatin1String("hostDisable")) {
        if (!preferences.setManagedHostAllowed(false))
            hostPreferenceFeedback = tr("无法保存允许本机被控的设置。");
        host->stop();
    } else if (action == QLatin1String("hostCopyLan") || action == QLatin1String("hostCopyTailnet")) {
        const auto addressField = action == QLatin1String("hostCopyLan") ? QStringLiteral("lanAddress")
            : QStringLiteral("tailscaleAddress");
        const auto address = snapshot.value(QStringLiteral("host")).toMap().value(addressField).toString();
        if (!address.isEmpty() && QGuiApplication::clipboard())
            QGuiApplication::clipboard()->setText(address);
    } else if (action == QLatin1String("hostRetry") && preferences.managedHostAllowed() &&
               (phase == Phase::Stopped || phase == Phase::Failed)) {
        host->start();
    }
#ifdef Q_OS_MACOS
    else if (action == QLatin1String("hostOpenPermissions") && preferences.managedHostAllowed()) {
        if (!QDesktopServices::openUrl(QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy")))
            && hostRefresh == HostRefresh::Idle) {
            hostRefreshForPermissions = true;
            finishHostRefresh(tr("无法打开 macOS 隐私设置。"), true);
        }
    }
#endif
    publishHostState();
    return true;
}

void NativeDesktop::finishHostRefresh(const QString &text, bool failed)
{
    hostRefresh = HostRefresh::Idle;
    hostRefreshFeedback = text;
    hostRefreshFailed = failed;
    ++hostRefreshRevision;
}

bool NativeDesktop::readHostSettings(bool reportRefresh)
{
    if (!host || host->state().phase != LifecycleController::Phase::Ready)
        return false;
    if (host->readSettings([this, reportRefresh](Aurora::Sunshine::SettingsReadResult result) {
            if (!host || result.session != host->state().settingsSession || result.session != hostSettingsSession)
                return;
            const bool read = result.snapshot.has_value();
            if (read)
                hostSettings = std::move(result.snapshot);
            if (!result.error.isEmpty())
                hostSettingsFeedback = result.error;
            if (reportRefresh && hostRefresh == HostRefresh::Reading) {
                finishHostRefresh(read ? tr("检测完成，本机被控正在运行。")
                                       : tr("未能确认 Sunshine 状态，请重新检测。"), !read);
            }
            publishHostState();
            readManualPairings();
        })) {
        hostSettingsFeedback.clear();
        publishHostState();
        return true;
    }
    return false;
}

bool NativeDesktop::handleSunshineAction(const QString &action, const QJsonObject &request)
{
    if (!action.startsWith(QLatin1String("sunshine")))
        return false;
    if (!host || host->state().phase != LifecycleController::Phase::Ready || hostSettingsSession.isEmpty())
        return true;
    const auto message = request.value(QStringLiteral("sunshine")).toObject();
    if (message.value(QStringLiteral("session")).toString() != hostSettingsSession)
        return true;
    if (action == QLatin1String("sunshineLoad")) {
        readHostSettings();
    } else if (action == QLatin1String("sunshinePairingRefresh")) {
        readManualPairings();
    } else if (action == QLatin1String("sunshinePair")) {
        approveManualPairing(message);
    } else if (action == QLatin1String("sunshineReauthorize")) {
        const auto confirmation = pairingHostConfirmation();
        if (!confirmation.isEmpty() && message.value(QStringLiteral("pairingConfirmation")).toString() == confirmation &&
            !tailnetBusy && !tailnetConfirmation && !tailnetVerification) {
            refreshPairingHost(true);
            publishHostState();
        }
    } else if (action == QLatin1String("sunshineRestart") && !host->settingsBusy()) {
        host->restart();
    } else if (action == QLatin1String("sunshineSave")) {
        const auto id = message.value(QStringLiteral("id")).toInt();
        const auto changes = message.value(QStringLiteral("changes")).toObject();
        if (id <= 0 || changes.isEmpty() || changes.size() > 110)
            return true;
        if (!hostSettings || !host->saveSettings({hostSettings->version, id, changes},
                [this](Aurora::Sunshine::SettingsSaveResult result) {
                    if (!host || result.submitted.session != host->state().settingsSession ||
                        result.submitted.session != hostSettingsSession)
                        return;
                    if (result.snapshot)
                        hostSettings = std::move(result.snapshot);
                    hostSettingsReceipt = result.receiptForQml();
                    hostSettingsFeedback = result.error;
                    publishHostState();
                    if (result.refreshRequired)
                        readHostSettings();
                })) {
            Aurora::Sunshine::SettingsSaveResult busy;
            busy.id = id;
            for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
                busy.errors.insert(it.key(), tr("Sunshine 正在处理另一项操作。请稍后重试保存。"));
            hostSettingsReceipt = busy.receiptForQml();
        } else {
            hostSettingsReceipt = {};
            hostSettingsFeedback.clear();
        }
        publishHostState();
    }
    return true;
}

void NativeDesktop::requestExit()
{
    settleDevicePower();
    devicesForeground = false;
    refreshDeviceWallpapers();
    retirePairingHost();
    if ((sessionSupervisor && sessionSupervisor->phase() != Aurora::Session::SessionSupervisor::Phase::Idle) ||
        (pairingAttempt && pairingAttempt->exchange)) {
        exitAfterSession = true;
        cancelConnection();
        return;
    }
    exitAfterSession = false;
    cancelConnection();
    if (host)
        host->shutdown();
    else
        quitDesktop();
}

void NativeDesktop::startTailnetRefresh(RefreshTarget target)
{
    if (manualRefresh || tailnetSwitch || tailnetConfirmation || tailnetVerification ||
        (tailnetBusy && tailnetRefreshing))
        return;
    manualRefresh = TailnetRefresh{target, tailnetBusy ? tailnetGeneration : tailnetGeneration + 1, std::nullopt};
    if (target == RefreshTarget::Devices) {
        snapshot.insert(QStringLiteral("refreshFeedback"), QString{});
        snapshot.insert(QStringLiteral("refreshFailed"), false);
        snapshot.insert(QStringLiteral("deviceRefreshResult"), false);
    } else {
        accountRefreshFeedback.clear();
        accountRefreshFailed = false;
    }
    if (tailnetBusy)
        publishTailnetStatus(tailnetStatus);
    else
        readTailnet(false);
}

void NativeDesktop::finishTailnetRefresh(const QString &text, bool failed)
{
    const auto target = manualRefresh->target;
    manualRefresh.reset();
    deviceRefreshPoll.stop();
    if (target == RefreshTarget::Devices) {
        snapshot.insert(QStringLiteral("refreshFeedback"), text);
        snapshot.insert(QStringLiteral("refreshFailed"), failed);
        snapshot.insert(QStringLiteral("deviceRefreshResult"), true);
        snapshot.insert(QStringLiteral("refreshRevision"), snapshot.value(QStringLiteral("refreshRevision")).toInt() + 1);
    } else {
        accountRefreshFeedback = text;
        accountRefreshFailed = failed;
        ++accountRefreshRevision;
    }
}

void NativeDesktop::checkDeviceRefresh()
{
    if (!manualRefresh || !manualRefresh->deviceDeadline)
        return;
    if (!tailnetStatus.account || tailnetSwitch) {
        finishTailnetRefresh(tr("Tailscale 账号已变化，请重新刷新设备列表。"), true);
    } else if (localPresence && localPresence->scanning()) {
        if (!manualRefresh->deviceDeadline->hasExpired())
            return;
        finishTailnetRefresh(tr("刷新设备列表超时，请重试。"), true);
    } else if (connection) {
        finishTailnetRefresh(tr("网络状态已刷新，结束远控后会重新发现设备。"), false);
    } else {
        finishTailnetRefresh(tr("设备列表已刷新。"), false);
    }
    publishTailnetStatus(tailnetStatus);
}

void NativeDesktop::readTailnet(bool showRefresh)
{
    if (tailnetBusy || tailnetConfirmation)
        return;

    const auto next = tailnetGeneration + 1;
    bool accepted = false;
    if (embeddedSelected && tailnet && tailnet->isAvailable()) {
        // Reading status starts tsnet. A fresh installation waits for an explicit login.
        if (QFileInfo(QDir(directory).filePath(QStringLiteral("Tailnet"))).isDir())
            accepted = tailnet->requestStatus(next);
        else {
            tailnetStatus.state = BackendState::Stopped;
            if (tailnetSwitch && tailnetSwitch->statusDeadline)
                finishTailnetSwitch(true);
        }
    }
#ifdef Q_OS_WIN
    else if (!embeddedSelected && systemTailnet)
        accepted = systemTailnet->requestStatus(next);
#endif
    if (accepted) {
        tailnetGeneration = next;
        tailnetBusy = true;
        tailnetRefreshing = showRefresh;
    } else if (manualRefresh && manualRefresh->generation == next && !manualRefresh->deviceDeadline) {
        const bool stopped = embeddedSelected && tailnetStatus.state == BackendState::Stopped;
        finishTailnetRefresh(stopped ? tr("内置 Tailscale 尚未启动。") : tr("无法读取 Tailscale 状态，请重试。"), true);
    } else if (tailnetVerification) {
        tailnetVerification.reset();
        tailnetFeedback = tr("无法重新检查 Tailscale 账号，请刷新后重试。");
        finishTailnetSwitch(false);
    } else if (tailnetSwitch && tailnetSwitch->statusDeadline) {
        if (tailnetSwitch->statusDeadline->hasExpired()) {
            tailnetFeedback = tr("暂时无法确认 Tailscale 状态，请重试。");
            finishTailnetSwitch(false);
        } else {
            tailnetSwitchRetry.start(250);
        }
    }
    publishTailnetStatus(tailnetStatus, accepted && showRefresh);
}

void NativeDesktop::receiveTailnetStatus(quint64 generation, const aurora::tailscale::StatusResult &result)
{
    // The pairing listener also refreshes identity using this helper. Only the
    // desktop's pending status request may complete its refresh or account action.
    if (generation != tailnetGeneration || !tailnetBusy)
        return;
    tailnetBusy = false;
    tailnetRefreshing = false;
    const auto fresh = result.status.value_or(aurora::tailscale::Status{});
    const auto verifying = std::exchange(tailnetVerification, std::nullopt);
    if (tailnetSwitch && tailnetSwitch->waitingForProviderStatus) {
        beginTailnetProviderSwitch();
        return;
    }
    if (verifying && verifying->change == TailnetChange::Provider) {
        const bool embedded = verifying->target == QLatin1String("embedded");
        if (result.error == StatusError::None && result.status && fresh.state != BackendState::Unknown)
            changeTailnetProvider(embedded, result);
        else {
            tailnetFeedback = embedded ? tr("无法读取内置 Tailscale 状态，请重试。")
                                       : tr("无法读取系统 Tailscale 状态，请重试。");
            finishTailnetSwitch(false);
            publishTailnetStatus(tailnetStatus);
        }
        return;
    }
    const bool completingSwitch = tailnetSwitch && tailnetSwitch->statusDeadline && !verifying;
    if (completingSwitch) {
        const auto &pending = *tailnetSwitch;
        const bool staleProfile = pending.intent.change == TailnetChange::Profile && !pending.failed &&
            fresh.state == BackendState::Connected &&
            (!fresh.account || fresh.account == pending.intent.account ||
             (pending.verifyLogin && !fresh.loginName.isEmpty() &&
              pending.login.compare(fresh.loginName, Qt::CaseInsensitive) != 0));
        const bool unsettled = result.error != StatusError::None || fresh.state == BackendState::Unknown ||
            fresh.state == BackendState::Starting || staleProfile;
        if (unsettled && !pending.statusDeadline->hasExpired()) {
            tailnetSwitchRetry.start(250);
            publishTailnetStatus(tailnetStatus);
            return;
        }
        if (unsettled) {
            tailnetFeedback = tr("暂时无法确认 Tailscale 状态，请重试。");
#ifdef Q_OS_WIN
            profiles.clear();
            profilesGeneration = 0;
            wantSystemProfiles = !embeddedSelected;
#endif
            finishTailnetSwitch(false);
            publishTailnetStatus(tailnetStatus);
            return;
        }
    }
    const bool contextChanged = !(tailnetStatus.account == fresh.account) || tailnetStatus.state != fresh.state;
    tailnetStatus = fresh;
    if (fresh.state == BackendState::Connected || fresh.state == BackendState::NeedsApproval ||
        fresh.state == BackendState::Stopped) {
        tailnetRuntimeNeedsLogin = false;
        tailnetBrowserPending = false;
    }
    if (contextChanged) {
        ++accountRevision;
#ifdef Q_OS_WIN
        if (!completingSwitch || !tailnetSwitch || tailnetSwitch->intent.change != TailnetChange::Profile ||
            tailnetSwitch->failed) {
            wantSystemProfiles = wantSystemProfiles || (!embeddedSelected && !profiles.isEmpty());
            profiles.clear();
            profilesGeneration = 0;
        }
#endif
    }
    if (verifying) {
        if (result.error == StatusError::None && verifying->account == tailnetStatus.account &&
            beginVerifiedTailnetChange(*verifying))
            return;
        tailnetFeedback = tr("Tailscale 账号状态已变化，或暂时无法完成操作。请刷新后重试。");
        finishTailnetSwitch(false);
    }
    if (completingSwitch && tailnetSwitch) {
        const bool provider = tailnetSwitch->intent.change == TailnetChange::Provider;
        const bool requiresLogin = fresh.state == BackendState::NeedsLogin;
        if (!tailnetSwitch->failed && requiresLogin) {
            tailnetRuntimeNeedsLogin = provider;
            if (!provider)
                tailnetFeedback = tr("此账号需要重新登录。");
            ++tailnetFocusVersion;
        }
        finishTailnetSwitch(!tailnetSwitch->failed && !requiresLogin &&
            (provider || fresh.state == BackendState::Connected));
        if (requiresLogin)
            tailnetSwitchRetryAvailable = false;
    }
    if (localPresence) {
        const bool visible = snapshot.value(QStringLiteral("windowVisible")).toBool() && !connection;
        localPresence->update(tailnetStatus, embeddedSelected, visible);
        if (visible && embeddedSelected && tailnet && tailnetStatus.account)
            localPresence->scanEmbedded(*tailnet, tailnetStatus, generation);
    }
    if (manualRefresh && manualRefresh->generation == generation && !manualRefresh->deviceDeadline) {
        if (result.error != StatusError::None || !result.status || fresh.state == BackendState::Unknown) {
            finishTailnetRefresh(tr("无法读取 Tailscale 状态，请重试。"), true);
        } else if (fresh.state != BackendState::Connected) {
            const auto message = fresh.state == BackendState::NeedsLogin ? tr("Tailscale 尚未登录。")
                : fresh.state == BackendState::NeedsApproval ? tr("Tailscale 正在等待批准。")
                : fresh.state == BackendState::Stopped ? tr("Tailscale 尚未连接。")
                : tr("Tailscale 正在启动。");
            finishTailnetRefresh(message, true);
        } else if (!fresh.account) {
            finishTailnetRefresh(tr("Tailscale 已连接，但未能确认此设备身份。"), true);
        } else if (manualRefresh->target == RefreshTarget::Account) {
            finishTailnetRefresh(tr("刷新完成，Tailscale 已连接。"), false);
        } else {
            manualRefresh->deviceDeadline.emplace(10000);
            deviceRefreshPoll.start();
        }
    }
    if (openLoginAfterStatus) {
        openLoginAfterStatus = false;
        if (snapshot.value(QStringLiteral("windowVisible")).toBool() && tailnetStatus.state == BackendState::NeedsLogin)
            openTailnetLogin();
    }
    connectionAccountReady();
    publishTailnetStatus(tailnetStatus);
    publishHostState();
    refreshDeviceWallpapers();
#ifdef Q_OS_WIN
    if (wantSystemProfiles)
        readSystemProfiles();
#endif
}

void NativeDesktop::syncEmbeddedHost()
{
    if (!tailnet || !localPresence || pendingAdvertisement || tailnetBusy || (connection && !connection->launched))
        return;
    EmbeddedAdvertisement next;
    if (embeddedSelected && tailnetStatus.account) {
        next.account = tailnetStatus.account;
        next.hosting = host && host->state().phase == LifecycleController::Phase::Ready;
    }
    if (next == advertisedHost && embeddedHostBound == next.hosting)
        return;
    if (tailnet->requestHost(tailnetGeneration, next.hosting))
        pendingAdvertisement = next;
}

void NativeDesktop::resumeTailnetAfterRetirement()
{
    if (refreshAfterRetirement && !pendingAdvertisement) {
        refreshAfterRetirement = false;
        readTailnet();
    }
}

void NativeDesktop::openTailnetLogin()
{
    // The typed status decoder only exposes the official, HTTPS Tailscale login URL.
    tailnetBrowserPending = false;
    if (tailnetStatus.loginUrl.isEmpty()) {
        tailnetFeedback = tr("Tailscale 登录链接尚未就绪，请稍后重试。");
    } else if (QDesktopServices::openUrl(tailnetStatus.loginUrl)) {
        tailnetBrowserPending = true;
        tailnetFeedback.clear();
    } else {
        tailnetFeedback = tr("无法打开 Tailscale 登录链接。");
    }
}

void NativeDesktop::startTailnetSwitch(const TailnetIntent &intent, const QString &name, const QString &login)
{
    tailnetFeedback.clear();
    tailnetNotice.clear();
    tailnetRuntimeNeedsLogin = false;
    tailnetSwitchRetryAvailable = false;
    tailnetSwitch = TailnetSwitch{intent, name, login, std::nullopt};
}

void NativeDesktop::finishTailnetSwitch(bool confirmed)
{
    if (!tailnetSwitch)
        return;
    tailnetSwitchRetry.stop();
    const auto completed = *std::exchange(tailnetSwitch, std::nullopt);
    if (!confirmed) {
        tailnetSwitchRetryAvailable = !tailnetFeedback.isEmpty();
        return;
    }

    const auto name = tailnetStatus.displayName.isEmpty() ? tailnetStatus.loginName : tailnetStatus.displayName;
    if (completed.intent.change == TailnetChange::Provider)
        tailnetNotice = embeddedSelected ? tr("已切换至内置 Tailscale") : tr("已切换至系统 Tailscale");
    else
        tailnetNotice = tr("已切换至 %1").arg(name.isEmpty() ? completed.name : name);
    ++tailnetNoticeVersion;
}

void NativeDesktop::beginTailnetProviderSwitch()
{
    auto &pending = *tailnetSwitch;
    pending.waitingForProviderStatus = tailnetBusy;
    if (pending.waitingForProviderStatus) {
        publishTailnetStatus(tailnetStatus);
        return;
    }
    const auto generation = tailnetGeneration + 1;
    const bool embedded = pending.intent.target == QLatin1String("embedded");
    bool accepted = embedded && tailnet && tailnet->requestStatus(generation);
#ifdef Q_OS_WIN
    if (!embedded && systemTailnet)
        accepted = systemTailnet->requestStatus(generation);
#endif
    if (accepted) {
        tailnetVerification = pending.intent;
        tailnetGeneration = generation;
        tailnetBusy = true;
        tailnetRefreshing = true;
        publishTailnetStatus(tailnetStatus);
    } else {
        tailnetFeedback = tr("无法检查所选 Tailscale 运行方式，请重试。");
        finishTailnetSwitch(false);
        publishTailnetStatus(tailnetStatus);
    }
}

void NativeDesktop::verifyTailnetProfile(TailnetIntent intent)
{
#ifdef Q_OS_WIN
    const auto selected = std::find_if(profiles.cbegin(), profiles.cend(), [&intent](const auto &profile) {
        return profile.id == intent.target && !profile.isCurrent;
    });
    if (selected == profiles.cend()) {
        tailnetFeedback = tr("所选账号已变化，请刷新后重试。");
        publishTailnetStatus(tailnetStatus);
        return;
    }
    startTailnetSwitch(intent, selected->name.isEmpty() ? selected->loginName : selected->name, selected->loginName);
    tailnetSwitch->verifyLogin = selected->loginNameAuthoritative;
    tailnetVerification = std::move(intent);
    // An in-flight background status read can also verify the selected account revision.
    if (tailnetBusy)
        publishTailnetStatus(tailnetStatus);
    else
        readTailnet(false);
#else
    Q_UNUSED(intent);
#endif
}

void NativeDesktop::clearTailnetContext()
{
    clientGrantCache.clear();
    settleDevicePower();
    retirePairingHost();
    cancelConnection(tr("账号已变化，连接已停止。"));
    tailnetStatus = {};
    tailnetBrowserPending = false;
    ++accountRevision;
    ++deviceEpoch;
    refreshDeviceWallpapers();
#ifdef Q_OS_WIN
    profiles.clear();
    profilesGeneration = 0;
#endif
    if (localPresence)
        localPresence->update(tailnetStatus, embeddedSelected, false);
}

bool NativeDesktop::beginVerifiedTailnetChange(const TailnetIntent &intent)
{
    const auto next = tailnetGeneration + 1;
    if (intent.change == TailnetChange::Logout) {
        if (!tailnetStatus.account || tailnetStatus.state != BackendState::Connected)
            return false;
        bool sent = embeddedSelected && tailnet && tailnet->requestLogout(next);
#ifdef Q_OS_WIN
        if (!embeddedSelected && systemTailnet) {
            sent = systemTailnet->requestLogout(next);
            if (sent)
                systemAction = next;
        }
#endif
        if (!sent)
            return false;
        tailnetGeneration = next;
    } else {
#ifdef Q_OS_WIN
        const auto selected = std::find_if(profiles.cbegin(), profiles.cend(), [&intent](const auto &profile) {
            return profile.id == intent.target && !profile.isCurrent;
        });
        if (intent.change != TailnetChange::Profile || embeddedSelected || !systemTailnet ||
            selected == profiles.cend() || !systemTailnet->requestProfileSwitch(selected->id, profilesGeneration))
            return false;
        systemAction = profilesGeneration;
#else
        return false;
#endif
    }
    tailnetBusy = true;
    tailnetRefreshing = true;
    openLoginAfterStatus = false;
    clearTailnetContext();
    tailnetFeedback.clear();
    publishTailnetStatus(tailnetStatus);
    publishHostState();
    return true;
}

void NativeDesktop::changeTailnetProvider(bool embedded, const aurora::tailscale::StatusResult &verified)
{
    QSaveFile preference(QDir(directory).filePath(QStringLiteral("tailnet-provider")));
    const QByteArray value = embedded ? QByteArrayLiteral("Embedded") : QByteArrayLiteral("System");
    if (directory.isEmpty() || !QDir().mkpath(directory) || !preference.open(QIODevice::WriteOnly) ||
        preference.write(value) != value.size() || !preference.commit()) {
        tailnetFeedback = tr("无法保存 Tailscale 运行方式。");
        finishTailnetSwitch(false);
        publishTailnetStatus(tailnetStatus);
        return;
    }
    embeddedSelected = embedded;
    openLoginAfterStatus = false;
    clearTailnetContext();
#ifdef Q_OS_WIN
    wantSystemProfiles = !embedded;
#endif
    tailnetFeedback.clear();
    syncEmbeddedHost();
    // Reconcile the previous embedded publication first. Its completion resumes the refresh.
    if (tailnetSwitch)
        tailnetSwitch->statusDeadline.emplace(6000);
    if (!pendingAdvertisement) {
        // The destination was just read successfully. Reuse that reply after persisting the selection.
        tailnetBusy = true;
        receiveTailnetStatus(tailnetGeneration, verified);
    } else {
        refreshAfterRetirement = true;
        publishTailnetStatus(tailnetStatus);
    }
}

#ifdef Q_OS_WIN
void NativeDesktop::readSystemProfiles()
{
    wantSystemProfiles = false;
    if (!embeddedSelected && systemTailnet && !tailnetBusy && systemTailnet->requestProfiles(tailnetGeneration)) {
        profilesRequest = tailnetGeneration;
        tailnetBusy = true;
        tailnetRefreshing = true;
        publishTailnetStatus(tailnetStatus, true);
    }
}

void NativeDesktop::receiveSystemProfiles(quint64 generation, const aurora::tailscale::system::ProfilesResult &result)
{
    if (embeddedSelected || !profilesRequest || *profilesRequest != generation)
        return;
    profilesRequest.reset();
    profiles = result.error == StatusError::None ? result.profiles : QVector<aurora::tailscale::system::Profile>{};
    profilesGeneration = generation;
    ++accountRevision;
    tailnetBusy = false;
    tailnetRefreshing = false;
    publishTailnetStatus(tailnetStatus);
}

void NativeDesktop::receiveSystemAction(quint64 generation, const aurora::tailscale::system::ActionResult &result)
{
    if (embeddedSelected || !systemAction || *systemAction != generation)
        return;
    systemAction.reset();
    tailnetBusy = false;
    tailnetRefreshing = false;
    if (result.error != StatusError::None) {
        tailnetFeedback = tr("无法完成系统 Tailscale 操作，请刷新后重试。");
        openLoginAfterStatus = false;
    }
    if (tailnetSwitch && result.action == aurora::tailscale::system::Action::SwitchProfile) {
        tailnetSwitch->failed = result.error != StatusError::None;
        tailnetSwitch->statusDeadline.emplace(6000);
        profiles = result.verifiedProfiles;
        profilesGeneration = generation;
        wantSystemProfiles = tailnetSwitch->failed;
        readTailnet(false);
    } else {
        wantSystemProfiles = true;
        readTailnet();
    }
}
#endif

bool NativeDesktop::handleTailnetAction(const QString &action, const QJsonObject &request)
{
    if (action == QLatin1String("refresh") || action == QLatin1String("account") ||
        action == QLatin1String("tailscale.enter") || action == QLatin1String("tailscale.refresh") ||
        action == QLatin1String("tailscale.retry")) {
        if (tailnetSwitch || manualRefresh)
            return true;
        if (action == QLatin1String("refresh") || action == QLatin1String("tailscale.refresh"))
            tailnetFeedback.clear();
#ifdef Q_OS_WIN
        if (!embeddedSelected && action != QLatin1String("refresh"))
            wantSystemProfiles = true;
#endif
        if (action == QLatin1String("tailscale.retry"))
            tailnetFeedback.clear();
        if (action == QLatin1String("tailscale.enter") && !accountRefreshFailed)
            accountRefreshFeedback.clear();
        if (action == QLatin1String("refresh"))
            startTailnetRefresh(RefreshTarget::Devices);
        else if (action == QLatin1String("tailscale.refresh"))
            startTailnetRefresh(RefreshTarget::Account);
        else
            readTailnet();
        return true;
    }
    if (!action.startsWith(QLatin1String("tailscale.")))
        return false;
    const bool switchAction = action == QLatin1String("tailscale.profile") || action == QLatin1String("tailscale.mode") ||
        ((action == QLatin1String("tailscale.confirm") || action == QLatin1String("tailscale.cancel")) &&
         tailnetConfirmation && tailnetConfirmation->change != TailnetChange::Logout);
    if (!snapshot.value(QStringLiteral("windowVisible")).toBool() || tailnetSwitch ||
        (tailnetBusy && (!switchAction || tailnetRefreshing || tailnetVerification)))
        return true;

    const auto target = request.value(QStringLiteral("accountTarget")).toString();
    if (action == QLatin1String("tailscale.confirm") || action == QLatin1String("tailscale.cancel")) {
        if (!tailnetConfirmation || target != QString::number(tailnetInvitation))
            return true;
        const auto intent = *std::exchange(tailnetConfirmation, std::nullopt);
        if (action == QLatin1String("tailscale.cancel") || intent.revision != accountRevision) {
            publishTailnetStatus(tailnetStatus);
            return true;
        }
        if (intent.change == TailnetChange::Provider) {
            startTailnetSwitch(intent);
            beginTailnetProviderSwitch();
        } else if (intent.change == TailnetChange::Profile) {
            verifyTailnetProfile(intent);
        } else {
            tailnetVerification = intent;
            readTailnet();
        }
        return true;
    }

    const auto currentRevision = request.value(QStringLiteral("accountRevision")).toInt(-1);
    if (tailnetConfirmation || currentRevision != accountRevision)
        return true;
    if (action == QLatin1String("tailscale.logout") && tailnetStatus.account) {
        tailnetConfirmation = TailnetIntent{TailnetChange::Logout, {}, tailnetStatus.account, accountRevision};
    }
#ifdef Q_OS_WIN
    else if (action == QLatin1String("tailscale.mode")) {
        if ((target == QLatin1String("embedded") && !embeddedSelected && tailnet && tailnet->isAvailable()) ||
            (target == QLatin1String("system") && embeddedSelected && hasSystemRuntime))
            tailnetConfirmation = TailnetIntent{TailnetChange::Provider, target, tailnetStatus.account, accountRevision};
    } else if (action == QLatin1String("tailscale.profile") && !embeddedSelected) {
        const int index = target.toInt();
        if (index >= 0 && index < profiles.size() && target == QString::number(index) && !profiles.at(index).isCurrent) {
            const TailnetIntent intent{TailnetChange::Profile, profiles.at(index).id, tailnetStatus.account, accountRevision};
            if (connection)
                tailnetConfirmation = intent;
            else {
                verifyTailnetProfile(intent);
                return true;
            }
        }
    }
#endif
    else if (action == QLatin1String("tailscale.primary") &&
               (tailnetStatus.state == BackendState::NeedsLogin || tailnetStatus.state == BackendState::Stopped)) {
        const bool login = tailnetStatus.state == BackendState::NeedsLogin;
        if (tailnetStatus.state == BackendState::NeedsLogin && !tailnetStatus.loginUrl.isEmpty()) {
            openTailnetLogin();
        } else {
            const auto next = tailnetGeneration + 1;
            bool sent = embeddedSelected && tailnet && tailnet->requestLogin(next);
#ifdef Q_OS_WIN
            if (!embeddedSelected && systemTailnet) {
                sent = login ? systemTailnet->requestLogin(next) : systemTailnet->requestResume(next);
                if (sent)
                    systemAction = next;
            }
#endif
            if (sent) {
                tailnetGeneration = next;
                tailnetBusy = true;
                tailnetRefreshing = true;
                openLoginAfterStatus = login;
                tailnetStatus.state = BackendState::Starting;
                ++accountRevision;
                tailnetFeedback.clear();
            }
        }
    }
    if (tailnetConfirmation)
        ++tailnetInvitation;
    publishTailnetStatus(tailnetStatus);
    return true;
}

void NativeDesktop::publishTailnetStatus(const aurora::tailscale::Status &status, bool refreshing)
{
    QString label;
    QString state;
    QString detail;
    switch (status.state) {
    case BackendState::Connected:
        label = tr("已连接"); state = QStringLiteral("Connected");
        break;
    case BackendState::NeedsLogin:
        label = tailnetBrowserPending ? tr("等待登录") : tr("未登录");
        state = QStringLiteral("NeedsLogin");
        detail = tailnetBrowserPending ? tr("请在浏览器完成 Tailscale 登录，Aurora 会自动刷新。")
                                      : tr("登录后可发现同一 Tailscale 网络中的设备。");
        break;
    case BackendState::NeedsApproval:
        label = tr("等待批准"); state = QStringLiteral("NeedsApproval");
        detail = tr("你的 Tailscale 网络需要管理员批准这台设备。批准后会自动更新。");
        break;
    case BackendState::Starting:
        label = tr("正在启动"); state = QStringLiteral("Starting");
        detail = tr("正在启动 Tailscale，状态会自动刷新。");
        break;
    case BackendState::Stopped:
        label = embeddedSelected ? tr("已停止") : tr("已暂停"); state = QStringLiteral("Stopped");
        detail = embeddedSelected ? tr("启动内置 Tailscale 后，即可登录或恢复连接。")
                                  : tr("恢复异地连接并保留现有设置。");
        break;
    case BackendState::Unknown:
        label = tr("状态未知"); state = QStringLiteral("Unknown");
        detail = tr("暂时无法读取 Tailscale，请确认后台服务已启动后刷新。");
        break;
    }

    const bool switching = tailnetSwitch.has_value();
    const bool runtimeProgress = switching && tailnetSwitch->intent.change == TailnetChange::Provider;
    const bool showEmbedded = runtimeProgress ? tailnetSwitch->intent.target == QLatin1String("system") : embeddedSelected;
    const auto progress = !switching ? QString() : runtimeProgress
        ? (tailnetSwitch->intent.target == QLatin1String("embedded")
            ? tr("正在切换到内置 Tailscale…") : tr("正在切换到系统 Tailscale…"))
        : tr("正在切换系统账号…");
    if (switching) {
        label = tr("正在切换");
        state = QStringLiteral("Switching");
        detail.clear();
    }

    const bool embeddedAvailable = tailnet && tailnet->isAvailable();
    const bool refreshAvailable = embeddedSelected ? embeddedAvailable : hasSystemRuntime;
    const bool needsLogin = status.state == BackendState::NeedsLogin;
    const auto runtimeNotice = needsLogin && !switching && tailnetRuntimeNeedsLogin
        ? tr("此账号需要重新登录。") : QString();
    const bool startAvailable = status.state == BackendState::Stopped;
    const auto primaryAction = needsLogin ? tr("登录 Tailscale")
        : startAvailable ? (embeddedSelected ? tr("启动内置 Tailscale") : tr("恢复连接")) : QString();
    QVariant confirmation;
    if (tailnetConfirmation) {
        const auto kind = tailnetConfirmation->change;
        const bool logout = kind == TailnetChange::Logout;
        const auto title = logout ? tr("要退出登录吗？")
            : kind == TailnetChange::Provider
                ? (tailnetConfirmation->target == QLatin1String("embedded")
                    ? tr("切换到内置 Tailscale？") : tr("切换到系统 Tailscale？"))
                : tr("切换系统 Tailscale 账号？");
        confirmation = QVariantMap{
            {QStringLiteral("id"), tailnetInvitation},
            {QStringLiteral("title"), title},
            {QStringLiteral("body"), logout ? tr("退出后本机会断开 Tailscale 网络，重新登录后才能连接设备。")
                : tr("Aurora 将重新读取所选账号的设备。正在进行的连接可能中断。")},
            {QStringLiteral("label"), logout ? tr("退出登录") : tr("切换")},
            {QStringLiteral("destructive"), logout},
        };
    }
    QVariantList profileRows;
#ifdef Q_OS_WIN
    if (!embeddedSelected) {
        for (int index = 0; index < profiles.size(); ++index) {
            const auto &profile = profiles.at(index);
            profileRows.append(QVariantMap{
                {QStringLiteral("key"), QString::number(index)},
                {QStringLiteral("name"), profile.name},
                {QStringLiteral("loginName"), profile.loginName},
                {QStringLiteral("tailnet"), profile.tailnet},
                {QStringLiteral("current"), profile.isCurrent},
            });
        }
    }
#endif
    const auto name = switching && !tailnetSwitch->name.isEmpty() ? tailnetSwitch->name
        : status.displayName.isEmpty() ? status.loginName : status.displayName;
    const auto login = switching && !tailnetSwitch->login.isEmpty() ? tailnetSwitch->login : status.loginName;
    if (!switching) {
        snapshot.insert(QStringLiteral("accountName"), name);
        snapshot.insert(QStringLiteral("accountSubtitle"), login);
    }
    const bool refreshingDevices = manualRefresh && manualRefresh->target == RefreshTarget::Devices;
    const bool refreshingAccount = manualRefresh && manualRefresh->target == RefreshTarget::Account;
    snapshot.insert(QStringLiteral("refreshAvailable"), refreshAvailable && !tailnetBusy && !tailnetConfirmation &&
        !switching && (!manualRefresh || refreshingDevices));
    snapshot.insert(QStringLiteral("refreshing"), refreshingDevices);
    snapshot.insert(QStringLiteral("tailscaleAccount"), QVariantMap{
        {QStringLiteral("mode"), showEmbedded ? QStringLiteral("embedded") : QStringLiteral("system")},
        {QStringLiteral("modeLabel"), showEmbedded ? tr("内置 Tailscale") : tr("系统 Tailscale")},
        {QStringLiteral("supportsEmbedded"), embeddedAvailable},
        {QStringLiteral("hasSystemRuntime"), hasSystemRuntime},
        {QStringLiteral("state"), state},
        {QStringLiteral("status"), label},
        {QStringLiteral("displayName"), name.isEmpty() ? QStringLiteral("Tailscale") : name},
        {QStringLiteral("loginName"), login},
        {QStringLiteral("initial"), switching ? name.left(1).toUpper() : QString()},
        {QStringLiteral("tailnetName"), status.tailnetName},
        {QStringLiteral("localAddress"), status.localAddress.toString()},
        {QStringLiteral("refreshAvailable"), refreshAvailable},
        {QStringLiteral("refreshing"), (refreshing || refreshingAccount) && !switching},
        {QStringLiteral("refreshFeedback"), accountRefreshFeedback},
        {QStringLiteral("refreshFailed"), accountRefreshFailed},
        {QStringLiteral("refreshRevision"), accountRefreshRevision},
        {QStringLiteral("refreshEnabled"), !manualRefresh || refreshingAccount},
        {QStringLiteral("busy"), switching || tailnetRefreshing || tailnetConfirmation.has_value() || refreshAfterRetirement},
        {QStringLiteral("runtimeProgress"), runtimeProgress},
        {QStringLiteral("runtimeNotice"), runtimeNotice},
        {QStringLiteral("detail"), detail},
        {QStringLiteral("progress"), progress},
        {QStringLiteral("retryAvailable"), !switching && tailnetSwitchRetryAvailable && !tailnetFeedback.isEmpty()},
        {QStringLiteral("notice"), tailnetNotice},
        {QStringLiteral("noticeVersion"), tailnetNoticeVersion},
        {QStringLiteral("focusVersion"), tailnetFocusVersion},
        {QStringLiteral("primaryAction"), primaryAction},
        {QStringLiteral("canOpenPrimaryAction"), refreshAvailable && !tailnetBusy && !primaryAction.isEmpty()},
        {QStringLiteral("feedback"), tailnetFeedback},
        {QStringLiteral("confirmation"), confirmation},
        {QStringLiteral("revision"), accountRevision},
        {QStringLiteral("profiles"), profileRows},
    });
    updateDeviceCatalog(status);
    refreshAccountAvatar(status);
    emit stateChanged(snapshot);
}

void NativeDesktop::refreshAccountAvatar(const aurora::tailscale::Status &status)
{
    if (!accountAvatar)
        return;

    std::optional<Aurora::Ui::AvatarLoader::Source> source;
    const bool visible = avatarVisible && snapshot.value(QStringLiteral("windowVisible")).toBool();
    if (visible && !tailnetSwitch && status.state == aurora::tailscale::BackendState::Connected && status.account
        && status.account->ownerId > 0 && !status.account->nodeId.isEmpty() && !status.account->tailnetId.isEmpty()) {
        const auto url = aurora::tailscale::vettedAvatarUrl(status.avatarUrl.toString(QUrl::FullyEncoded));
        if (!url.isEmpty()) {
            const auto &account = *status.account;
            const QJsonArray identity{QString::number(account.ownerId), account.nodeId, account.tailnetId};
            source = Aurora::Ui::AvatarLoader::Source{QJsonDocument(identity).toJson(QJsonDocument::Compact), url};
        }
    }
    accountAvatar->setContext(quint64(accountRevision), source, visible);
}

void NativeDesktop::updateDeviceCatalog(const aurora::tailscale::Status &status)
{
    const auto account = tailnetSwitch ? std::optional<aurora::tailscale::AccountIdentity>{} : status.account;
    if (!(catalogAccount == account)) {
        clientGrantCache.clear();
        catalogAccount = account;
        ++deviceEpoch;
    }
    QVariantList rows;
    if (account && localPresence) {
        const auto devices = Aurora::Discovery::visibleDevices(&localPresence->network(), &localPresence->lan(),
                                                               &localPresence->tailnet());
        for (const auto &device : devices)
            rows.append(Aurora::Discovery::qtDeviceRow(device, cachedDeviceWallpaper(device)).toVariantMap());
    }
    const bool signedOut = !tailnetSwitch &&
        (status.state == BackendState::NeedsLogin || status.state == BackendState::Stopped);
    snapshot.insert(QStringLiteral("deviceAccess"), account ? QStringLiteral("ready")
        : signedOut ? QStringLiteral("signedOut") : QStringLiteral("loading"));
    snapshot.insert(QStringLiteral("devices"), rows);
    snapshot.insert(QStringLiteral("deviceEpoch"), deviceEpoch);
}

void NativeDesktop::initializeTray()
{
    if (!tray.initialize())
        return;
    connect(&tray, &DesktopTray::openRequested, this, [this] { activate(); });
    connect(&tray, &DesktopTray::settingsRequested, this, [this] { activate(QStringLiteral("settings")); });
    connect(&tray, &DesktopTray::quitRequested, this, &NativeDesktop::requestExit);
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
    if (action == QLatin1String("avatar-visible") || action == QLatin1String("avatar-hidden")) {
        avatarVisible = action == QLatin1String("avatar-visible");
        refreshAccountAvatar(tailnetStatus);
        return;
    }
    if (action == QLatin1String("devices-foreground") || action == QLatin1String("devices-background")) {
        devicesForeground = action == QLatin1String("devices-foreground");
        refreshHostWallpaper();
        refreshDeviceWallpapers();
        return;
    }
    if (handleTailnetAction(action, request))
        return;
    if (handleHostAction(action))
        return;
    if (handleSunshineAction(action, request))
        return;
    if (handleDevicePower(action, request))
        return;
    if (handleConnectAction(action, request))
        return;
    if (action != QLatin1String("window-close")) {
        refreshPreferences(request);
        return;
    }
    openLoginAfterStatus = false;
    tailnetConfirmation.reset();
    if (tray.isVisible()) {
        snapshot.insert(QStringLiteral("windowVisible"), false);
        refreshDeviceWallpapers();
        publishTailnetStatus(tailnetStatus);
    } else {
        requestExit();
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
    QString privacyTestFeedback;
#ifdef Q_OS_WIN
    const auto programFiles = qEnvironmentVariable("ProgramFiles");
    const bool privacyHelperInstalled = !programFiles.isEmpty() && QFileInfo(QDir(programFiles)
        .filePath(QStringLiteral("Aurora Developer/AuroraPrivacyOverlay.exe"))).isFile();
#endif
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
#ifdef Q_OS_WIN
    } else if (action == QLatin1String("set-privacy-uiaccess-test")) {
        if (!value.isBool())
            privacyTestFeedback = tr("无法保存隐私屏保实现。");
        else if (value.toBool() && !privacyHelperInstalled)
            privacyTestFeedback = tr("未安装本机 UIAccess 测试组件。");
        else if (!preferences.setPrivacyUiAccessTest(value.toBool()))
            privacyTestFeedback = tr("无法保存隐私屏保实现。");
#endif
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
#ifdef Q_OS_WIN
        {QStringLiteral("privacyUiAccessTest"), preferences.privacyUiAccessTest()},
        {QStringLiteral("privacyHelperInstalled"), privacyHelperInstalled},
        {QStringLiteral("privacyTestFeedback"), privacyTestFeedback},
#endif
    });
    emit stateChanged(snapshot);
}
