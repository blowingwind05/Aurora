// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"
#include "pairing/client/MoonlightPin.h"

#include <QFileInfo>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace {
using namespace Aurora::Discovery;
using Aurora::Stream::Route;
using Aurora::Wallpaper::FetchResult;
using aurora::tailscale::EmbeddedStatusClient;

QList<Device> visible(const NativeDevicePresence &presence)
{
    return visibleDevices(&presence.network(), &presence.lan(), &presence.tailnet());
}

bool sameRoute(const StreamHost &first, const StreamHost &second)
{
    return first.endpoint.address == second.endpoint.address && first.endpoint.port == second.endpoint.port &&
        first.viaEmbeddedTailnet == second.viaEmbeddedTailnet && sameHostIdentity(first.id, second.id);
}
}

void NativeDesktop::initializeDeviceWallpapers(const QString &runtime)
{
    if (!Aurora::Stream::isBundledMoonlight(runtime))
        return;
    wallpaperMoonlight = runtime;
    wallpaperClock.start();
    wallpaperClient = std::make_unique<Aurora::Wallpaper::PreviewClient>(directory, runtime);
    connect(tailnet.get(), &EmbeddedStatusClient::openFinished, this, [this](quint64 generation, quint64 id, const auto &result) {
        if (!wallpaperAttempt || wallpaperAttempt->relayRequest != id)
            return;
        wallpaperAttempt->relayRequest = 0;
        if (generation != tailnetGeneration || !wallpaperRouteCurrent() || !result.lease || !result.lease->isValid()) {
            tryWallpaperRoute();
            return;
        }
        wallpaperAttempt->relay = result.lease;
        wallpaperAttempt->leaseLifetime = std::make_unique<QObject>();
        checkWallpaperRoute();
    });
    connect(tailnet.get(), &EmbeddedStatusClient::leasesInvalidated, this, [this] {
        if (wallpaperAttempt && wallpaperAttempt->relay && !wallpaperAttempt->relay->isValid())
            finishWallpaperPreview(false);
    });
    connect(&localPresence->routeClient(), &ServerInfo::Client::checkFinished, this,
            [this](quint64 id, const ServerInfo::CheckResult &result) {
        if (!wallpaperAttempt || wallpaperAttempt->check != id)
            return;
        wallpaperAttempt->check = 0;
        if (!wallpaperRouteCurrent() || result.status != ServerInfo::Status::Complete || !result.current) {
            tryWallpaperRoute();
            return;
        }

        auto &attempt = *wallpaperAttempt;
        const auto &host = *attempt.selected;
        const auto relay = attempt.relay;
        const Route route{relay ? relay->address() : host.endpoint.address,
                          relay ? relay->port() : host.endpoint.port, host.id, host.viaEmbeddedTailnet};
        Aurora::Stream::Request request;
        request.operation = Aurora::Stream::Operation::Wallpaper;
        request.route = route;
        request.wallpaperRevision = QStringLiteral("-");
        const auto command = Aurora::Stream::makeCommand(wallpaperMoonlight, request);
        if (!command.command || Aurora::Pairing::Client::Detail::readPin(*command.command, host.id).isEmpty()) {
            tryWallpaperRoute();
            return;
        }
        const auto devices = visible(*localPresence);
        const auto match = std::find_if(devices.cbegin(), devices.cend(), [&attempt](const auto &device) {
            return device.id == attempt.deviceId;
        });
        if (match == devices.cend()) {
            finishWallpaperPreview(false);
            return;
        }
        attempt.fetch = wallpaperClient->start(*match, &localPresence->network(),
                                               {attempt.authorization, route, attempt.leaseLifetime.get()});
        if (!attempt.fetch)
            tryWallpaperRoute();
    });
    connect(wallpaperClient.get(), &Aurora::Wallpaper::PreviewClient::finished, this, [this](quint64 id, FetchResult result) {
        if (!wallpaperAttempt || wallpaperAttempt->fetch != id)
            return;
        wallpaperAttempt->fetch = 0;
        if (result == FetchResult::Unavailable || result == FetchResult::TimedOut)
            tryWallpaperRoute();
        else
            finishWallpaperPreview(result == FetchResult::Updated);
    });
}

QString NativeDesktop::cachedDeviceWallpaper(const Device &device) const
{
    const auto path = Aurora::Wallpaper::existingPreviewPath(directory, device, &localPresence->network());
    if (path.isEmpty())
        return {};
    auto url = QUrl::fromLocalFile(path);
    url.setQuery(QStringLiteral("revision=") + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()));
    return url.toString(QUrl::FullyEncoded);
}

void NativeDesktop::refreshDeviceWallpapers()
{
    if (!wallpaperClient)
        return;
    const bool foreground = devicesForeground && !connection && !devicePower && !exitAfterSession && tailnetStatus.account &&
        snapshot.value(QStringLiteral("windowVisible")).toBool();
    const auto authorization = wallpaperClient->setContext(quint64(deviceEpoch), foreground);
    if (!foreground) {
        releaseWallpaperPreview();
        return;
    }
    if (wallpaperAttempt) {
        if (wallpaperRouteCurrent())
            return;
        releaseWallpaperPreview();
    }
    if (localPresence->scanning() || tailnetBusy)
        return;

    const auto now = wallpaperClock.elapsed();
    for (auto entry = wallpaperChecked.begin(); entry != wallpaperChecked.end();) {
        if (entry.value() <= now)
            entry = wallpaperChecked.erase(entry);
        else
            ++entry;
    }
    for (const auto &device : visible(*localPresence)) {
        if (!device.online || !device.canConnect())
            continue;
        const auto path = Aurora::Wallpaper::previewCachePath(directory, device, &localPresence->network());
        if (path.isEmpty() || wallpaperChecked.contains(path))
            continue;
        wallpaperAttempt.emplace(WallpaperAttempt{device.id, path, deviceEpoch, authorization});
        if (device.lan)
            wallpaperAttempt->routes.enqueue(*device.lan);
        if (device.tailnet)
            wallpaperAttempt->routes.enqueue(*device.tailnet);
        tryWallpaperRoute();
        return;
    }
}

bool NativeDesktop::wallpaperRouteCurrent() const
{
    if (!wallpaperAttempt || !wallpaperAttempt->selected || wallpaperAttempt->epoch != deviceEpoch ||
        connection || !tailnetStatus.account || !devicesForeground ||
        !snapshot.value(QStringLiteral("windowVisible")).toBool())
        return false;
    const auto devices = visible(*localPresence);
    const auto &attempt = *wallpaperAttempt;
    return std::any_of(devices.cbegin(), devices.cend(), [&attempt](const Device &device) {
        return device.online && device.id == attempt.deviceId &&
            ((device.lan && sameRoute(*device.lan, *attempt.selected)) ||
             (device.tailnet && sameRoute(*device.tailnet, *attempt.selected)));
    });
}

void NativeDesktop::tryWallpaperRoute()
{
    auto &attempt = *wallpaperAttempt;
    attempt.leaseLifetime.reset();
    attempt.relay.reset();
    if (attempt.routes.isEmpty()) {
        finishWallpaperPreview(false);
        return;
    }
    attempt.selected = attempt.routes.dequeue();
    if (!wallpaperRouteCurrent()) {
        finishWallpaperPreview(false);
        return;
    }
    if (attempt.selected->viaEmbeddedTailnet) {
        attempt.relayRequest = tailnet->requestOpen(tailnetGeneration, attempt.deviceId, attempt.selected->endpoint.address);
        if (!attempt.relayRequest)
            finishWallpaperPreview(false);
    } else {
        checkWallpaperRoute();
    }
}

void NativeDesktop::checkWallpaperRoute()
{
    auto &attempt = *wallpaperAttempt;
    const auto relay = attempt.relay;
    const auto endpoint = relay ? std::optional<Endpoint>({relay->address(), relay->port()}) : std::nullopt;
    attempt.check = localPresence->routeClient().revalidate(*attempt.selected, endpoint);
    if (!attempt.check)
        finishWallpaperPreview(false);
}

void NativeDesktop::releaseWallpaperPreview()
{
    const auto prior = std::exchange(wallpaperAttempt, std::nullopt);
    if (!prior)
        return;
    if (prior->check)
        localPresence->routeClient().cancel(prior->check);
    if (prior->relayRequest)
        tailnet->cancelOpen(prior->relayRequest);
    if (prior->fetch)
        wallpaperClient->cancel(prior->fetch);
}

void NativeDesktop::finishWallpaperPreview(bool updated)
{
    wallpaperChecked.insert(wallpaperAttempt->cachePath, wallpaperClock.elapsed() + 120000);
    releaseWallpaperPreview();
    if (updated) {
        updateDeviceCatalog(tailnetStatus);
        emit stateChanged(snapshot);
    }
    QTimer::singleShot(0, this, &NativeDesktop::refreshDeviceWallpapers);
}
