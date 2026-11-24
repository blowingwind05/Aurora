// SPDX-License-Identifier: GPL-3.0-only
#include "BundledHost.h"

#include <QDeadlineTimer>
#include <QOperatingSystemVersion>
#include <QProcess>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <signal.h>
#include <unistd.h>
#endif

namespace Aurora::Sunshine {
namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)

constexpr int launchWaitMs = 5000;

#if defined(Q_OS_WIN)
struct CloseWinHandle
{
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using WinHandle = std::unique_ptr<void, CloseWinHandle>;

WinHandle newHostJob()
{
    WinHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return {};
    return job;
}
#else
void startChildProcessGroup()
{
    // This runs after fork and before exec. Both calls are async-signal-safe.
    if (::setpgid(0, 0) != 0)
        ::_exit(127);
}
#endif

class CompanionProcess final : public QProcess
{
#if defined(Q_OS_MACOS) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void setupChildProcess() override { startChildProcessGroup(); }
#endif
};

class NativeHostProcess final : public HostProcess
{
public:
    ~NativeHostProcess() override
    {
        if (running())
            forceStop();
    }

    bool start(const HostLaunch &launch, QString &error) override
    {
        if (running())
            return false;
        started_ = false;
#if defined(Q_OS_WIN)
        const auto name = launch.environment.value(QStringLiteral("AURORA_SUNSHINE_STOP_EVENT")).toStdWString();
        SetLastError(ERROR_SUCCESS);
        WinHandle stop(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
        if (!stop || GetLastError() == ERROR_ALREADY_EXISTS) {
            error = QStringLiteral("无法为内置 Sunshine 创建专用停机信号。");
            return false;
        }
        auto job = newHostJob();
        if (!job) {
            error = QStringLiteral("无法将 Sunshine 进程绑定到 Aurora。");
            return false;
        }
        stopEvent_ = std::move(stop);
        process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#elif QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        process_.setChildProcessModifier(startChildProcessGroup);
#endif
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("AURORA_SUNSHINE_STOP_EVENT"));
        for (auto it = launch.environment.cbegin(); it != launch.environment.cend(); ++it)
            environment.insert(it.key(), it.value());
        process_.setProcessEnvironment(environment);
        process_.setWorkingDirectory(launch.workingDirectory);
        process_.setProcessChannelMode(QProcess::ForwardedChannels);
        process_.setInputChannelMode(QProcess::ManagedInputChannel);
        process_.start(launch.executable, launch.arguments);
        if (!process_.waitForStarted(launchWaitMs)) {
            error = QStringLiteral("无法启动内置 Sunshine。");
            return false;
        }
        started_ = true;

        // The companion is blocked on its stdin frame until containment is in place.
#if defined(Q_OS_WIN)
        WinHandle child(OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process_.processId())));
        if (!child || !AssignProcessToJobObject(job.get(), child.get())) {
            error = QStringLiteral("无法将 Sunshine 进程绑定到 Aurora。");
            return false;
        }
        job_ = std::move(job);
#else
        const auto pid = pid_t(process_.processId());
        if (pid <= 1 || ::getpgid(pid) != pid) {
            error = QStringLiteral("无法建立内置 Sunshine 的专用进程组。");
            return false;
        }
        processGroup_ = pid;
#endif
        return true;
    }

    bool writePasswordFrame(const QByteArray &frame, QString &error) override
    {
        QDeadlineTimer deadline(launchWaitMs);
        bool sent = process_.write(frame) == frame.size();
        while (sent && process_.bytesToWrite() != 0)
            sent = !deadline.hasExpired() && process_.waitForBytesWritten(int(deadline.remainingTime()));
        if (sent && running())
            return true;

        error = QStringLiteral("无法发送内置 Sunshine 的启动凭据。");
        return false;
    }

    void closeInput() override { process_.closeWriteChannel(); }

    bool signalStopEvent() override
    {
#if defined(Q_OS_WIN)
        return stopEvent_ && SetEvent(stopEvent_.get());
#else
        return false;
#endif
    }

    bool running() override
    {
        if (process_.state() == QProcess::Running)
            process_.waitForFinished(0);
        if (process_.state() != QProcess::NotRunning)
            return true;

#if defined(Q_OS_WIN)
        job_.reset();
        stopEvent_.reset();
#else
        processGroup_ = 0;
#endif
        return false;
    }

    void waitForExit(int timeoutMs) override
    {
        if (running())
            process_.waitForFinished(timeoutMs);
    }

    void forceStop() override
    {
#if defined(Q_OS_WIN)
        if (job_) {
            job_.reset();
            return;
        }
#else
        if (processGroup_ > 1 && pid_t(process_.processId()) == processGroup_ &&
            ::killpg(processGroup_, SIGKILL) == 0)
            return;
#endif
        process_.kill();
    }

    std::optional<int> exitCode() const override
    {
        if (!started_ || process_.state() != QProcess::NotRunning || process_.exitStatus() != QProcess::NormalExit)
            return std::nullopt;
        return process_.exitCode();
    }

private:
    CompanionProcess process_;
#if defined(Q_OS_WIN)
    WinHandle stopEvent_;
    WinHandle job_;
#else
    pid_t processGroup_ = 0;
#endif
    bool started_ = false;
};

#endif

} // namespace

std::unique_ptr<HostProcess> makeNativeHostProcess(HostPlatform platform)
{
#if defined(Q_OS_WIN)
    if (platform == HostPlatform::Windows)
        return std::make_unique<NativeHostProcess>();
#elif defined(Q_OS_MACOS)
    if (platform == HostPlatform::MacOS &&
        QOperatingSystemVersion::current() >= QOperatingSystemVersion(QOperatingSystemVersion::MacOS, 14, 2))
        return std::make_unique<NativeHostProcess>();
#else
    Q_UNUSED(platform);
#endif
    return {};
}

} // namespace Aurora::Sunshine
