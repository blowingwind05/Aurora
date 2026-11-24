// SPDX-License-Identifier: GPL-3.0-only
#include "MoonlightProcess.h"

#include <QFileInfo>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Aurora::Stream {

#ifdef Q_OS_WIN
struct MoonlightProcess::WindowsJob
{
    ~WindowsJob()
    {
        if (attributes)
            DeleteProcThreadAttributeList(attributes);
        if (handle)
            CloseHandle(handle);
    }

    bool initialize()
    {
        handle = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!handle || !SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof limits))
            return false;

        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size)
            return false;
        storage = std::make_unique<unsigned char[]>(size);
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
            return false;
        attributes = list;
        return UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                         &handle, sizeof handle, nullptr, nullptr) != FALSE;
    }

    void configure(QProcess::CreateProcessArguments &arguments)
    {
        // Create the process in the job, so the executable cannot run unbound.
        startup.StartupInfo = *arguments.startupInfo;
        startup.StartupInfo.cb = sizeof startup;
        startup.lpAttributeList = attributes;
        arguments.startupInfo = &startup.StartupInfo;
        arguments.flags |= CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    }

    HANDLE handle = nullptr;
    std::unique_ptr<unsigned char[]> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    STARTUPINFOEXW startup{};
};
#endif

MoonlightProcess::MoonlightProcess(QObject *parent) : QProcess(parent)
{
#ifdef Q_OS_WIN
    connect(this, qOverload<int, ExitStatus>(&QProcess::finished), this, [this] { job_.reset(); });
    connect(this, &QProcess::errorOccurred, this, [this](ProcessError error) {
        if (error == FailedToStart)
            job_.reset();
    });
#endif
}

MoonlightProcess::~MoonlightProcess()
{
    blockSignals(true);
    cancel();
    if (state() != NotRunning)
        waitForFinished(2000);
}

bool MoonlightProcess::launch(const Command &command, QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (error)
        error->clear();
    if (launchRequested_ || state() != NotRunning) {
        if (error)
            *error = QStringLiteral("The Moonlight process already has a launch request.");
        return false;
    }
    const QFileInfo program(command.program);
    if (!program.isAbsolute() || program.fileName().isEmpty() || command.workingDirectory != program.absolutePath()
        || command.arguments.isEmpty()) {
        if (error)
            *error = QStringLiteral("Invalid Moonlight launch command.");
        return false;
    }

#ifdef Q_OS_WIN
    job_ = std::make_unique<WindowsJob>();
    if (!job_->initialize()) {
        job_.reset();
        if (error)
            *error = QStringLiteral("Could not bind the Moonlight process to Aurora.");
        return false;
    }
    setCreateProcessArgumentsModifier([this](CreateProcessArguments *arguments) {
        job_->configure(*arguments);
    });
#endif

    launchRequested_ = true;
    setProgram(command.program);
    setWorkingDirectory(command.workingDirectory);
    setArguments(command.arguments);
    setProcessEnvironment(command.environment);
    setInputChannelMode(ForwardedInputChannel);
    setProcessChannelMode(command.captureOutput ? SeparateChannels : ForwardedChannels);
    if (command.captureOutput)
        setStandardErrorFile(nullDevice());
    QProcess::start();
    return true;
}

void MoonlightProcess::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state() != NotRunning)
        kill();
}

} // namespace Aurora::Stream
