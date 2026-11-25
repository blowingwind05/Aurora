// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "adaptors/moonlight/control/ControlAdapter.h"

#include <algorithm>
#include <utility>

namespace {
using namespace Aurora::Discovery;
namespace Control = Aurora::Stream::Control;
using aurora::tailscale::EmbeddedStatusClient;

std::optional<Endpoint> relayEndpoint(const std::shared_ptr<aurora::tailscale::StreamLease> &relay)
{
    if (!relay)
        return {};
    return Endpoint{relay->address(), relay->port()};
}
}

void NativeDesktop::initializeDevicePower(const QString &runtime)
{
    if (!Aurora::Stream::isBundledMoonlight(runtime))
        return;
    powerRuntime = runtime;
    powerAdapter = std::make_unique<Control::Adapter>();
    connect(tailnet.get(), &EmbeddedStatusClient::openFinished, this, [this](quint64 generation, quint64 id, const auto &result) {
        if (!devicePower || devicePower->relayRequest != id)
            return;
        devicePower->relayRequest = 0;
        if (!ownsDevicePower() || generation != tailnetGeneration || !result.lease || !result.lease->isValid()) {
            tryPowerRoute();
            return;
        }
        devicePower->relay = result.lease;
        checkPowerRoute();
    });
    connect(&localPresence->routeClient(), &ServerInfo::Client::checkFinished, this,
            [this](quint64 id, const ServerInfo::CheckResult &result) {
        if (!devicePower || devicePower->check != id)
            return;
        devicePower->check = 0;
        if (!ownsDevicePower() || result.status != ServerInfo::Status::Complete || !result.current) {
            tryPowerRoute();
            return;
        }

        const auto &attempt = *devicePower;
        const bool submitted = powerAdapter->start({attempt.token, attempt.operation, powerRuntime,
            *attempt.selected, result, relayEndpoint(attempt.relay), attempt.relay, [this](const QUuid &token) {
                return ownsDevicePower() && devicePower->token == token;
            }});
        if (!submitted)
            settleDevicePower(tr("无法启动设备电源操作。"), true);
    });
    connect(powerAdapter.get(), &Control::Adapter::finished, this, [this](const QUuid &token, const Control::Result &result) {
        if (!devicePower || devicePower->token != token)
            return;
        if (result.status == Control::Status::Accepted) {
            settleDevicePower(devicePower->operation == Control::Operation::Restart ? tr("被控端已确认重启请求。")
                                                                                     : tr("被控端已确认关机请求。"));
        } else if (result.status == Control::Status::PairingRequired) {
            settleDevicePower(tr("请先连接并完成设备配对。"), true);
        } else {
            settleDevicePower(tr("未收到被控端确认，请检查设备状态。"), true);
        }
    });
}

bool NativeDesktop::handleDevicePower(const QString &action, const QJsonObject &request)
{
    const bool restart = action == QLatin1String("device-power-restart");
    if (!restart && action != QLatin1String("device-power-shutdown"))
        return false;
    if (!powerAdapter || devicePower || (connection && !connection->launched) || !tailnetStatus.account ||
        tailnetConfirmation || tailnetVerification || request.value(QStringLiteral("deviceEpoch")).toInt(-1) != deviceEpoch ||
        !snapshot.value(QStringLiteral("windowVisible")).toBool())
        return true;

    const auto id = request.value(QStringLiteral("deviceId")).toString();
    const auto devices = visibleDevices(&localPresence->network(), &localPresence->lan(), &localPresence->tailnet());
    const auto selected = std::find_if(devices.cbegin(), devices.cend(), [&id](const Device &device) {
        return device.id == id && device.online && device.supportsPower();
    });
    if (selected == devices.cend())
        return true;

    devicePower.emplace(DevicePowerAttempt{QUuid::createUuid(), id, *tailnetStatus.account, deviceEpoch,
        restart ? Control::Operation::Restart : Control::Operation::Shutdown});
    for (const auto *route : {&selected->lan, &selected->tailnet}) {
        if (*route && (*route)->supportsPower)
            devicePower->routes.enqueue(**route);
    }
    refreshDeviceWallpapers();
    localPresence->setSessionActive(true);
    tryPowerRoute();
    return true;
}

bool NativeDesktop::ownsDevicePower() const
{
    return devicePower && tailnetStatus.account == devicePower->account && deviceEpoch == devicePower->epoch &&
        std::any_of(tailnetStatus.peers.cbegin(), tailnetStatus.peers.cend(), [this](const auto &peer) {
            return peer.nodeId == devicePower->deviceId;
        });
}

void NativeDesktop::tryPowerRoute()
{
    if (!ownsDevicePower() || devicePower->routes.isEmpty()) {
        settleDevicePower(tr("无法确认这台设备的电源操作地址。"), true);
        return;
    }
    auto &attempt = *devicePower;
    attempt.relay.reset();
    attempt.selected = attempt.routes.dequeue();
    if (attempt.selected->viaEmbeddedTailnet) {
        attempt.relayRequest = tailnet->requestOpen(tailnetGeneration, attempt.deviceId, attempt.selected->endpoint.address);
        if (!attempt.relayRequest)
            settleDevicePower(tr("无法建立设备电源操作的 Tailscale 通道。"), true);
    } else {
        checkPowerRoute();
    }
}

void NativeDesktop::checkPowerRoute()
{
    auto &attempt = *devicePower;
    attempt.check = localPresence->routeClient().revalidate(*attempt.selected, relayEndpoint(attempt.relay), RouteRequirement::Power);
    if (!attempt.check)
        settleDevicePower(tr("Aurora 正在处理另一项设备操作，请重试。"), true);
}

void NativeDesktop::settleDevicePower(const QString &message, bool failed)
{
    auto prior = std::exchange(devicePower, std::nullopt);
    if (!prior)
        return;
    powerAdapter->cancel(prior->token);
    if (prior->check)
        localPresence->routeClient().cancel(prior->check);
    if (prior->relayRequest)
        tailnet->cancelOpen(prior->relayRequest);
    localPresence->setSessionActive(bool(connection));
    if (!message.isEmpty()) {
        snapshot.insert(QStringLiteral("refreshFeedback"), message);
        snapshot.insert(QStringLiteral("refreshFailed"), failed);
        emit stateChanged(snapshot);
    }
}
