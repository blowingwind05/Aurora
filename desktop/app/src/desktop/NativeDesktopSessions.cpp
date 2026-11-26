// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "adaptors/moonlight/MoonlightPin.h"
#include "ui/StreamSessionWindow.h"

#include <algorithm>
#include <utility>

namespace {
using namespace Aurora::Discovery;
using namespace Aurora::Session;

QList<Device> availableDevices(const NativeDevicePresence &presence)
{
    return visibleDevices(&presence.network(), &presence.lan(), &presence.tailnet());
}

std::optional<Endpoint> relayEndpoint(const Aurora::Stream::Route &route)
{
    if (!route.viaEmbeddedTailnet)
        return {};
    return Endpoint{route.address, quint16(route.port)};
}
}

void NativeDesktop::initializeStreamSessions(const QString &applicationDirectory)
{
    const auto runtime = Aurora::Stream::findMoonlight(applicationDirectory);
    sessionSupervisor = std::make_unique<SessionSupervisor>(runtime, tailnet.get());
    connect(sessionSupervisor.get(), &SessionSupervisor::prepared, this, &NativeDesktop::receivePrepared);
    connect(sessionSupervisor.get(), &SessionSupervisor::finished, this, &NativeDesktop::sessionFinished);
    connect(sessionSupervisor.get(), &SessionSupervisor::videoSettingRequested, this, &NativeDesktop::changeVideoSetting);
    connect(sessionSupervisor.get(), &SessionSupervisor::videoSettingsApplied, this, &NativeDesktop::finishVideoSetting);
    connect(sessionSupervisor.get(), &SessionSupervisor::preferenceChangeRequested, this,
            [this](const QString &option, const QString &value) {
        if (!ownsConnection() || !connection->launched)
            return;
        const Aurora::Preferences::SessionControlChange change{option, value};
        const auto saved = (change.isVideoSetting() ? deviceStreamPreferences() : streamPreferences)
            .applySessionControlChange(change);
        if (saved == Aurora::Preferences::SessionControlSaveResult::SaveFailed)
            reportConnection(tr("无法保存串流设置。"), true);
    });
    connect(&localPresence->routeClient(), &ServerInfo::Client::checkFinished, this, &NativeDesktop::routeChecked);
    connectionStatusRetry.setInterval(120);
    connect(&connectionStatusRetry, &QTimer::timeout, this, &NativeDesktop::requestConnectionStatus);
    trustDeadline.setSingleShot(true);
    connect(&trustDeadline, &QTimer::timeout, this, [this] {
        failRoute(tr("等待串流组件验证配对超时。"), true);
    });
    initializePairingClient();
    initializeRouteQuality();
    initializeDeviceWallpapers(runtime);
    initializeDevicePower(runtime);
}

bool NativeDesktop::handleConnectAction(const QString &action, const QJsonObject &request)
{
    if (action != QLatin1String("connect"))
        return false;
    if (!sessionSupervisor || !localPresence || connection || exitAfterSession ||
        tailnetSwitch || tailnetConfirmation || tailnetVerification || !tailnetStatus.account ||
        tailnetStatus.state != aurora::tailscale::BackendState::Connected ||
        !snapshot.value(QStringLiteral("windowVisible")).toBool() ||
        request.value(QStringLiteral("deviceEpoch")).toInt(-1) != deviceEpoch)
        return true;

    const auto id = request.value(QStringLiteral("deviceId")).toString();
    const auto devices = availableDevices(*localPresence);
    const auto selected = std::find_if(devices.cbegin(), devices.cend(), [&id](const Device &device) {
        return device.id == id && device.online && device.canConnect();
    });
    if (selected == devices.cend()) {
        reportConnection(tr("这台设备目前无法连接，请刷新设备列表。"), true);
        return true;
    }

    connection.emplace(Connection{selected->id, selected->name, *tailnetStatus.account, embeddedSelected});
    refreshDeviceWallpapers();
    localPresence->setSessionActive(true);
    reportConnection(tr("正在检查设备连接…"), false);
    connectionStatusRetry.start();
    requestConnectionStatus();
    return true;
}

void NativeDesktop::requestConnectionStatus()
{
    if (!ownsConnection() || connection->statusDeadline.hasExpired()) {
        cancelConnection(tr("暂时无法确认当前 Tailscale 账号，请重试。"));
        return;
    }
    if (tailnetBusy || tailnetConfirmation || tailnetVerification || connection->verificationGeneration)
        return;
    // A canceled discovery lease may still be closing in tsnet. Retry the
    // status request without using previously displayed credentials or routes.
    readTailnet(false);
    if (tailnetBusy)
        connection->verificationGeneration = tailnetGeneration;
}

bool NativeDesktop::ownsConnection() const
{
    return connection && tailnetStatus.state == aurora::tailscale::BackendState::Connected &&
        tailnetStatus.account && *tailnetStatus.account == connection->account &&
        embeddedSelected == connection->embedded &&
        std::any_of(tailnetStatus.peers.cbegin(), tailnetStatus.peers.cend(), [this](const auto &peer) {
            return peer.nodeId == connection->peerId;
        });
}

void NativeDesktop::connectionAccountReady()
{
    if (!connection)
        return;
    if (!ownsConnection()) {
        cancelConnection(tr("Tailscale 账号或设备已变化，连接已停止。"));
        return;
    }
    if (connection->routesVerified || connection->verificationGeneration != tailnetGeneration)
        return;
    connection->routesVerified = true;
    connectionStatusRetry.stop();

    const auto devices = availableDevices(*localPresence);
    const auto selected = std::find_if(devices.cbegin(), devices.cend(), [this](const Device &device) {
        return device.id == connection->peerId && device.online && device.canConnect();
    });
    if (selected == devices.cend()) {
        cancelConnection(tr("暂时没有可用的连接路径，请刷新设备列表后重试。"));
        return;
    }
    connection->preferredLan = selected->lan;
    connection->tailnetFallback = selected->tailnet;
    if (selected->lan)
        connection->routes.enqueue(*selected->lan);
    if (selected->tailnet)
        connection->routes.enqueue(*selected->tailnet);
    prepareNextRoute();
}

void NativeDesktop::prepareNextRoute()
{
    if (!ownsConnection() || connection->routes.isEmpty()) {
        cancelConnection(tr("暂时没有可用的连接路径。"));
        return;
    }
    endRouteQuality();
    auto &next = *connection;
    next.launched = false;
    next.streamingConfirmed = false;
    next.selected = next.routes.dequeue();
    next.prepared.reset();
    next.controls.reset();
    next.certificateSha256 = next.selected->publicIpv6
        ? next.publicCertificateSha256 : next.resumeCertificateSha256;
    next.waitingForEncoder = false;
    next.retryRoute = false;
    next.failure.clear();
    const auto &host = *next.selected;
    SessionCandidate candidate{tailnetGeneration, host.id, next.name,
                               DirectRoute{host.endpoint.address, host.endpoint.port}};
    if (host.viaEmbeddedTailnet)
        candidate.route = EmbeddedRoute{next.peerId, host.endpoint.address};
    next.attempt = sessionSupervisor->prepare(std::move(candidate));
    if (next.attempt.isNull())
        cancelConnection(tr("已有串流会话正在结束，请稍后重试。"));
}

void NativeDesktop::receivePrepared(const PreparedSession &session)
{
    if (!connection || connection->attempt != session.attempt)
        return;
    if (!ownsConnection() || session.generation != tailnetGeneration) {
        cancelConnection(tr("连接信息已变化，请重试。"));
        return;
    }
    connection->prepared = session;
    if (connection->selected->publicIpv6) {
        if (!session.verifiesHostIdentity ||
            connection->certificateSha256.size() != Aurora::Pairing::HostCertificateSha256Size ||
            (!connection->resumeCertificateSha256.isEmpty() &&
             connection->resumeCertificateSha256 != connection->certificateSha256)) {
            failRoute(tr("公网 IPv6 路径缺少已验证的证书绑定。"), true);
            return;
        }
#ifdef Q_OS_WIN
        connection->controls = SessionControlRoute{*connection->selected, {}, {}};
#endif
        verifyMoonlightTrust();
        return;
    }
    connection->probe = localPresence->routeClient().revalidate(*connection->selected, relayEndpoint(session.route));
    if (!connection->probe)
        failRoute(tr("无法重新检查被控端的串流地址。"), true);
}

void NativeDesktop::routeChecked(quint64 request, const ServerInfo::CheckResult &result)
{
    if (!connection || connection->probe != request)
        return;
    connection->probe = 0;
    if (!ownsConnection() || connection->prepared->generation != tailnetGeneration) {
        cancelConnection(tr("连接信息已变化，请重试。"));
        return;
    }
    if (result.status != ServerInfo::Status::Complete || !result.current) {
        failRoute(connection->waitingForEncoder ? tr("被控端的编码器尚未准备好。")
                                                : tr("无法验证被控端的串流地址。"), true);
        return;
    }
    if (!result.current->encoderProbePending) {
#ifdef Q_OS_WIN
        connection->controls = SessionControlRoute{*connection->selected, result,
                                                    relayEndpoint(connection->prepared->route)};
#endif
        if (canPairOverRoute(*connection->selected)) {
            if (connection->resumeCertificateSha256.isEmpty() && !connection->requestedHostRoutes) {
                connection->requestedHostRoutes = true;
                if (beginHostIdentity())
                    return;
            }
            verifyMoonlightTrust();
        } else if (!beginHostIdentity()) {
            failRoute(tr("当前直连路径无法绑定到 Tailscale 已确认的被控端证书。"), true);
        }
        return;
    }
    const auto endpoint = relayEndpoint(connection->prepared->route);
    connection->waitingForEncoder = true;
    connection->probe = localPresence->routeClient().waitForEncoder(*connection->selected, endpoint);
    if (!connection->probe)
        failRoute(tr("无法读取被控端的编码器状态。"), true);
    else
        reportConnection(tr("正在等待被控端准备画面…"), false);
}

void NativeDesktop::verifyMoonlightTrust()
{
    const auto &prepared = *connection->prepared;
    if (connection->clientGrantRequired) {
        if (connection->clientGranted && !connection->clientGrantDue.hasExpired())
            launchPreparedSession();
        else if (!beginHostIdentity(Aurora::Pairing::RequestKind::ClientGrant))
            cancelConnection(tr("无法启动已认证的客户端授权。"));
        return;
    }
    Aurora::Stream::Request request;
    request.operation = Aurora::Stream::Operation::List;
    request.route = prepared.route;
    request.route.certificateSha256 = connection->certificateSha256;
    auto command = Aurora::Stream::makeCommand(prepared.executable, request);
    if (!command.command) {
        failRoute(tr("无法启动串流组件验证配对。"), false);
        return;
    }
    if (!connection->certificateSha256.isEmpty()) {
        const auto pin = Aurora::Stream::readPin(*command.command, prepared.route.hostId);
        if (pin == connection->certificateSha256) {
            // The playback process checks the same Tailscale-authenticated pin before sending HTTPS requests.
            launchPreparedSession();
            return;
        }
        if (pin.isEmpty() && connection->resumeCertificateSha256.isEmpty() && beginPairing())
            return;
        failRoute(pin.isEmpty() ? tr("被控端还没有可用于串流的凭据。")
                                : tr("已保存的串流证书与 Tailscale 确认的证书不一致。"), true);
        return;
    }
    command.command->captureOutput = true;
    trustCheck = std::make_unique<Aurora::Stream::MoonlightProcess>();
    auto *worker = trustCheck.get();
    connect(worker, &QProcess::readyReadStandardOutput, this, [worker] { worker->readAllStandardOutput(); });
    connect(worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, worker](int code, QProcess::ExitStatus status) {
                if (trustCheck.get() == worker)
                    trustCheckFinished(status == QProcess::NormalExit && code == 0);
            });
    connect(worker, &QProcess::errorOccurred, this, [this, worker](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && trustCheck.get() == worker)
            failRoute(tr("无法启动串流组件验证配对。"), false);
    });
    trustDeadline.start(16000);
    if (!worker->launch(*command.command) && trustCheck.get() == worker)
        failRoute(tr("无法启动串流组件验证配对。"), false);
}

void NativeDesktop::trustCheckFinished(bool trusted)
{
    trustDeadline.stop();
    auto *worker = trustCheck.release();
    const auto workingDirectory = worker->workingDirectory();
    worker->deleteLater();
    if (!ownsConnection() || connection->prepared->generation != tailnetGeneration) {
        cancelConnection(tr("连接信息已变化，请重试。"));
        return;
    }
    if (!trusted) {
        Aurora::Stream::Command workerSettings;
        workerSettings.workingDirectory = workingDirectory;
        const auto pin = Aurora::Stream::readPin(workerSettings, connection->prepared->route.hostId);
        if (pin.isEmpty() && connection->resumeCertificateSha256.isEmpty() && beginPairing())
            return;
        failRoute(tr("当前路径未通过配对验证。"), true);
        return;
    }
    launchPreparedSession();
}

void NativeDesktop::launchPreparedSession()
{
    const auto &attempt = *connection->prepared;
    SessionView view;
    const auto selection = streamPreferences.read().withHostOverrides(deviceStreamPreferences().read().overrides);
    connection->playbackPreferences = selection;
    view.preferenceArguments = [selection](Aurora::Stream::PreferenceDefaults defaults) {
        return selection.arguments(defaults.useViewerDefault, defaults.useBundledDefaults);
    };
    view.sessionPreferences = selection.sessionControlSnapshot();
    view.startFullScreen = selection.value(QStringLiteral("display-mode")) == QLatin1String("fullscreen");
#ifdef Q_OS_WIN
    const auto windowError = sessionWindow.prepare(attempt.attempt, connection->name, *sessionSupervisor, view);
    if (!windowError.isEmpty()) {
        failRoute(windowError, false);
        return;
    }
#endif
    if (!sessionSupervisor->start({attempt.attempt, attempt.generation, connection->controls,
                                  connection->certificateSha256, !connection->resumeCertificateSha256.isEmpty()},
                                 std::move(view))) {
        failRoute(tr("无法启动串流会话。"), false);
        return;
    }
    connection->launched = true;
    sessionWindow.show();
    reportConnection({}, false);
}

Aurora::Preferences::StreamPreferences NativeDesktop::deviceStreamPreferences() const
{
    const auto &active = *connection;
    return Aurora::Preferences::StreamPreferences::forDevice(directory, active.account.tailnetId,
                                                              active.account.ownerId, active.selected->id);
}

void NativeDesktop::changeVideoSetting(const QString &option, const QString &value)
{
    const Aurora::Preferences::SessionControlChange change{option, value};
    if (!ownsConnection() || !connection->launched || connection->pendingVideoSetting || !change.isVideoSetting())
        return;

    auto &active = *connection;
    if (sessionSupervisor->videoSettings().value(option).toString() == value)
        return;
    const auto updated = active.playbackPreferences.withHostOverrides({{option, value}});
    active.pendingVideoSetting = change;
    SessionPlaybackSettings playback;
    playback.preferenceArguments = [updated](Aurora::Stream::PreferenceDefaults defaults) {
        return updated.arguments(defaults.useViewerDefault, defaults.useBundledDefaults);
    };
    playback.sessionPreferences = updated.sessionControlSnapshot();
    const auto attempt = active.attempt;
    if (!sessionSupervisor->reconfigureVideo(std::move(playback)) && connection && connection->attempt == attempt)
        connection->pendingVideoSetting.reset();
}

void NativeDesktop::finishVideoSetting(bool applied)
{
    if (!connection || !connection->pendingVideoSetting)
        return;
    const auto change = *std::exchange(connection->pendingVideoSetting, std::nullopt);
    if (!applied || !ownsConnection())
        return;

    connection->playbackPreferences = connection->playbackPreferences.withHostOverrides({{change.option, change.value}});
    if (deviceStreamPreferences().applySessionControlChange(change) == Aurora::Preferences::SessionControlSaveResult::SaveFailed)
        reportConnection(tr("串流画质已应用，无法保存到此设备的设置。"), true);
}

bool NativeDesktop::canQueryPublicFallback() const
{
    return ownsConnection() && !connection->launched && !connection->requestedHostRoutes &&
        !connection->lookingForPublicFallback && connection->selected &&
        canPairOverRoute(*connection->selected);
}

void NativeDesktop::sessionFinished(const QUuid &attempt, quint64, const SessionResult &result)
{
    endRouteQuality();
    sessionWindow.close(attempt);
    if (exitAfterSession) {
        exitAfterSession = false;
        requestExit();
        return;
    }
    if (!connection || connection->attempt != attempt)
        return;

    clientGrantRenewal.stop();
    if (!connection->streamingConfirmed && result.reason == SessionEnd::Exited &&
        result.exitCode && *result.exitCode != 0 && connection->clientGranted)
        clientGrantCache.forgetGrant(connection->peerId);
    if (pairingAttempt && pairingAttempt->renewingGrant) {
        pairingAttempt->interruptedPlayback = result;
        pairingClient->cancel(pairingAttempt->exchange);
        return;
    }

    const auto alternate = std::exchange(connection->pendingRouteSwitch, std::nullopt);
    if (result.reason == SessionEnd::RouteSwitched && alternate && ownsConnection() && connection->selected) {
        connection->resumeCertificateSha256 = connection->certificateSha256;
        connection->routes.clear();
        connection->routes.enqueue(*alternate);
        connection->routes.enqueue(*connection->selected);
        connection->retryRoute = true;
        reportConnection(tr("正在切换到更稳定的串流路径…"), false);
        if (!connection || connection->attempt != attempt)
            return;
    }

    if (result.canResumeAfterLoss && beginRouteRecovery())
        return;

    const bool failedRelay = result.reason == SessionEnd::RelayUnavailable || result.reason == SessionEnd::RelayLost;
    const bool resumeFailed = !connection->resumeCertificateSha256.isEmpty() &&
        result.reason != SessionEnd::Cancelled &&
        !(result.reason == SessionEnd::Exited && result.exitCode == 0);
    const bool ipv6StartupFailed = !connection->streamingConfirmed && connection->selected &&
        connection->selected->publicIpv6 && result.reason != SessionEnd::Cancelled &&
        !(result.reason == SessionEnd::Exited && result.exitCode == 0);
    const bool tailnetStartupFailed = !connection->streamingConfirmed && ownsConnection() && connection->selected &&
        canPairOverRoute(*connection->selected) && connection->certificateSha256.size() == Aurora::Pairing::HostCertificateSha256Size &&
        connection->certificateSha256 == connection->publicCertificateSha256 &&
        (failedRelay || result.reason == SessionEnd::TransportLost ||
            (result.reason == SessionEnd::Exited && result.exitCode && *result.exitCode != 0));
    if (tailnetStartupFailed) {
        for (const auto &route : connection->publicRoutes)
            connection->routes.enqueue(route);
    }
    if ((connection->retryRoute || (failedRelay && !connection->launched) || resumeFailed || ipv6StartupFailed ||
         tailnetStartupFailed) &&
        !connection->routes.isEmpty() && ownsConnection()) {
        QTimer::singleShot(0, this, [this, attempt] {
            if (connection && connection->attempt == attempt)
                prepareNextRoute();
        });
        return;
    }
    if ((failedRelay || connection->retryRoute) && canQueryPublicFallback()) {
        connection->lookingForPublicFallback = true;
        connection->requestedHostRoutes = true;
        connection->prepared.reset();
        if (beginHostIdentity())
            return;
    }
    const auto reason = connection->failure;
    connection->attempt = {};
    if (!reason.isEmpty())
        cancelConnection(reason);
    else if (result.reason == SessionEnd::Cancelled || (result.reason == SessionEnd::Exited && result.exitCode == 0))
        cancelConnection();
    else if (result.reason == SessionEnd::RuntimeUnavailable || result.reason == SessionEnd::RuntimeProbeFailed)
        cancelConnection(tr("内置串流组件缺失或版本不匹配。"));
    else if (failedRelay)
        cancelConnection(tr("Tailscale 串流连接已断开。"));
    else
        cancelConnection(tr("串流会话异常结束，请重试。"));
}

void NativeDesktop::cancelConnection(const QString &reason)
{
    if (pairingAttempt && pairingAttempt->exchange) {
        if (!pairingAttempt->cancellation)
            pairingAttempt->cancellation = reason;
        pairingClient->cancel(pairingAttempt->exchange);
        return;
    }
    endRouteQuality();
    const auto prior = std::exchange(connection, std::nullopt);
    if (!prior)
        return;
    connectionStatusRetry.stop();
    clientGrantRenewal.stop();
    localPresence->setSessionActive(bool(devicePower));
    if (prior->probe)
        localPresence->routeClient().cancel(prior->probe);
    trustDeadline.stop();
    if (auto *worker = trustCheck.release()) {
        disconnect(worker, nullptr, this, nullptr);
        worker->cancel();
        worker->deleteLater();
    }
    sessionSupervisor->cancel(prior->attempt);
    reportConnection(reason, !reason.isEmpty());
}

void NativeDesktop::failRoute(const QString &reason, bool tryNext)
{
    if (!connection)
        return;
    if (!tryNext || !ownsConnection() || (connection->routes.isEmpty() && !canQueryPublicFallback())) {
        cancelConnection(reason);
        return;
    }
    trustDeadline.stop();
    if (auto *worker = trustCheck.release()) {
        disconnect(worker, nullptr, this, nullptr);
        worker->cancel();
        worker->deleteLater();
    }
    connection->retryRoute = true;
    connection->failure = reason;
    if (!sessionSupervisor->cancel(connection->attempt))
        cancelConnection(reason);
}

void NativeDesktop::reportConnection(const QString &message, bool failed)
{
    snapshot.insert(QStringLiteral("connectionPending"), bool(connection));
    snapshot.insert(QStringLiteral("refreshFeedback"), message);
    snapshot.insert(QStringLiteral("refreshFailed"), failed);
    snapshot.insert(QStringLiteral("deviceRefreshResult"), false);
    emit stateChanged(snapshot);
}
