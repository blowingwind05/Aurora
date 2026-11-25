// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SystemStatus.h"
#include "WindowsInstallation.h"

#include <QObject>
#include <QTcpSocket>
#include <memory>

namespace Aurora::Pairing {

struct SystemHostAccount
{
    aurora::tailscale::AccountIdentity identity;
    QHostAddress localAddress;
};

struct SystemHostPeer
{
    QString nodeId;
    QString name;
    QHostAddress address; // Same source to match in the managed Sunshine pending request.
};

enum class SystemHostStop {
    Cancelled, AuthorizationChanged, InstallationUnavailable, IdentityMismatch,
    PortCheckFailed, PortOccupied, ListenFailed, RelayFailed, StartupTimeout
};

// Use on the owning Qt event-loop thread. The coordinator owns enrollment and approval.
class SystemPairingHost final : public QObject
{
    Q_OBJECT

public:
    using Locate = std::function<aurora::tailscale::system::Installation()>;
    using StillCurrent = std::function<bool()>;
    using FindOwner = std::function<std::optional<quint32>(quint16 sourcePort, quint16 destinationPort)>;

    explicit SystemPairingHost(QObject *parent = nullptr);
    SystemPairingHost(Locate locate, FindOwner findOwner, QObject *parent = nullptr);
    ~SystemPairingHost() override;

    // Pass only the verified system-provider account. Advance the generation and clear
    // the account during transitions. Older updates are ignored.
    void setAuthorization(quint64 generation, std::optional<SystemHostAccount> account);
    // Zero means unauthorized or busy. Startup completes asynchronously.
    quint64 start(quint64 generation);
    void stop(quint64 host);

    // Call immediately before approval, after matching the managed Sunshine pending
    // request. False means the request could not start. Failure closes the channel.
    bool revalidate(quint64 host, quint64 connection);
    // Obtain on the host thread after revalidated. The returned predicate only
    // reads the connection's revocation flag and deadline and may run on a worker.
    StillCurrent approvalGuard(quint64 host, quint64 connection) const;
    void closeConnection(quint64 host, quint64 connection);

signals:
    void listening(quint64 host, quint64 generation);
    // The adapter has consumed the PROXY header and pairing request. Write the offer
    // to this borrowed socket and leave reads to the adapter. It owns the socket.
    void authenticated(quint64 host, quint64 generation, quint64 connection,
        const Aurora::Pairing::SystemHostPeer &peer, QTcpSocket *channel);
    void revalidated(quint64 host, quint64 generation, quint64 connection);
    void connectionClosed(quint64 host, quint64 connection);
    void stopped(quint64 host, quint64 generation, Aurora::Pairing::SystemHostStop reason);

private:
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace Aurora::Pairing
