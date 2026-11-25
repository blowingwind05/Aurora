// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledHost.h"

#include <QList>

namespace Aurora::Sunshine {

struct PortFamily
{
    QList<quint16> tcp;
    QList<quint16> udp;
};

enum class PortProtocol { Tcp, Udp };

struct PortCheck
{
    enum class Status { Available, Occupied, Unavailable };
    Status status = Status::Available;
    PortProtocol protocol = PortProtocol::Tcp;
    quint16 port = 0;

    QString errorMessage() const;
};

PortFamily bundledPorts();
PortCheck checkPorts(HostPlatform platform, const PortFamily &ports);

} // namespace Aurora::Sunshine
