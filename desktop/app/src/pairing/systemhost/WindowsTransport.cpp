// SPDX-License-Identifier: GPL-3.0-only
#include <winsock2.h>
#include <qt_windows.h>
#include <iphlpapi.h>

#include "WindowsTransport.h"

#include <QDir>
#include <QFileInfo>
#include <QTcpServer>

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Aurora::Pairing::SystemHostDetail {
namespace {

constexpr auto protectedPipe = L"\\\\.\\pipe\\protectedprefix\\administrators\\tailscale\\tailscaled";
using Handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>;

bool localAbsolutePath(const QString &path)
{
    const auto normalized = QDir::fromNativeSeparators(path);
    if (normalized.size() < 3 || normalized.at(1) != QLatin1Char(':') ||
        normalized.at(2) != QLatin1Char('/') || normalized.contains(QChar::Null))
        return false;
    const auto drive = normalized.at(0).unicode();
    return (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
}

bool hasConnection(DWORD pid, quint16 sourcePort, quint16 destinationPort)
{
    constexpr auto headerBytes = offsetof(MIB_TCPTABLE_OWNER_PID, table);
    constexpr DWORD maxBytes = 4 * 1024 * 1024;
    DWORD bytes = 0;
    for (int attempt = 0; attempt < 3 && bytes <= maxBytes; ++attempt) {
        QByteArray buffer(int(bytes), Qt::Uninitialized);
        const auto result = GetExtendedTcpTable(bytes ? buffer.data() : nullptr, &bytes, FALSE,
            AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != NO_ERROR || bytes < headerBytes || bytes > DWORD(buffer.size()))
            return false;
        DWORD rows = 0;
        std::memcpy(&rows, buffer.constData(), sizeof rows);
        if (rows > (bytes - headerBytes) / sizeof(MIB_TCPROW_OWNER_PID))
            return false;
        for (DWORD index = 0; index < rows; ++index) {
            MIB_TCPROW_OWNER_PID row{};
            std::memcpy(&row, buffer.constData() + headerBytes + index * sizeof(row), sizeof row);
            if (row.dwOwningPid == pid && row.dwState == MIB_TCP_STATE_ESTAB &&
                row.dwLocalAddr == htonl(INADDR_LOOPBACK) && row.dwRemoteAddr == htonl(INADDR_LOOPBACK) &&
                ntohs(u_short(row.dwLocalPort)) == sourcePort &&
                ntohs(u_short(row.dwRemotePort)) == destinationPort)
                return true;
        }
        return false;
    }
    return false;
}

struct RelayJob
{
    Handle job{CreateJobObjectW(nullptr, nullptr), CloseHandle};
    std::vector<std::max_align_t> storage;
    STARTUPINFOEXW startup{};
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;

    ~RelayJob()
    {
        if (attributes)
            DeleteProcThreadAttributeList(attributes);
    }

    bool initialize()
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits))
            return false;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size)
            return false;
        storage.resize((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
            return false;
        attributes = list;
        return UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &jobHandle, sizeof jobHandle, nullptr, nullptr);
    }

    HANDLE jobHandle = job.get();
};

} // namespace

QString runningProgram(const aurora::tailscale::system::Installation &installation)
{
    if (installation.service != aurora::tailscale::system::ServiceState::Running ||
        !localAbsolutePath(installation.executable))
        return {};
    const QFileInfo file(installation.executable);
    const auto canonical = file.canonicalFilePath();
    return file.isFile() && localAbsolutePath(canonical) ? canonical : QString{};
}

bool prepareProcess(QProcess &process, const QString &program)
{
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const auto rootSize = GetSystemWindowsDirectoryW(buffer.data(), UINT(buffer.size()));
    if (!rootSize || rootSize >= buffer.size())
        return false;
    const auto root = QString::fromWCharArray(buffer.data(), int(rootSize));
    const auto systemSize = GetSystemDirectoryW(buffer.data(), UINT(buffer.size()));
    if (!systemSize || systemSize >= buffer.size())
        return false;
    const auto system = QString::fromWCharArray(buffer.data(), int(systemSize));
    if (!localAbsolutePath(root) || !localAbsolutePath(system))
        return false;

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("SystemRoot"), root);
    environment.insert(QStringLiteral("WINDIR"), root);
    environment.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(QFileInfo(program).absolutePath()) +
        QLatin1Char(';') + QDir::toNativeSeparators(system));
    process.setProcessEnvironment(environment);
    process.setProgram(program);
    process.setWorkingDirectory(QFileInfo(program).absolutePath());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    return true;
}

bool bindToParentJob(QProcess &process)
{
    auto job = std::make_shared<RelayJob>();
    if (!job->initialize())
        return false;
    process.setCreateProcessArgumentsModifier([job](QProcess::CreateProcessArguments *args) {
        job->startup.StartupInfo = *args->startupInfo;
        job->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        job->startup.lpAttributeList = job->attributes;
        args->startupInfo = &job->startup.StartupInfo;
        args->flags |= CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    });
    return true;
}

bool listenLoopback(QTcpServer &server)
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return false;
    const auto socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const BOOL exclusive = TRUE;
    const bool listening = socket != INVALID_SOCKET &&
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof exclusive) == 0 &&
        ::bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0 &&
        ::listen(socket, 8) == 0 && server.setSocketDescriptor(qintptr(socket));
    if (!listening && socket != INVALID_SOCKET)
        closesocket(socket);
    WSACleanup();
    return listening;
}

bool tcpOwnedByProcess(quint32 pid, quint16 sourcePort, quint16 destinationPort)
{
    return pid && sourcePort && destinationPort && hasConnection(pid, sourcePort, destinationPort);
}

std::optional<quint32> loopbackOwner(quint16 sourcePort, quint16 destinationPort)
{
    if (!sourcePort || !destinationPort)
        return std::nullopt;
    // Use the protected LocalAPI server PID. The SCM service PID can be its supervisor.
    const auto pipe = CreateFileW(protectedPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return std::nullopt;
    const Handle handle(pipe, CloseHandle);
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(handle.get(), &pid) || !tcpOwnedByProcess(pid, sourcePort, destinationPort))
        return std::nullopt;
    return pid;
}

} // namespace Aurora::Pairing::SystemHostDetail
