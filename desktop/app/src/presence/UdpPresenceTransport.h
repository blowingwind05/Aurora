// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "PresenceProbe.h"

#include <QObject>

class QUdpSocket;

namespace Aurora::Presence {

struct LanSubnet
{
    QHostAddress address;
    QHostAddress mask;

    bool isUsable() const;
    QHostAddress broadcastAddress() const;
    bool contains(const QHostAddress &remote) const;
    bool operator==(const LanSubnet &other) const;
};

struct UdpBindings
{
    QHostAddress ipv4{QHostAddress::AnyIPv4};
    QHostAddress ipv6{QHostAddress::AnyIPv6};
};

struct ListenerState
{
    bool ipv4 = false;
    bool ipv6 = false;
};

enum class ScanStatus { Complete, Cancelled, AccessChanged, SocketError };

struct ScanResult
{
    ScanStatus status = ScanStatus::Complete;
    QList<Response> replies;
};

// Use this object and its sockets on the owning Qt event-loop thread.
class UdpPresenceTransport final : public QObject
{
    Q_OBJECT

public:
    explicit UdpPresenceTransport(const Announcement &local, const UdpBindings &bindings = {},
                                  QObject *parent = nullptr);

    // A null binding disables that address family. Each successful bind is exclusive.
    ListenerState startResponding();
    void stopResponding();
    void updateAnnouncement(const QString &name, Platform platform, bool hosting);

    // Pass only the current SYSTEM Tailscale snapshot, or nullptr. Supply an empty LAN list
    // when LAN discovery is disabled. A changed policy invalidates outstanding scan results.
    void updateAccess(const QList<LanSubnet> &lan, const aurora::tailscale::Status *systemTailnet,
                      quint64 accountGeneration);

    // Requests always complete asynchronously, including empty target sets and bind failures.
    quint64 probeLan();
    quint64 probePeers(const QList<QHostAddress> &targets);
    void cancelScan(quint64 requestId);

signals:
    void scanFinished(quint64 requestId, const Aurora::Presence::ScanResult &result);

private:
    struct Plan;
    struct ProbeSocket;
    struct Scan;

    bool allows(const QHostAddress &source) const;
    void drainRequests(QUdpSocket *socket);
    quint64 queueScan(QList<Plan> plans);
    void beginScan(Scan *scan, const QList<Plan> &plans);
    void drainReplies(Scan *scan, ProbeSocket *group);
    void finishIfComplete(Scan *scan);
    void finish(Scan *scan, ScanStatus status);

    Announcement local_;
    const UdpBindings bindings_;
    QUdpSocket *const ipv4_;
    QUdpSocket *const ipv6_;
    QList<LanSubnet> lan_;
    std::optional<aurora::tailscale::AccountIdentity> account_;
    QHostAddress accountAddress_;
    QHash<QString, QString> tailnetPeers_;
    quint64 generation_ = 0;
    quint64 revision_ = 0;
    quint64 nextRequest_ = 0;
    QHash<quint64, Scan *> scans_;
};

} // namespace Aurora::Presence

Q_DECLARE_METATYPE(Aurora::Presence::ScanResult)
