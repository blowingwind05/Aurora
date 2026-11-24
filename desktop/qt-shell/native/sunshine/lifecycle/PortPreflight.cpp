// SPDX-License-Identifier: GPL-3.0-only
#include "PortPreflight.h"
#include "SunshineConfig.h"

#if defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <QtEndian>

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#elif defined(Q_OS_UNIX)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Aurora::Sunshine {
namespace {

using Status = PortCheck::Status;

#if defined(Q_OS_WIN)

template <typename Table, typename Query>
PortCheck inspectTable(ULONG family, PortProtocol protocol, const QList<quint16> &ports, Query query)
{
    if (ports.isEmpty())
        return {};
    constexpr DWORD maxTableBytes = 32 * 1024 * 1024;
    constexpr int attempts = 4;
    std::vector<DWORD> buffer;
    DWORD size = 0;
    for (int ri = 0; ri < attempts; ++ri) {
        const auto rc = query(buffer.empty() ? nullptr : buffer.data(), &size, family);
        if (rc == ERROR_INSUFFICIENT_BUFFER && size > 0 && size <= maxTableBytes) {
            buffer.resize((size + sizeof(DWORD) - 1) / sizeof(DWORD));
            continue;
        }
        if (rc == ERROR_NOT_SUPPORTED && family == AF_INET6)
            return {};
        constexpr auto rowsOffset = offsetof(Table, table);
        using Row = std::remove_reference_t<decltype(std::declval<Table>().table[0])>;
        if (rc != NO_ERROR || size < rowsOffset || size > buffer.size() * sizeof(DWORD))
            break;

        const auto *bytes = reinterpret_cast<const unsigned char *>(buffer.data());
        DWORD count = 0;
        std::memcpy(&count, bytes + offsetof(Table, dwNumEntries), sizeof(count));
        if (count > (size - rowsOffset) / sizeof(Row))
            break;
        for (DWORD i = 0; i < count; ++i) {
            Row row{};
            std::memcpy(&row, bytes + rowsOffset + i * sizeof(Row), sizeof(row));
            const auto port = qFromBigEndian(quint16(row.dwLocalPort));
            if (ports.contains(port))
                return {Status::Occupied, protocol, port};
        }
        return {};
    }
    return {Status::Unavailable, protocol, 0};
}

PortCheck inspectWindows(const PortFamily &ports)
{
    const auto tcp = [](void *entries, DWORD *size, ULONG family) {
        return GetExtendedTcpTable(entries, size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    };
    const auto udp = [](void *entries, DWORD *size, ULONG family) {
        return GetExtendedUdpTable(entries, size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
    };
    for (const auto result : {
             inspectTable<MIB_TCPTABLE_OWNER_PID>(AF_INET, PortProtocol::Tcp, ports.tcp, tcp),
             inspectTable<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, PortProtocol::Tcp, ports.tcp, tcp),
             inspectTable<MIB_UDPTABLE_OWNER_PID>(AF_INET, PortProtocol::Udp, ports.udp, udp),
             inspectTable<MIB_UDP6TABLE_OWNER_PID>(AF_INET6, PortProtocol::Udp, ports.udp, udp)}) {
        if (result.status != Status::Available)
            return result;
    }
    return {};
}

#elif defined(Q_OS_UNIX)

PortCheck probeIpv4(PortProtocol protocol, quint16 port)
{
    const bool tcp = protocol == PortProtocol::Tcp;
    const int descriptor = ::socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (descriptor < 0)
        return {Status::Unavailable, protocol, port};

    const int yes = 1;
    sockaddr_in address{};
#if defined(Q_OS_MACOS)
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool available = ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0 &&
        (!tcp || ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0) &&
        ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
        (!tcp || ::listen(descriptor, 1) == 0);
    const int error = errno;
    ::close(descriptor);
    if (available)
        return {};
    return {error == EADDRINUSE ? Status::Occupied : Status::Unavailable, protocol, port};
}

PortCheck probeMac(const PortFamily &ports)
{
    for (const auto port : ports.tcp) {
        const auto result = probeIpv4(PortProtocol::Tcp, port);
        if (result.status != Status::Available)
            return result;
    }
    for (const auto port : ports.udp) {
        const auto result = probeIpv4(PortProtocol::Udp, port);
        if (result.status != Status::Available)
            return result;
    }
    return {};
}

#endif

} // namespace

QString PortCheck::errorMessage() const
{
    if (status == Status::Available)
        return {};
    if (status == Status::Occupied) {
        const auto type = protocol == PortProtocol::Tcp ? QStringLiteral("TCP") : QStringLiteral("UDP");
        return QStringLiteral("Aurora 内置 Sunshine 所需的端口已被其他程序占用（%1 %2）。").arg(type).arg(port);
    }
    return QStringLiteral("无法检查 Aurora 内置 Sunshine 所需的端口。");
}

PortFamily bundledPorts()
{
    const auto base = managedOptions().value(QStringLiteral("port")).toUShort();
    return {{quint16(base - 5), base, quint16(base + 1), quint16(base + 21)},
            {quint16(base + 9), quint16(base + 10), quint16(base + 11), quint16(base + 13)}};
}

PortCheck checkPorts(HostPlatform platform, const PortFamily &ports)
{
#if defined(Q_OS_WIN)
    if (platform == HostPlatform::Windows)
        return inspectWindows(ports);
#elif defined(Q_OS_UNIX)
    if (platform == HostPlatform::MacOS)
        return probeMac(ports);
#else
    Q_UNUSED(platform);
    Q_UNUSED(ports);
#endif
    return {Status::Unavailable, PortProtocol::Tcp, 0};
}

} // namespace Aurora::Sunshine
