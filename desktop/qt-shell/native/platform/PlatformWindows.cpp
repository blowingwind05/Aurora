// SPDX-License-Identifier: GPL-3.0-only
#include "PlatformBackends.h"

#include <QDebug>
#include <QDir>

#include <windows.h>
#include <powrprof.h>
#include <powerbase.h>

#include <atomic>
#include <cstdint>
#include <utility>

namespace Aurora::Platform {
namespace {

constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t runValue[] = L"org.aurora-remote.desktop";
constexpr int maxCommandLength = 260;

bool missing(LSTATUS status)
{
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

QString quotedLauncher(const QString &launcher)
{
    return QLatin1Char('"') + QDir::toNativeSeparators(launcher) + QLatin1Char('"');
}

class WindowsStartupBackend final : public StartupBackend
{
public:
    explicit WindowsStartupBackend(QString launcher) : launcher_(std::move(launcher)) {}

    bool available() const override { return true; }

    StartupState read() const override
    {
        wchar_t command[maxCommandLength + 1]{};
        DWORD size = sizeof command;
        DWORD type = 0;
        const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, runKey, runValue, RRF_RT_REG_SZ,
                                            &type, command, &size);
        if (missing(status) || status == ERROR_UNSUPPORTED_TYPE)
            return StartupState::Off;
        if (status == ERROR_MORE_DATA && (type == REG_SZ || type == REG_EXPAND_SZ))
            return StartupState::OtherLocation;
        if (status != ERROR_SUCCESS)
            return StartupState::ReadFailed;
        if (command[0] == L'\0')
            return StartupState::Off;

        const auto expected = quotedLauncher(launcher_).toStdWString();
        return CompareStringOrdinal(command, -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL
            ? StartupState::On : StartupState::OtherLocation;
    }

    StartupResult setEnabled(bool enabled) override
    {
        HKEY key = nullptr;
        if (!enabled) {
            const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
            if (missing(opened))
                return StartupResult::Ok;
            if (opened != ERROR_SUCCESS)
                return StartupResult::WriteFailed;

            const LSTATUS removed = RegDeleteValueW(key, runValue);
            RegCloseKey(key);
            return removed == ERROR_SUCCESS || missing(removed)
                ? StartupResult::Ok : StartupResult::WriteFailed;
        }

        const auto command = windowsStartupCommand(launcher_).toStdWString();
        if (command.empty())
            return StartupResult::InvalidLauncher;
        const auto path = command.substr(1, command.size() - 2);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return StartupResult::InvalidLauncher;

        const LSTATUS opened = RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr,
                                               REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (opened != ERROR_SUCCESS)
            return StartupResult::WriteFailed;

        const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
        const LSTATUS written = RegSetValueExW(key, runValue, 0, REG_SZ,
                                               reinterpret_cast<const BYTE *>(command.c_str()), bytes);
        RegCloseKey(key);
        return written == ERROR_SUCCESS ? StartupResult::Ok : StartupResult::WriteFailed;
    }

private:
    QString launcher_;
};

// The OS callback may outlive a controller. It only touches this process-wide counter.
std::atomic<std::uint64_t> resumeGeneration{0};

ULONG CALLBACK powerNotification(PVOID, ULONG type, PVOID)
{
    if (type == PBT_APMRESUMEAUTOMATIC)
        resumeGeneration.fetch_add(1, std::memory_order_relaxed);
    return ERROR_SUCCESS;
}

class WindowsHostSleepBackend final : public HostSleepBackend
{
public:
    bool onAcPower() const override
    {
        SYSTEM_POWER_STATUS status{};
        return GetSystemPowerStatus(&status) && status.ACLineStatus == 1;
    }

    bool acquireAssertion() override
    {
        wchar_t reason[] = L"Aurora 正在允许本机被控";
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = reason;

        const HANDLE request = PowerCreateRequest(&context);
        if (request == INVALID_HANDLE_VALUE) {
            qDebug() << "Aurora could not create a Windows idle sleep request:" << GetLastError();
            return false;
        }
        if (!PowerSetRequest(request, PowerRequestSystemRequired)) {
            qDebug() << "Aurora could not set a Windows idle sleep request:" << GetLastError();
            CloseHandle(request);
            return false;
        }

        request_ = request;
        return true;
    }

    void releaseAssertion() override
    {
        PowerClearRequest(request_, PowerRequestSystemRequired);
        CloseHandle(request_);
        request_ = nullptr;
    }

    bool startResumeMonitoring() override
    {
        seenResume_ = resumeGeneration.load(std::memory_order_relaxed);
        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS recipient{powerNotification, nullptr};
        HPOWERNOTIFY notification = nullptr;
        const DWORD result = PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &recipient,
                                                                    &notification);
        if (result != ERROR_SUCCESS || !notification) {
            qDebug() << "Aurora could not subscribe to Windows power resume:" << result;
            return false;
        }
        notification_ = notification;
        return true;
    }

    void stopResumeMonitoring() override
    {
        const DWORD result = PowerUnregisterSuspendResumeNotification(notification_);
        if (result != ERROR_SUCCESS)
            qDebug() << "Aurora could not unsubscribe from Windows power resume:" << result;
        notification_ = nullptr;
    }

    bool takeResume() override
    {
        const auto current = resumeGeneration.load(std::memory_order_relaxed);
        return std::exchange(seenResume_, current) != current;
    }

private:
    HANDLE request_ = nullptr;
    HPOWERNOTIFY notification_ = nullptr;
    std::uint64_t seenResume_ = 0;
};

} // namespace

QString windowsStartupCommand(const QString &launcher)
{
    const auto path = QDir::toNativeSeparators(launcher);
    if (path.size() + 2 > maxCommandLength || path.contains(QLatin1Char('"'))
        || path.contains(QChar::Null)) {
        return {};
    }

    auto qualified = path;
    if (qualified.startsWith(QStringLiteral("\\\\?\\"))) {
        qualified.remove(0, 4);
        if (qualified.startsWith(QStringLiteral("UNC\\"), Qt::CaseInsensitive))
            qualified.replace(0, 4, QStringLiteral("\\\\"));
    }

    const auto shareSeparator = qualified.indexOf(QLatin1Char('\\'), 2);
    const auto fileSeparator = qualified.indexOf(QLatin1Char('\\'), shareSeparator + 1);
    const bool unc = qualified.startsWith(QStringLiteral("\\\\")) && shareSeparator > 2
        && fileSeparator > shareSeparator + 1 && qualified.at(2) != QLatin1Char('.');
    const bool drive = qualified.size() >= 3 && qualified.at(0).toUpper() >= QLatin1Char('A')
        && qualified.at(0).toUpper() <= QLatin1Char('Z') && qualified.mid(1, 2) == QStringLiteral(":\\");
    const auto name = qualified.mid(qualified.lastIndexOf(QLatin1Char('\\')) + 1);
    if ((!unc && !drive) || !name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
        || name.compare(QStringLiteral("dotnet.exe"), Qt::CaseInsensitive) == 0) {
        return {};
    }
    return quotedLauncher(path);
}

std::unique_ptr<StartupBackend> makeStartupBackend(const QString &windowsLauncherPath)
{
    return std::make_unique<WindowsStartupBackend>(windowsLauncherPath);
}

std::unique_ptr<HostSleepBackend> makeHostSleepBackend()
{
    return std::make_unique<WindowsHostSleepBackend>();
}

} // namespace Aurora::Platform
