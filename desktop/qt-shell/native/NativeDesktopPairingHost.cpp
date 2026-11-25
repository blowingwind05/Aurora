// SPDX-License-Identifier: GPL-3.0-only
#include "NativeDesktop.h"

#include <QRegularExpression>

#include <algorithm>
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

bool isManual(const Aurora::Sunshine::PendingPairing &row)
{
    static const QRegularExpression automatic(QStringLiteral(R"(\Aaurora-[0-9a-f]{32}\z)"));
    return !automatic.match(row.name).hasMatch();
}

bool samePending(const Aurora::Sunshine::PendingPairing &a, const Aurora::Sunshine::PendingPairing &b)
{
    return a.id == b.id && a.name == b.name && a.address == b.address &&
        a.sourceIpv4 == b.sourceIpv4 && a.sourcePort == b.sourcePort;
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

void NativeDesktop::readManualPairings()
{
    if (!host || !hostSettings || hostSettingsSession.isEmpty() ||
        !snapshot.value(QStringLiteral("windowVisible")).toBool())
        return;
    const auto session = hostSettingsSession;
    host->readPendingPairings(session, [this, session](Aurora::Sunshine::BundledAdmin::PendingResult result) {
        if (session != hostSettingsSession || !result)
            return;
        auto &rows = result.pairings;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto &row) { return !isManual(row); }), rows.end());
        const bool same = std::equal(rows.cbegin(), rows.cend(), manualPairings.cbegin(), manualPairings.cend(), samePending);
        if (!same) {
            manualPairings = std::move(rows);
            publishHostState();
        }
    });
}

void NativeDesktop::approveManualPairing(const QJsonObject &request)
{
    using Aurora::Sunshine::BundledAdmin;
    using Aurora::Sunshine::SupervisorPairingApproval;
    const auto id = request.value(QStringLiteral("pairingId")).toString();
    const auto pin = request.value(QStringLiteral("pin")).toString();
    static const QRegularExpression fourDigits(QStringLiteral(R"(\A[0-9]{4}\z)"));
    if (!fourDigits.match(pin).hasMatch()) {
        hostSettingsFeedback = tr("请输入主控端显示的四位 PIN。");
        publishHostState();
        return;
    }
    const auto selected = std::find_if(manualPairings.cbegin(), manualPairings.cend(), [&id](const auto &row) {
        return row.id == id;
    });
    if (selected == manualPairings.cend())
        return;
    const auto session = hostSettingsSession;
    const auto expected = *selected;
    if (!host->readPendingPairings(session, [this, session, expected, pin](BundledAdmin::PendingResult result) {
        if (session != hostSettingsSession)
            return;
        const auto count = std::count_if(result.pairings.cbegin(), result.pairings.cend(), [&expected](const auto &row) {
            return isManual(row) && samePending(row, expected);
        });
        if (!result || count != 1 || !host->approvePairing(session,
                SupervisorPairingApproval::authorized(expected.id, pin, QStringLiteral("Moonlight")),
                [this, session](BundledAdmin::ApprovalResult reply) {
                    if (session != hostSettingsSession)
                        return;
                    hostSettingsFeedback = reply ? tr("Moonlight 已配对。") : reply.message;
                    readManualPairings();
                    publishHostState();
                })) {
            hostSettingsFeedback = tr("配对请求已失效。请在主控端重新发起配对。");
            readManualPairings();
            publishHostState();
        }
    })) {
        hostSettingsFeedback = tr("Sunshine 正在处理另一项操作。请稍后重试。");
        publishHostState();
        return;
    }
    publishHostState();
}
