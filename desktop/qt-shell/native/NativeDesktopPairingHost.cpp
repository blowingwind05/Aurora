// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"

#include <utility>

namespace {
using Aurora::Pairing::EmbeddedHostContext;
using Aurora::Pairing::EmbeddedPairingHost;
using Aurora::Pairing::EnrollmentResult;
using Aurora::Pairing::Provider;
using Aurora::Sunshine::LifecycleController;
using aurora::tailscale::BackendState;

bool hasValidHostIdentity(const LifecycleController::State &state)
{
    return state.phase == LifecycleController::Phase::Ready && !state.settingsSession.isEmpty() &&
        state.adminIdentity.port && state.adminIdentity.certificateSha256.size() == 32;
}
}

void NativeDesktop::initializePairingHost()
{
    if (!host || !tailnet || !tailnet->isAvailable())
        return;
    embeddedPairingHost = std::make_unique<EmbeddedPairingHost>(*tailnet, *host, [this] {
        return pairingHostContext;
    });
    connect(embeddedPairingHost.get(), &EmbeddedPairingHost::stateChanged, this,
            &NativeDesktop::publishHostState, Qt::QueuedConnection);
}

void NativeDesktop::refreshPairingHost(bool confirmed)
{
    if (!embeddedPairingHost)
        return;
    const auto state = host->state();
    if (exitAfterSession || !embeddedSelected || tailnetStatus.state != BackendState::Connected ||
        !tailnetStatus.account || tailnetStatus.localAddress.protocol() != QAbstractSocket::IPv4Protocol ||
        !hasValidHostIdentity(state)) {
        retirePairingHost();
        return;
    }

    const auto &account = *tailnetStatus.account;
    EmbeddedHostContext next;
    next.scope = {Provider::Embedded, account.ownerId, account.nodeId, state.adminIdentity.port,
                  state.adminIdentity.certificateSha256};
    next.account = account;
    next.localAddress = tailnetStatus.localAddress;
    next.accountEpoch = quint64(accountRevision);
    next.helperGeneration = tailnetGeneration;
    next.sunshineSession = state.settingsSession;
    const bool same = pairingHostContext && pairingHostContext->accountEpoch == next.accountEpoch &&
        pairingHostContext->sunshineSession == next.sunshineSession &&
        pairingHostContext->localAddress == next.localAddress &&
        Aurora::Pairing::approvalId(pairingHostContext->scope) == Aurora::Pairing::approvalId(next.scope);
    next.enrollment = same && !confirmed ? pairingHostContext->enrollment
        : Aurora::Pairing::enrollLocalHost(directory, next.scope, confirmed);
    pairingHostContext = std::move(next);
    embeddedPairingHost->contextChanged();

    if (pairingHostContext->enrollment != EnrollmentResult::Approved || tailnetBusy || pendingAdvertisement ||
        (connection && !connection->launched))
        return;
    using Phase = EmbeddedPairingHost::Phase;
    const auto phase = embeddedPairingHost->phase();
    if (phase == Phase::Failed && (!same || confirmed || pairingHostRetry.hasExpired(15000)))
        embeddedPairingHost->stop();
    if (embeddedPairingHost->phase() == Phase::Stopped) {
        pairingHostRetry.start();
        embeddedPairingHost->start();
    }
}

void NativeDesktop::retirePairingHost()
{
    pairingHostContext.reset();
    if (embeddedPairingHost && embeddedPairingHost->phase() != EmbeddedPairingHost::Phase::Stopped)
        embeddedPairingHost->stop();
}

QString NativeDesktop::pairingHostConfirmation() const
{
    if (!pairingHostContext || pairingHostContext->enrollment != EnrollmentResult::NeedsConfirmation)
        return {};
    return pairingHostContext->sunshineSession + QLatin1Char('/') + QString::number(pairingHostContext->accountEpoch);
}

QString NativeDesktop::pairingHostFeedback() const
{
    if (!pairingHostContext)
        return {};
    switch (pairingHostContext->enrollment) {
    case EnrollmentResult::NeedsConfirmation:
        return tr("账号或被控证书已变更，请确认当前账号的设备访问权限。");
    case EnrollmentResult::Unavailable:
        return tr("暂时无法读取本机自动配对授权。");
    case EnrollmentResult::Approved:
        return embeddedPairingHost->error();
    }
    return {};
}
