// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHostAddress>

namespace Aurora::Presence::Detail {

inline QHostAddress normalizeMappedAddress(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const QHostAddress ipv4(address.toIPv4Address());
        if (address.isEqual(ipv4, QHostAddress::ConvertV4MappedToIPv4))
            return ipv4;
    }
    return address;
}

inline bool isEndpointAddress(const QHostAddress &address)
{
    return !address.isNull() && address != QHostAddress::Any &&
        address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6;
}

} // namespace Aurora::Presence::Detail
