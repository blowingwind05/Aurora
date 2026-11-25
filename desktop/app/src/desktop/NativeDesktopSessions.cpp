// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
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
    connect(sessionSupervisor.get(), &SessionSupervisor::preferenceChangeRequested, this,
            [this](const QString &option, const QString &value) {
        if (!ownsConnection() || !connection->launched)
            return;
        const auto saved = streamPreferences.applySessionControlChange({option, value});
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
    initializeDeviceWallpapers(runtime);
    initializeDevicePower(runtime);
}

bool NativeDesktop::handleConnectAction(const QString &action, const QJsonObject &request)
{
    if (action != QLatin1String("connect"))
        return false;
    if (!sessionSupervisor || !localPresence || connection || exitAfterSession ||
        tailnetConfirmation || tailnetVerification || !tailnetStatus.account ||
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
    return connection && tailnetStatus.account && *tailnetStatus.account == connection->account &&
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
    auto &next = *connection;
    next.selected = next.routes.dequeue();
    next.prepared.reset();
    next.privacy.reset();
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
        if (result.current->supportsSessionPrivacy) {
            connection->privacy = SessionPrivacyRoute{*connection->selected, result,
                                                       relayEndpoint(connection->prepared->route)};
        }
#endif
        verifyMoonlightTrust();
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
    Aurora::Stream::Request request;
    request.operation = Aurora::Stream::Operation::List;
    request.route = prepared.route;
    auto command = Aurora::Stream::makeCommand(prepared.executable, request);
    if (!command.command) {
        failRoute(tr("无法启动串流组件验证配对。"), false);
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
    trustCheck.release()->deleteLater();
    if (!ownsConnection() || connection->prepared->generation != tailnetGeneration) {
        cancelConnection(tr("连接信息已变化，请重试。"));
        return;
    }
    if (!trusted) {
        if (beginPairing())
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
    const auto selection = streamPreferences.read();
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
    if (!sessionSupervisor->start({attempt.attempt, attempt.generation, connection->privacy}, std::move(view))) {
        failRoute(tr("无法启动串流会话。"), false);
        return;
    }
    connection->launched = true;
    sessionWindow.show();
    reportConnection({}, false);
}

void NativeDesktop::sessionFinished(const QUuid &attempt, quint64, const SessionResult &result)
{
    sessionWindow.close(attempt);
    if (exitAfterSession) {
        exitAfterSession = false;
        requestExit();
        return;
    }
    if (!connection || connection->attempt != attempt)
        return;

    const bool failedRelay = result.reason == SessionEnd::RelayUnavailable || result.reason == SessionEnd::RelayLost;
    if ((connection->retryRoute || (failedRelay && !connection->launched)) &&
        !connection->routes.isEmpty() && ownsConnection()) {
        QTimer::singleShot(0, this, [this, attempt] {
            if (connection && connection->attempt == attempt)
                prepareNextRoute();
        });
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
    releasePairingTransport();
    const auto prior = std::exchange(connection, std::nullopt);
    if (!prior)
        return;
    connectionStatusRetry.stop();
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
    if (!tryNext || connection->routes.isEmpty() || !ownsConnection()) {
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
    emit stateChanged(snapshot);
}
