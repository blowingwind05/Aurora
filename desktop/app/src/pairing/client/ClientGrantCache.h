// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QString>

#include <optional>

namespace Aurora::Pairing::Client {

// Kept in memory for one authenticated local account. A receipt only applies
// to the exact host launch and the client certificate that received the grant.
class ClientGrantCache
{
public:
    void clear();
    bool knowsPeer(const QString &peer) const;
    QByteArray hostFingerprint(const QString &peer) const;
    void noteSupport(const QString &peer, const QByteArray &hostFingerprint);
    void remember(const QString &peer, const QByteArray &hostFingerprint, QByteArray instance,
                  QByteArray clientFingerprint, QDeadlineTimer renewBy);
    bool hasRecentGrant(const QString &peer) const;
    std::optional<QDeadlineTimer> reuse(const QString &peer, const QByteArray &hostFingerprint,
        const QByteArray &instance, const QByteArray &clientFingerprint) const;
    void forgetGrant(const QString &peer);

private:
    struct Receipt {
        QByteArray hostFingerprint;
        QByteArray instance;
        QByteArray clientFingerprint;
        QDeadlineTimer renewBy;
    };
    QHash<QString, Receipt> receipts_;
};

} // namespace Aurora::Pairing::Client
