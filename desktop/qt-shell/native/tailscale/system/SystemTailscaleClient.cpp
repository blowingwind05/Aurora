// SPDX-License-Identifier: GPL-3.0-only
#include "SystemTailscaleClient.h"

#include <QFileInfo>
#include <QStringList>
#include <qt_windows.h>

#include <utility>

namespace aurora::tailscale::system {
namespace {

constexpr int commandDeadlineMs = 8000;
constexpr int maxStderrBytes = 64 * 1024;
const auto systemSocket = QStringLiteral(R"(--socket=\\.\pipe\protectedprefix\administrators\tailscale\tailscaled)");

} // namespace

SystemTailscaleClient::SystemTailscaleClient(QObject *parent)
    : SystemTailscaleClient(inspectWindowsInstallation, parent)
{
}

SystemTailscaleClient::SystemTailscaleClient(Locate locate, QObject *parent)
    : QObject(parent), locate(std::move(locate))
{
    qRegisterMetaType<StatusResult>();
    qRegisterMetaType<ProfilesResult>();
    process.setStandardInputFile(QProcess::nullDevice());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);

    connect(&deadline, &QTimer::timeout, this, [this] { finish(StatusError::Timeout); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        if (request && !collect(QProcess::StandardOutput))
            finish(StatusError::ResponseTooLarge);
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        if (request && !collect(QProcess::StandardError))
            finish(StatusError::ResponseTooLarge);
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(StatusError::StartFailed);
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            finish(StatusError::Transport);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SystemTailscaleClient::commandFinished);
}

SystemTailscaleClient::~SystemTailscaleClient()
{
    deadline.stop();
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(200);
    }
}

bool SystemTailscaleClient::requestStatus(quint64 generation)
{
    return enqueue(Operation::Status, generation);
}

bool SystemTailscaleClient::requestProfiles(quint64 generation)
{
    return enqueue(Operation::Profiles, generation);
}

bool SystemTailscaleClient::enqueue(Operation operation, quint64 generation)
{
    if (request || stopping)
        return false;
    request = Request{operation, generation, {}};
    elapsed.start();
    deadline.start(commandDeadlineMs);
    QTimer::singleShot(0, this, &SystemTailscaleClient::begin);
    return true;
}

void SystemTailscaleClient::begin()
{
    if (!request)
        return;
    const auto installation = locate();
    const QFileInfo executable(installation.executable);
    if (!executable.isAbsolute() || !executable.isFile() || installation.service == ServiceState::Missing) {
        return finish(request->operation == Operation::Status ? StatusError::Unavailable : StatusError::None);
    }

    if (request->operation == Operation::Profiles) {
        if (installation.service != ServiceState::Running)
            return finish(StatusError::None);
    } else {
        Status status;
        if (installation.service == ServiceState::Stopped || installation.service == ServiceState::Starting) {
            status.state = installation.service == ServiceState::Stopped ? BackendState::Stopped : BackendState::Starting;
            return finish(StatusError::None, status);
        }
    }
    request->executable = installation.executable;
    startCommand();
}

void SystemTailscaleClient::startCommand()
{
    if (!request)
        return;
    if (elapsed.hasExpired(commandDeadlineMs))
        return finish(StatusError::Timeout);
    QStringList arguments{systemSocket};
    if (request->operation == Operation::Status) {
        arguments << QStringLiteral("status") << QStringLiteral("--json");
    } else {
        arguments << QStringLiteral("switch") << QStringLiteral("--list");
        if (!request->legacy)
            arguments << QStringLiteral("--json");
    }
    process.start(request->executable, arguments);
}

bool SystemTailscaleClient::collect(QProcess::ProcessChannel channel)
{
    const bool readingError = channel == QProcess::StandardError;
    auto &bytes = readingError ? standardError : standardOutput;
    const int limit = readingError ? maxStderrBytes :
        request->operation == Operation::Status ? maxSystemStatusBytes : maxProfilesBytes;
    process.setReadChannel(channel);
    bytes += process.read(limit + 1 - bytes.size());
    return bytes.size() <= limit;
}

void SystemTailscaleClient::commandFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    stopping = false;
    if (!request)
        return;
    if (!collect(QProcess::StandardOutput) || !collect(QProcess::StandardError))
        return finish(StatusError::ResponseTooLarge);
    if (exitStatus != QProcess::NormalExit)
        return finish(StatusError::Transport);

    if (exitCode != 0) {
        if (request->operation == Operation::Profiles && !request->legacy && !elapsed.hasExpired(commandDeadlineMs) &&
                cliLacksJsonProfileList(standardOutput, standardError)) {
            request->legacy = true;
            standardOutput.clear();
            standardError.clear();
            QTimer::singleShot(0, this, &SystemTailscaleClient::startCommand);
            return;
        }
        return finish(StatusError::HelperRejected);
    }

    if (request->operation == Operation::Status) {
        auto decoded = decodeSystemStatus(standardOutput);
        return finish(decoded.error, std::move(decoded.status));
    }

    auto decoded = request->legacy ? decodeLegacyProfileList(standardOutput) : decodeProfileList(standardOutput);
    finish(decoded.error, {}, std::move(decoded.profiles));
}

void SystemTailscaleClient::finish(StatusError error, std::optional<Status> status, QVector<Profile> profiles)
{
    if (!request)
        return;
    if (elapsed.hasExpired(commandDeadlineMs)) {
        error = StatusError::Timeout;
        status.reset();
        profiles.clear();
    }

    const auto completed = std::exchange(request, std::nullopt);
    deadline.stop();
    standardOutput.clear();
    standardError.clear();
    if (process.state() != QProcess::NotRunning) {
        stopping = true;
        process.kill();
    }
    if (completed->operation == Operation::Status)
        emit statusFinished(completed->generation, {error, std::move(status)});
    else
        emit profilesFinished(completed->generation, {error, std::move(profiles)});
}

} // namespace aurora::tailscale::system
