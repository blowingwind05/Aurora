// SPDX-License-Identifier: GPL-3.0-only
#include "ClientGrantCache.h"

#include <utility>

namespace Aurora::Pairing::Client {

void ClientGrantCache::clear()
{
    receipts_.clear();
}

bool ClientGrantCache::knowsPeer(const QString &peer) const
{
    return receipts_.contains(peer);
}

QByteArray ClientGrantCache::hostFingerprint(const QString &peer) const
{
    const auto found = receipts_.constFind(peer);
    return found == receipts_.cend() ? QByteArray{} : found->hostFingerprint;
}

void ClientGrantCache::noteSupport(const QString &peer, const QByteArray &hostFingerprint)
{
    const auto found = receipts_.constFind(peer);
    if (found == receipts_.cend() || found->hostFingerprint != hostFingerprint)
        receipts_.insert(peer, Receipt{hostFingerprint});
}

void ClientGrantCache::remember(const QString &peer, const QByteArray &hostFingerprint, QByteArray instance,
                                QByteArray clientFingerprint, QDeadlineTimer renewBy)
{
    if (instance.isEmpty() || clientFingerprint.isEmpty()) {
        noteSupport(peer, hostFingerprint);
        forgetGrant(peer);
        return;
    }
    receipts_.insert(peer, Receipt{hostFingerprint, std::move(instance), std::move(clientFingerprint), renewBy});
}

bool ClientGrantCache::hasRecentGrant(const QString &peer) const
{
    const auto found = receipts_.constFind(peer);
    return found != receipts_.cend() && !found->instance.isEmpty() && !found->renewBy.hasExpired();
}

std::optional<QDeadlineTimer> ClientGrantCache::reuse(const QString &peer, const QByteArray &hostFingerprint,
    const QByteArray &instance, const QByteArray &clientFingerprint) const
{
    const auto found = receipts_.constFind(peer);
    if (found == receipts_.cend() || found->instance.isEmpty() || found->renewBy.hasExpired() ||
        found->hostFingerprint != hostFingerprint || found->instance != instance ||
        found->clientFingerprint != clientFingerprint) {
        return {};
    }
    return found->renewBy;
}

void ClientGrantCache::forgetGrant(const QString &peer)
{
    auto found = receipts_.find(peer);
    if (found == receipts_.end())
        return;
    found->instance.clear();
    found->clientFingerprint.clear();
    found->renewBy = {};
}

} // namespace Aurora::Pairing::Client
