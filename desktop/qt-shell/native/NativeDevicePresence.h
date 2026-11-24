// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "discovery/Discovery.h"
#include "presence/UdpPresenceTransport.h"
#include "tailscale/EmbeddedStatusClient.h"

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <memory>

class NativeDevicePresence final : public QObject
{
    Q_OBJECT

public:
    explicit NativeDevicePresence(const QString &directory);

    void update(const aurora::tailscale::Status &status, bool embedded, bool scan);
    void scanEmbedded(aurora::tailscale::EmbeddedStatusClient &client,
                      const aurora::tailscale::Status &status, quint64 generation);
    void setHosting(bool hosting);
    QByteArray announcementPacket(bool canHost) const;
    bool scanning() const { return lanRequest || peersRequest || embeddedDeadline.isActive(); }
    const Aurora::Discovery::NetworkSnapshot &network() const { return currentNetwork; }
    const Aurora::Discovery::LanSnapshot &lan() const { return lanDevices; }
    const Aurora::Discovery::TailnetSnapshot &tailnet() const { return tailnetDevices; }

signals:
    void changed();

private:
    void receiveScan(quint64 request, const Aurora::Presence::ScanResult &result);
    void sendEmbeddedProbes();
    void finishEmbeddedScan();
    void clearEmbeddedScan();

    const QString directory;
    std::unique_ptr<Aurora::Presence::UdpPresenceTransport> transport;
    Aurora::Discovery::NetworkSnapshot currentNetwork;
    Aurora::Discovery::LanSnapshot lanDevices;
    Aurora::Discovery::TailnetSnapshot tailnetDevices;
    std::optional<aurora::tailscale::AccountIdentity> account;
    quint64 accountGeneration = 0;
    quint64 lanRequest = 0;
    quint64 peersRequest = 0;
    bool hosting = false;
    QUuid localIdentity;
    QPointer<aurora::tailscale::EmbeddedStatusClient> embeddedClient;
    QMetaObject::Connection embeddedReplies;
    aurora::tailscale::Status embeddedStatus;
    QHash<quint64, Aurora::Presence::EmbeddedProbe> embeddedProbes;
    QQueue<QPair<QString, QHostAddress>> embeddedTargets;
    QList<Aurora::Discovery::PresentDevice> embeddedDevices;
    QTimer embeddedDeadline;
    quint64 embeddedGeneration = 0;
};
