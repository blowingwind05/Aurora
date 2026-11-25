// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QString>
#include <QUuid>

#include <functional>
#include <optional>

namespace Aurora::Discovery {

inline constexpr quint16 BundledSunshinePort = 48989;

struct Endpoint
{
    QHostAddress address;
    quint16 port = 0;
};

struct StreamHost
{
    Endpoint endpoint;
    QString id;
    QString name;
    bool viaEmbeddedTailnet = false;
    bool encoderProbePending = false;
    bool supportsPower = false;
    bool supportsSessionPrivacy = false;
    bool publicIpv6 = false;
};

struct PresentDevice
{
    QUuid id;
    QHostAddress address;
    QString name;
    QString platform;
    bool hosting = false;
};

struct Device
{
    QString id;
    QString name;
    QString platform;
    QHostAddress address;
    QHostAddress alternateAddress;
    QHostAddress lanAddress;
    bool online = false;
    bool tailnetVisible = false;
    std::optional<StreamHost> lan;
    std::optional<StreamHost> tailnet;

    bool canConnect() const;
    bool supportsPower() const;
};

struct NetworkSnapshot
{
    QHostAddress localAddress;
    qint64 owner = 0;
    QString localId;
    QList<Device> devices;

    bool isAuthenticated() const;
};

struct LanSnapshot
{
    QHostAddress address;
    QList<StreamHost> hosts;
    QList<PresentDevice> devices;
};

using KnownIdentities = QHash<QString, QUuid>;

struct TailnetSnapshot
{
    QList<StreamHost> hosts;
    QList<PresentDevice> devices;
    KnownIdentities known;
};

QList<QHostAddress> tailnetAddresses(const Device &peer);

// The result borrows from network->devices until that list changes.
const Device *findTailnetPeer(const NetworkSnapshot *network, const QHostAddress &source);

// saved must belong to the current owner and local node. This function performs no persistence.
KnownIdentities resolveKnownIdentities(const QList<Device> &peers, const QList<PresentDevice> &present,
                                       const KnownIdentities &saved);

// Returns address hints for the same authenticated node on the current LAN.
QList<QHostAddress> nearbyHostAddresses(const NetworkSnapshot &network, const KnownIdentities &known,
                                       const QString &peerId, const QList<PresentDevice> &lan);

QList<Device> merge(const QList<Device> &peers, const LanSnapshot *lan, const TailnetSnapshot *tailnet,
                    const QLocale &locale = QLocale());

// Desktop consumer policy. The shell must also enforce its current catalog/account epoch.
QList<Device> visibleDevices(const NetworkSnapshot *network, const LanSnapshot *lan,
                             const TailnetSnapshot *tailnet, const QLocale &locale = QLocale());
QJsonObject qtDeviceRow(const Device &device, const QString &wallpaperUrl = {});

bool sameHostIdentity(const QString &first, const QString &second);

enum class RouteRequirement { Stream, Power };

struct RouteSelection
{
    StreamHost route;
    StreamHost current;
};

using RouteProbe = std::function<std::optional<StreamHost>(const StreamHost &)>;

// Calls the supplied probe in preference order. An empty result means no route passed revalidation.
std::optional<RouteSelection> chooseRoute(const Device &device, const RouteProbe &probe,
                                          RouteRequirement requirement = RouteRequirement::Stream);

} // namespace Aurora::Discovery
