// SPDX-License-Identifier: GPL-3.0-only
#include "WindowsInstallation.h"

#include <QFileInfo>
#include <qt_windows.h>
#include <shlobj.h>

#include <array>
#include <memory>
#include <type_traits>

namespace aurora::tailscale::system {
namespace {

constexpr auto serviceName = L"Tailscale";
constexpr auto serviceKey = L"SYSTEM\\CurrentControlSet\\Services\\Tailscale";
constexpr int maxCommandCharacters = 32768;

bool ordinalEquals(const QString &left, const QString &right)
{
    return CompareStringOrdinal(reinterpret_cast<LPCWCH>(left.utf16()), int(left.size()),
        reinterpret_cast<LPCWCH>(right.utf16()), int(right.size()), TRUE) == CSTR_EQUAL;
}

bool separator(QChar ch)
{
    return ch == QLatin1Char('\\') || ch == QLatin1Char('/');
}

bool fullyQualified(const QString &path)
{
    if (path.size() < 2)
        return false;
    if (separator(path.at(0)))
        return separator(path.at(1)) || path.at(1) == QLatin1Char('?');
    const auto drive = path.at(0).toUpper().unicode();
    return path.size() >= 3 && drive >= 'A' && drive <= 'Z' && path.at(0).unicode() <= 127 &&
        path.at(1) == QLatin1Char(':') && separator(path.at(2));
}

QString normalizeSeparators(const QString &path)
{
    QString result;
    result.reserve(path.size());
    for (auto ch : path) {
        if (separator(ch)) {
            if (result.size() > 1 && result.endsWith(QLatin1Char('\\')))
                continue;
            ch = QLatin1Char('\\');
        }
        result.append(ch);
    }
    return result;
}

int rootLength(const QString &path)
{
    const bool device = path.startsWith(QLatin1String("\\\\?\\")) || path.startsWith(QLatin1String("\\\\.\\")) ||
        path.startsWith(QLatin1String("\\??\\"));
    const bool deviceUnc = device && path.mid(4, 4) == QLatin1String("UNC\\");
    if (deviceUnc || (!device && path.startsWith(QLatin1String("\\\\")))) {
        const auto serverEnd = path.indexOf(QLatin1Char('\\'), deviceUnc ? 8 : 2);
        if (serverEnd < 0)
            return int(path.size());
        const auto shareEnd = path.indexOf(QLatin1Char('\\'), serverEnd + 1);
        return shareEnd < 0 ? int(path.size()) : int(shareEnd);
    }
    if (device) {
        const auto end = path.indexOf(QLatin1Char('\\'), 4);
        return end < 0 ? int(path.size()) : int(end + (end > 4));
    }
    return path.startsWith(QLatin1Char('\\')) ? 1 : 3;
}

QString directoryName(const QString &path)
{
    if (path.isEmpty())
        return {};
    const int root = rootLength(path);
    if (path.size() <= root)
        return {};
    return path.left(qMax(root, int(path.lastIndexOf(QLatin1Char('\\')))));
}

QString join(const QString &directory, const QString &name)
{
    return directory.isEmpty() || directory.endsWith(QLatin1Char('\\'))
        ? directory + name : directory + QLatin1Char('\\') + name;
}

ServiceImagePath readImagePath()
{
    std::array<wchar_t, maxCommandCharacters> data;
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const auto result = RegGetValueW(HKEY_LOCAL_MACHINE, serviceKey, L"ImagePath",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, data.data(), &bytes);
    if (result != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes > sizeof(data) ||
            bytes % sizeof(wchar_t) != 0 ||
            data[bytes / sizeof(wchar_t) - 1] != L'\0')
        return {};
    return {QString::fromWCharArray(data.data(), int(bytes / sizeof(wchar_t) - 1)),
        type == REG_EXPAND_SZ ? ImagePathType::ExpandString : ImagePathType::String};
}

QString expandImagePath(const QString &path)
{
    if (path.contains(QChar(0)))
        return {};
    const auto input = path.toStdWString();
    std::array<wchar_t, maxCommandCharacters> expanded;
    const auto size = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), DWORD(expanded.size()));
    return size && size <= expanded.size() ? QString::fromWCharArray(expanded.data(), int(size - 1)) : QString();
}

QString programFilesPath()
{
    std::array<wchar_t, MAX_PATH> path;
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES,
            nullptr, SHGFP_TYPE_CURRENT, path.data())))
        return {};
    return QString::fromWCharArray(path.data());
}

ServiceState queryServiceState()
{
    const std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, decltype(&CloseServiceHandle)> manager(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT), CloseServiceHandle);
    if (!manager)
        return ServiceState::Unknown;
    const std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, decltype(&CloseServiceHandle)> service(
        OpenServiceW(manager.get(), serviceName, SERVICE_QUERY_STATUS), CloseServiceHandle);
    if (!service)
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::Missing : ServiceState::Unknown;
    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service.get(), &status))
        return ServiceState::Unknown;
    switch (status.dwCurrentState) {
    case SERVICE_STOPPED: return ServiceState::Stopped;
    case SERVICE_START_PENDING:
    case SERVICE_STOP_PENDING:
    case SERVICE_CONTINUE_PENDING: return ServiceState::Starting;
    case SERVICE_RUNNING: return ServiceState::Running;
    default: return ServiceState::Unknown;
    }
}

} // namespace

QString cliFromServiceImagePath(const ServiceImagePath &value, const QString &programFiles,
    const std::function<QString(const QString &)> &expand)
{
    const auto image = (value.type == ImagePathType::ExpandString ? expand(value.text) : value.text).trimmed();
    const bool quoted = image.startsWith(QLatin1Char('"'));
    const auto suffix = QStringLiteral("\\tailscaled.exe");
    const auto index = quoted ? image.indexOf(QLatin1Char('"'), 1) :
        FindStringOrdinal(FIND_FROMSTART, reinterpret_cast<LPCWCH>(image.utf16()), int(image.size()),
            reinterpret_cast<LPCWCH>(suffix.utf16()), int(suffix.size()), TRUE);
    if (index < 0)
        return {};
    const auto end = quoted ? index : index + suffix.size();
    const auto arguments = end + (quoted ? 1 : 0);
    if (arguments < image.size() && !image.at(arguments).isSpace())
        return {};

    const auto start = quoted ? 1 : 0;
    const auto daemon = image.mid(start, end - start);
    if (!fullyQualified(daemon) || daemon.contains(QChar(0)))
        return {};
    const auto path = normalizeSeparators(daemon);
    const auto filename = path.mid(qMax(rootLength(path), int(path.lastIndexOf(QLatin1Char('\\'))) + 1));
    if (!ordinalEquals(filename, QStringLiteral("tailscaled.exe")))
        return {};

    const auto directory = directoryName(path);
    const auto previewRoot = join(normalizeSeparators(programFiles), QStringLiteral("AuroraTailnet"));
    if (ordinalEquals(directoryName(directory), previewRoot))
        return {};
    return join(directory, QStringLiteral("tailscale.exe"));
}

Installation inspectWindowsInstallation()
{
    const auto executable = cliFromServiceImagePath(readImagePath(), programFilesPath(), expandImagePath);
    if (executable.isEmpty() || !QFileInfo(executable).isFile())
        return {};
    return {executable, queryServiceState()};
}

} // namespace aurora::tailscale::system
