// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "discovery/Discovery.h"
#include "discovery/serverinfo/ServerInfoClient.h"
#include "presence/UdpPresenceTransport.h"
#include "adaptors/tailscale/EmbeddedStatusClient.h"

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
    bool scanning() const { return lanRequest || peersRequest || embeddedDeadline.isActive() || bool(routeSurvey); }
    const Aurora::Discovery::NetworkSnapshot &network() const { return currentNetwork; }
    const Aurora::Discovery::LanSnapshot &lan() const { return lanDevices; }
    const Aurora::Discovery::TailnetSnapshot &tailnet() const { return tailnetDevices; }
    Aurora::Discovery::ServerInfo::Client &routeClient() { return serverInfo; }
    void setSessionActive(bool active);
    quint64 probeSessionLan(const QString &peerId);
    void cancelSessionLanProbe(quint64 request);

signals:
    void changed();
    void sessionLanProbed(quint64 request, const QList<QHostAddress> &addresses);

private:
    void receiveScan(quint64 request, const Aurora::Presence::ScanResult &result);
    void sendEmbeddedProbes();
    void finishEmbeddedScan();
    void clearEmbeddedScan();
    void startRouteSurvey();
    void continueRouteSurvey();
    void receiveRouteBatch(quint64 request, const Aurora::Discovery::ServerInfo::BatchResult &result);
    void receiveProbeLease(quint64 generation, quint64 request, const aurora::tailscale::OpenResult &result);
    void finishRouteSurvey(bool keep);

    struct RouteSurvey
    {
        QQueue<QPair<QString, QHostAddress>> targets;
        QList<Aurora::Discovery::ServerInfo::Candidate> candidates;
        QList<std::shared_ptr<aurora::tailscale::StreamLease>> leases;
        QList<Aurora::Discovery::StreamHost> lan;
        QList<Aurora::Discovery::StreamHost> tailnet;
        QHostAddress openingAddress;
        quint64 opening = 0;
        quint64 batch = 0;
    };

    const QString directory;
    std::unique_ptr<Aurora::Presence::UdpPresenceTransport> transport;
    Aurora::Discovery::NetworkSnapshot currentNetwork;
    Aurora::Discovery::LanSnapshot lanDevices;
    Aurora::Discovery::TailnetSnapshot tailnetDevices;
    std::optional<aurora::tailscale::AccountIdentity> account;
    quint64 accountGeneration = 0;
    quint64 lanRequest = 0;
    QString sessionLanPeer;
    quint64 peersRequest = 0;
    bool hosting = false;
    bool embeddedMode = false;
    bool sessionActive = false;
    QUuid localIdentity;
    QPointer<aurora::tailscale::EmbeddedStatusClient> embeddedClient;
    QMetaObject::Connection embeddedReplies;
    QMetaObject::Connection embeddedOpens;
    aurora::tailscale::Status embeddedStatus;
    QHash<quint64, Aurora::Presence::EmbeddedProbe> embeddedProbes;
    QQueue<QPair<QString, QHostAddress>> embeddedTargets;
    QList<Aurora::Discovery::PresentDevice> embeddedDevices;
    QTimer embeddedDeadline;
    quint64 embeddedGeneration = 0;
    Aurora::Discovery::ServerInfo::Client serverInfo;
    QTimer routeDeadline;
    std::optional<RouteSurvey> routeSurvey;
};
