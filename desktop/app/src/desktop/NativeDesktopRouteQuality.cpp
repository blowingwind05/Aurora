// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"

#include <algorithm>
#include <utility>

namespace {

using Aurora::Discovery::StreamHost;
using Supervisor = Aurora::Session::SessionSupervisor;
namespace Stream = Aurora::Stream;

constexpr int observePoorRouteMs = 8000;
constexpr int retryPreferredLanMs = 30000;
constexpr int switchCooldownMs = 30000;
constexpr int healthyLanMedianMs = 50;
constexpr int waitForRelayMs = 4000;
constexpr int probeDeadlineMs = 7500;
constexpr int comparisonBudgetMs = 12000;
constexpr int maxProbeResponseBytes = 128;
constexpr int maxNearbyRoutes = 4;

} // namespace

void NativeDesktop::initializeRouteQuality()
{
    routeQualityDelay.setSingleShot(true);
    routeQualityDelay.setTimerType(Qt::PreciseTimer);
    routeQualityDeadline.setSingleShot(true);
    routeQualityBudget.setSingleShot(true);
    connect(&routeQualityBudget, &QTimer::timeout, this, [this] {
        if (!routeQuality)
            return;
        routeQuality->remaining.clear();
        routeProbeFinished(std::nullopt);
    });
    connect(&routeQualityDelay, &QTimer::timeout, this, &NativeDesktop::beginRouteQuality);
    connect(&routeQualityDeadline, &QTimer::timeout, this, [this] { routeProbeFinished(std::nullopt); });
    connect(localPresence.get(), &NativeDevicePresence::sessionLanProbed, this, &NativeDesktop::routeLanProbed);
    connect(sessionSupervisor.get(), &Supervisor::connectionChanged, this, &NativeDesktop::updateRouteQuality);
    connect(sessionSupervisor.get(), &Supervisor::phaseChanged, this, [this] {
        if (connection && sessionSupervisor->phase() == Supervisor::Phase::Streaming) {
            connection->streamingConfirmed = true;
            if (connection->clientGranted)
                clientGrantRenewal.start(int(connection->clientGrantDue.remainingTime()));
            if (!connection->resumeCertificateSha256.isEmpty()) {
                connection->resumeCertificateSha256.clear();
                connection->routeSwitchCooldown.setRemainingTime(switchCooldownMs);
                connection->routes.clear();
            }
        }
        updateRouteQuality();
    });
    connect(sessionSupervisor.get(), &Supervisor::privacyScreenChanged, this, &NativeDesktop::updateRouteQuality);
    connect(sessionSupervisor.get(), &Supervisor::videoSettingsChanged, this, &NativeDesktop::updateRouteQuality);
    connect(sessionSupervisor.get(), &Supervisor::routeSwitchDenied, this, [this](const QUuid &attempt) {
        if (connection && connection->attempt == attempt) {
            connection->pendingRouteSwitch.reset();
            connection->routeSwitchCooldown.setRemainingTime(switchCooldownMs);
            updateRouteQuality();
        }
    });
    if (tailnet) {
        connect(tailnet.get(), &aurora::tailscale::EmbeddedStatusClient::openFinished, this,
                [this](quint64 generation, quint64 request, const aurora::tailscale::OpenResult &result) {
            if (!routeQuality || !request || request != routeQuality->opening)
                return;
            routeQuality->opening = 0;
            if (!ownsRouteQuality() || generation != routeQuality->generation ||
                !result.lease || !result.lease->isValid()) {
                if (result.lease)
                    result.lease->close();
                routeProbeFinished(std::nullopt);
                return;
            }
            routeQuality->lease = result.lease;
            launchRouteProbe({result.lease->address(), result.lease->port(), routeQuality->candidate->id,
                              true, connection->certificateSha256});
        });
    }
}

bool NativeDesktop::canMeasureRouteQuality() const
{
    if (!ownsConnection() || (pairingAttempt && pairingAttempt->renewingGrant) ||
        !connection->launched || connection->retryRoute || connection->pendingRouteSwitch ||
        !connection->resumeCertificateSha256.isEmpty() || !connection->prepared || !connection->selected ||
        connection->certificateSha256.size() != Aurora::Pairing::HostCertificateSha256Size ||
        !sessionSupervisor->canChangeRoute()) {
        return false;
    }
    const auto &selected = *connection->selected;
    const bool onTailnet = canPairOverRoute(selected);
    if (selected.publicIpv6 || onTailnet)
        return true;

    const bool tailnetCandidate = !onTailnet && connection->tailnetFallback &&
        canPairOverRoute(*connection->tailnetFallback);
    const bool publicCandidate = connection->certificateSha256 == connection->publicCertificateSha256 &&
        std::any_of(connection->publicRoutes.cbegin(), connection->publicRoutes.cend(), [&selected](const auto &route) {
            return route.endpoint.address != selected.endpoint.address;
        });
    return sessionSupervisor->networkPoor() && (tailnetCandidate || publicCandidate);
}

bool NativeDesktop::ownsRouteQuality() const
{
    if (!routeQuality || !ownsConnection() || connection->attempt != routeQuality->attempt ||
        tailnetGeneration != routeQuality->generation)
        return false;
    switch (routeQuality->mode) {
    case RouteCheckMode::Playing:
        return canMeasureRouteQuality() && (!routeQuality->comparingWeakRoute || sessionSupervisor->networkPoor());
    case RouteCheckMode::Recovery:
        return connection->recoveringRoute && sessionSupervisor->phase() == Supervisor::Phase::Idle;
    }
    return false;
}

void NativeDesktop::updateRouteQuality()
{
    if (routeQuality) {
        if (ownsRouteQuality())
            return;
        const bool recovering = connection && connection->attempt == routeQuality->attempt &&
            routeQuality->mode == RouteCheckMode::Recovery && sessionSupervisor->phase() == Supervisor::Phase::Idle;
        endRouteQuality();
        if (recovering) {
            cancelConnection(tr("连接信息已变化，网络探测已停止。"));
            return;
        }
    }

    if (!canMeasureRouteQuality()) {
        routeQualityDelay.stop();
        return;
    }
    const bool weak = sessionSupervisor->networkPoor();
    const int delay = weak ? int(qMax<qint64>(0, observePoorRouteMs - sessionSupervisor->networkPoorDurationMs()))
                           : retryPreferredLanMs;
    const int next = qMax(delay, int(connection->routeSwitchCooldown.remainingTime()));
    if (!routeQualityDelay.isActive() || (weak && next < routeQualityDelay.remainingTime()))
        routeQualityDelay.start(next);
}

bool NativeDesktop::beginRouteRecovery()
{
    if (!ownsConnection() || !connection->selected || !connection->prepared || connection->recoveringRoute ||
        !connection->resumeCertificateSha256.isEmpty() ||
        connection->certificateSha256.size() != Aurora::Pairing::HostCertificateSha256Size) {
        return false;
    }

    connection->resumeCertificateSha256 = connection->certificateSha256;
    connection->recoveringRoute = true;
    reportConnection(tr("串流链路已断开，正在重新验证备用路径…"), false);
    if (beginHostIdentity(Aurora::Pairing::RequestKind::HostRoutes))
        return true;
    if (connection) {
        connection->recoveringRoute = false;
        connection->resumeCertificateSha256.clear();
    }
    return false;
}

void NativeDesktop::beginRecoveryRouteQuality()
{
    if (!ownsConnection() || !connection->recoveringRoute || !connection->selected || !connection->prepared ||
        sessionSupervisor->phase() != Supervisor::Phase::Idle) {
        cancelConnection(tr("连接信息已变化，串流未恢复。"));
        return;
    }
    auto &check = routeQuality.emplace();
    check.mode = RouteCheckMode::Recovery;
    check.attempt = connection->attempt;
    check.generation = tailnetGeneration;
    check.current = Stream::RouteQuality{0, 0};

    const auto &lost = *connection->selected;
    const auto consider = [&](const StreamHost &route) {
        if ((route.endpoint.address != lost.endpoint.address || route.endpoint.port != lost.endpoint.port ||
             route.viaEmbeddedTailnet != lost.viaEmbeddedTailnet) &&
            Aurora::Discovery::sameHostIdentity(route.id, lost.id)) {
            check.remaining.enqueue(route);
        }
    };
    if (connection->preferredLan)
        consider(*connection->preferredLan);
    if (connection->tailnetFallback)
        consider(*connection->tailnetFallback);
    if (connection->certificateSha256 == connection->publicCertificateSha256) {
        for (const auto &route : connection->publicRoutes)
            consider(route);
    }

    routeQualityBudget.start(comparisonBudgetMs);
    if (!probeSessionLanForRouteQuality())
        probeNextRoute();
}

void NativeDesktop::beginRouteQuality()
{
    if (!canMeasureRouteQuality() || routeQuality)
        return;
    const bool weak = sessionSupervisor->networkPoor();
    if (weak && sessionSupervisor->networkPoorDurationMs() < observePoorRouteMs) {
        updateRouteQuality();
        return;
    }
    auto &check = routeQuality.emplace();
    check.attempt = connection->attempt;
    check.generation = tailnetGeneration;
    check.comparingWeakRoute = weak;
    const auto &selected = *connection->selected;
    const bool onTailnet = canPairOverRoute(selected);
    const auto &lan = connection->preferredLan;
    if ((selected.publicIpv6 || onTailnet) && lan &&
        Aurora::Discovery::sameHostIdentity(lan->id, selected.id))
        check.remaining.enqueue(*lan);

    if (weak) {
        const auto &fallback = connection->tailnetFallback;
        if (!onTailnet && fallback && canPairOverRoute(*fallback) &&
            Aurora::Discovery::sameHostIdentity(fallback->id, selected.id))
            check.remaining.enqueue(*fallback);
    }
    if (connection->certificateSha256 == connection->publicCertificateSha256 &&
        (weak || (onTailnet && !connection->comparedPublicRoutes))) {
        for (const auto &route : connection->publicRoutes) {
            if (route.endpoint.address != selected.endpoint.address)
                check.remaining.enqueue(route);
        }
    }
    routeQualityBudget.start(comparisonBudgetMs);
    if (!(selected.publicIpv6 || onTailnet) || !probeSessionLanForRouteQuality())
        startRouteComparison();
}

bool NativeDesktop::probeSessionLanForRouteQuality()
{
    localPresence->update(tailnetStatus, embeddedSelected, false);
    routeQuality->lanProbe = localPresence->probeSessionLan(connection->peerId);
    return routeQuality->lanProbe != 0;
}

void NativeDesktop::routeLanProbed(quint64 request, const QList<QHostAddress> &addresses)
{
    if (!routeQuality || routeQuality->lanProbe != request)
        return;
    routeQuality->lanProbe = 0;
    if (!ownsRouteQuality()) {
        updateRouteQuality();
        return;
    }

    const auto &selected = *connection->selected;
    const auto candidatePort = Aurora::Discovery::BundledSunshinePort;
    auto &remaining = routeQuality->remaining;
    int nearby = 0;
    for (const auto &address : addresses) {
        if ((address == selected.endpoint.address && candidatePort == selected.endpoint.port) ||
            std::any_of(remaining.cbegin(), remaining.cend(), [&address, candidatePort](const StreamHost &known) {
                return known.endpoint.address == address && known.endpoint.port == candidatePort;
            })) {
            continue;
        }
        remaining.insert(nearby, StreamHost{{address, candidatePort}, selected.id, selected.name});
        if (++nearby == maxNearbyRoutes)
            break;
    }
    if (routeQuality->mode == RouteCheckMode::Recovery)
        probeNextRoute();
    else
        startRouteComparison();
}

void NativeDesktop::startRouteComparison()
{
    if (routeQuality->remaining.isEmpty()) {
        endRouteQuality(true);
        updateRouteQuality();
        return;
    }
    auto current = connection->prepared->route;
    current.certificateSha256 = connection->certificateSha256;
    launchRouteProbe(current);
}

void NativeDesktop::launchRouteProbe(const Stream::Route &route)
{
    Stream::Request request;
    request.operation = Stream::Operation::RouteProbe;
    request.route = route;
    const auto built = Stream::makeCommand(connection->prepared->executable, request);
    if (!built.command) {
        routeProbeFinished(std::nullopt);
        return;
    }

    auto &check = *routeQuality;
    check.output.clear();
    check.worker = std::make_unique<Stream::MoonlightProcess>();
    const QPointer<Stream::MoonlightProcess> worker(check.worker.get());
    connect(worker.data(), &QProcess::readyReadStandardOutput, this, [this, worker] {
        if (!worker || !routeQuality || routeQuality->worker.get() != worker.data())
            return;
        if (worker->bytesAvailable() > maxProbeResponseBytes - routeQuality->output.size()) {
            routeProbeFinished(std::nullopt);
            return;
        }
        routeQuality->output += worker->readAllStandardOutput();
    }, Qt::QueuedConnection);
    connect(worker.data(), &QProcess::errorOccurred, this, [this, worker](QProcess::ProcessError error) {
        if (worker && routeQuality && routeQuality->worker.get() == worker.data() && error == QProcess::FailedToStart)
            routeProbeFinished(std::nullopt);
    }, Qt::QueuedConnection);
    connect(worker.data(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, worker](int code, QProcess::ExitStatus status) {
        if (!worker || !routeQuality || routeQuality->worker.get() != worker.data())
            return;
        const auto &output = routeQuality->output;
        const bool bounded = worker->bytesAvailable() <= maxProbeResponseBytes - output.size();
        const auto sample = bounded && status == QProcess::NormalExit && code == 0
            ? Stream::parseRouteQuality(output + worker->readAllStandardOutput()) : std::nullopt;
        routeProbeFinished(sample);
    }, Qt::QueuedConnection);

    routeQualityDeadline.start(probeDeadlineMs);
    if (!worker->launch(*built.command))
        routeProbeFinished(std::nullopt);
}

void NativeDesktop::routeProbeFinished(std::optional<Stream::RouteQuality> sample)
{
    if (!ownsRouteQuality()) {
        updateRouteQuality();
        return;
    }
    auto &check = *routeQuality;
    routeQualityDeadline.stop();
    if (check.opening && tailnet)
        tailnet->cancelOpen(std::exchange(check.opening, 0));
    check.worker.reset();
    if (auto lease = std::exchange(check.lease, {}))
        lease->close();

    if (!check.candidate) {
        if (!sample) {
            const bool recovering = check.mode == RouteCheckMode::Recovery;
            endRouteQuality(true);
            if (recovering)
                cancelConnection(tr("没有能通过验证的备用串流路径。"));
            else
                updateRouteQuality();
            return;
        }
        check.current = *sample;
    } else if (sample) {
        const bool healthyLan = check.testingLan && sample->successfulChecks == 3 &&
            sample->medianMilliseconds <= healthyLanMedianMs;
        const bool better = Stream::betterRouteQuality(*check.current, *sample) &&
            (!check.bestQuality || Stream::betterRouteQuality(*check.bestQuality, *sample));
        if ((healthyLan && !Stream::betterRouteQuality(*sample, *check.current)) ||
            ((!check.testingLan || check.mode == RouteCheckMode::Recovery) && better)) {
            check.best = check.candidate;
            check.bestQuality = sample;
        }
        if (healthyLan)
            check.remaining.clear();
    }
    probeNextRoute();
}

void NativeDesktop::probeNextRoute()
{
    auto &check = *routeQuality;
    while (!check.remaining.isEmpty()) {
        auto candidate = check.remaining.dequeue();
        if (!Aurora::Discovery::sameHostIdentity(candidate.id, connection->selected->id))
            continue;
        check.testingLan = !candidate.publicIpv6 && !canPairOverRoute(candidate);
        check.testedPublicRoute = check.testedPublicRoute || candidate.publicIpv6;

        check.candidate = std::move(candidate);
        if (!check.candidate->viaEmbeddedTailnet) {
            launchRouteProbe({check.candidate->endpoint.address, check.candidate->endpoint.port,
                              check.candidate->id, false, connection->certificateSha256});
            return;
        }
        if (tailnet)
            check.opening = tailnet->requestOpen(check.generation, connection->peerId, check.candidate->endpoint.address);
        if (check.opening) {
            routeQualityDeadline.start(waitForRelayMs);
            return;
        }
    }

    const auto candidate = check.best;
    const auto attempt = check.attempt;
    const auto mode = check.mode;
    if (mode == RouteCheckMode::Playing && check.testedPublicRoute && canPairOverRoute(*connection->selected))
        connection->comparedPublicRoutes = true;
    if (candidate && !candidate->publicIpv6 && !canPairOverRoute(*candidate))
        connection->preferredLan = candidate;
    endRouteQuality(true);
    if (!candidate) {
        if (mode == RouteCheckMode::Recovery)
            cancelConnection(tr("没有能通过验证的备用串流路径。"));
        else
            updateRouteQuality();
        return;
    }

    auto &active = *connection;
    if (mode == RouteCheckMode::Recovery) {
        active.recoveringRoute = false;
        active.routes.clear();
        active.routes.enqueue(*candidate);
        reportConnection(tr("正在通过已验证的备用路径恢复串流…"), false);
        if (connection && connection->attempt == attempt)
            prepareNextRoute();
        return;
    }
    active.pendingRouteSwitch = *candidate;
    if (!sessionSupervisor->requestRouteSwitch(attempt) && connection && connection->attempt == attempt) {
        connection->pendingRouteSwitch.reset();
        updateRouteQuality();
    }
}

void NativeDesktop::endRouteQuality(bool comparisonFinished)
{
    routeQualityDelay.stop();
    routeQualityDeadline.stop();
    routeQualityBudget.stop();
    auto check = std::exchange(routeQuality, std::nullopt);
    if (!check)
        return;
    if (comparisonFinished && check->mode == RouteCheckMode::Playing && connection &&
        connection->attempt == check->attempt) {
        connection->routeSwitchCooldown.setRemainingTime(switchCooldownMs);
    }
    if (check->lanProbe)
        localPresence->cancelSessionLanProbe(check->lanProbe);
    if (check->opening && tailnet)
        tailnet->cancelOpen(check->opening);
    check->worker.reset();
    if (check->lease)
        check->lease->close();
}
