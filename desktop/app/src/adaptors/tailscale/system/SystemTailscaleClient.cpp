// SPDX-License-Identifier: GPL-3.0-only
#include "SystemTailscaleClient.h"

#include <QFileInfo>
#include <QStringList>
#include <qt_windows.h>

#include <algorithm>
#include <utility>

namespace aurora::tailscale::system {
namespace {

constexpr int readDeadlineMs = 8000;
constexpr int upDeadlineMs = 5000;
constexpr int switchDeadlineMs = 12000;
constexpr int maxActionOutputBytes = 64 * 1024;
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
    qRegisterMetaType<ActionResult>();
    process.setStandardInputFile(QProcess::nullDevice());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    dispatch.setSingleShot(true);

    connect(&dispatch, &QTimer::timeout, this, [this] {
        if (request && request->executable.isEmpty())
            begin();
        else
            startCommand();
    });
    connect(&deadline, &QTimer::timeout, this, [this] { fail(StatusError::Timeout); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        if (request && !stopping && !collect(QProcess::StandardOutput))
            fail(StatusError::ResponseTooLarge);
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        if (request && !stopping && !collect(QProcess::StandardError))
            fail(StatusError::ResponseTooLarge);
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (stopping)
            return;
        if (error == QProcess::FailedToStart)
            fail(StatusError::StartFailed);
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            fail(StatusError::Transport);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SystemTailscaleClient::commandFinished);
}

SystemTailscaleClient::~SystemTailscaleClient()
{
    dispatch.stop();
    deadline.stop();
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(200);
    }
}

bool SystemTailscaleClient::requestStatus(quint64 generation)
{
    return enqueue(Phase::Status, generation);
}

bool SystemTailscaleClient::requestProfiles(quint64 generation)
{
    return enqueue(Phase::ListProfiles, generation);
}

bool SystemTailscaleClient::requestLogin(quint64 generation)
{
    return enqueue(Phase::Up, generation, Action::Login);
}

bool SystemTailscaleClient::requestResume(quint64 generation)
{
    return enqueue(Phase::Up, generation, Action::Resume);
}

bool SystemTailscaleClient::requestLogout(quint64 generation)
{
    return enqueue(Phase::Logout, generation, Action::Logout);
}

bool SystemTailscaleClient::requestProfileSwitch(const QString &id, quint64 generation)
{
    if (!listedProfiles || listedProfiles->generation != generation || !listedProfiles->ids.contains(id))
        return false;
    return enqueue(Phase::CheckProfile, generation, Action::SwitchProfile, id);
}

bool SystemTailscaleClient::enqueue(Phase phase, quint64 generation, std::optional<Action> action, const QString &id)
{
    if (request || stopping)
        return false;
    if (phase == Phase::ListProfiles || (action && phase != Phase::CheckProfile))
        listedProfiles.reset();
    request = Request{phase, generation, action, {}, id};
    resetDeadline(phase);
    dispatch.start(0);
    return true;
}

void SystemTailscaleClient::resetDeadline(Phase phase)
{
    dispatch.stop();
    request->phase = phase;
    request->legacy = false;
    standardOutput.clear();
    standardError.clear();
    deadlineMs = phase == Phase::Up ? upDeadlineMs : phase == Phase::SwitchProfile ? switchDeadlineMs : readDeadlineMs;
    elapsed.start();
    deadline.start(deadlineMs);
}

void SystemTailscaleClient::begin()
{
    if (!request)
        return;
    const auto installation = locate();
    const QFileInfo executable(installation.executable);
    if (!executable.isAbsolute() || !executable.isFile() || installation.service == ServiceState::Missing) {
        return finish(request->phase == Phase::ListProfiles ? StatusError::None : StatusError::Unavailable);
    }

    switch (request->phase) {
    case Phase::ListProfiles:
        if (installation.service != ServiceState::Running)
            return finish(StatusError::None);
        break;
    case Phase::CheckProfile: {
        const auto listed = std::exchange(listedProfiles, std::nullopt);
        if (!listed || installation.service != ServiceState::Running ||
                listed->executable.compare(installation.executable, Qt::CaseInsensitive) != 0)
            return fail(StatusError::Unavailable);
        break;
    }
    case Phase::Status:
        if (installation.service == ServiceState::Stopped || installation.service == ServiceState::Starting) {
            Status status;
            status.state = installation.service == ServiceState::Stopped ? BackendState::Stopped : BackendState::Starting;
            return finish(StatusError::None, status);
        }
        break;
    default:
        break;
    }
    request->executable = installation.executable;
    startCommand();
}

void SystemTailscaleClient::startCommand()
{
    if (!request)
        return;
    if (elapsed.hasExpired(deadlineMs))
        return fail(StatusError::Timeout);

    QStringList arguments{systemSocket};
    switch (request->phase) {
    case Phase::Status:
        arguments << QStringLiteral("status") << QStringLiteral("--json");
        break;
    case Phase::ListProfiles:
    case Phase::CheckProfile:
    case Phase::VerifyProfile:
        arguments << QStringLiteral("switch") << QStringLiteral("--list");
        if (!request->legacy)
            arguments << QStringLiteral("--json");
        break;
    case Phase::Up:
        arguments << QStringLiteral("up");
        break;
    case Phase::Logout:
        arguments << QStringLiteral("logout");
        break;
    case Phase::SwitchProfile:
        arguments << QStringLiteral("switch") << QStringLiteral("--") << request->profileId;
        break;
    }
    process.start(request->executable, arguments);
}

bool SystemTailscaleClient::collect(QProcess::ProcessChannel channel)
{
    const bool readingError = channel == QProcess::StandardError;
    auto &bytes = readingError ? standardError : standardOutput;
    int limit = maxStderrBytes;
    if (!readingError) {
        switch (request->phase) {
        case Phase::Status: limit = maxSystemStatusBytes; break;
        case Phase::ListProfiles:
        case Phase::CheckProfile:
        case Phase::VerifyProfile: limit = maxProfilesBytes; break;
        default: limit = maxActionOutputBytes; break;
        }
    }
    process.setReadChannel(channel);
    bytes += process.read(limit + 1 - bytes.size());
    return bytes.size() <= limit;
}

void SystemTailscaleClient::commandFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (stopping) {
        stopping = false;
        if (request)
            dispatch.start(0);
        return;
    }
    if (!request)
        return;
    if (elapsed.hasExpired(deadlineMs))
        return fail(StatusError::Timeout);

    const auto phase = request->phase;
    if (exitStatus == QProcess::NormalExit && (phase == Phase::Up || phase == Phase::Logout || phase == Phase::SwitchProfile))
        request->exitCode = exitCode;
    if (!collect(QProcess::StandardOutput) || !collect(QProcess::StandardError))
        return fail(StatusError::ResponseTooLarge);
    if (exitStatus != QProcess::NormalExit)
        return fail(StatusError::Transport);

    const bool listing = phase == Phase::ListProfiles || phase == Phase::CheckProfile || phase == Phase::VerifyProfile;
    if (exitCode != 0) {
        if (listing && !request->legacy && cliLacksJsonProfileList(standardOutput, standardError)) {
            request->legacy = true;
            standardOutput.clear();
            standardError.clear();
            dispatch.start(0);
            return;
        }
        return fail(StatusError::HelperRejected);
    }

    if (listing)
        return profilesRead(request->legacy ? decodeLegacyProfileList(standardOutput) : decodeProfileList(standardOutput));
    switch (phase) {
    case Phase::Status: {
        auto decoded = decodeSystemStatus(standardOutput);
        return finish(decoded.error, std::move(decoded.status));
    }
    case Phase::Up:
        return finish(StatusError::None, {}, {}, ActionOutcome::ReadStatus);
    case Phase::Logout:
        return finish(StatusError::None, {}, {}, ActionOutcome::Completed);
    case Phase::SwitchProfile:
        return verifyProfile(StatusError::None);
    default:
        break;
    }
}

void SystemTailscaleClient::profilesRead(ProfilesResult result)
{
    if (result.error != StatusError::None || elapsed.hasExpired(deadlineMs))
        return fail(result.error);
    if (request->phase == Phase::ListProfiles)
        return finish(StatusError::None, {}, std::move(result.profiles));

    const auto selected = std::find_if(result.profiles.cbegin(), result.profiles.cend(),
        [this](const Profile &profile) { return profile.id == request->profileId; });
    if (request->phase == Phase::VerifyProfile) {
        if (selected == result.profiles.cend() || !selected->isCurrent)
            return fail(StatusError::InvalidResponse);
        return finish(StatusError::None, {}, std::move(result.profiles), request->switchError == StatusError::None ?
            ActionOutcome::Completed : ActionOutcome::Revalidated);
    }

    if (selected == result.profiles.cend())
        return fail(StatusError::Unavailable);
    if (selected->isCurrent) {
        if (!sameRunningInstallation())
            return fail(StatusError::Unavailable);
        return finish(StatusError::None, {}, std::move(result.profiles), ActionOutcome::AlreadyCurrent);
    }
    resetDeadline(Phase::SwitchProfile);
    dispatch.start(0);
}

bool SystemTailscaleClient::sameRunningInstallation() const
{
    const auto installation = locate();
    return installation.service == ServiceState::Running &&
        installation.executable.compare(request->executable, Qt::CaseInsensitive) == 0 &&
        QFileInfo(installation.executable).isFile();
}

void SystemTailscaleClient::verifyProfile(StatusError switchError)
{
    request->switchError = switchError;
    resetDeadline(Phase::VerifyProfile);
    if (process.state() != QProcess::NotRunning) {
        stopping = true;
        process.kill();
    } else {
        dispatch.start(0);
    }
}

void SystemTailscaleClient::fail(StatusError error)
{
    if (!request)
        return;
    if (elapsed.hasExpired(deadlineMs))
        error = StatusError::Timeout;
    if (request->phase == Phase::Up && error == StatusError::Timeout)
        return finish(StatusError::None, {}, {}, ActionOutcome::ReadStatus);
    if (request->phase == Phase::SwitchProfile &&
            (error == StatusError::Timeout || error == StatusError::HelperRejected))
        return verifyProfile(error);
    if (request->phase == Phase::VerifyProfile && request->switchError != StatusError::None)
        error = request->switchError;
    finish(error);
}

void SystemTailscaleClient::finish(StatusError error, std::optional<Status> status, QVector<Profile> profiles,
    ActionOutcome outcome)
{
    if (!request)
        return;
    // A parse or synchronous inspection can outlast the timer's event delivery.
    if (error == StatusError::None && outcome != ActionOutcome::ReadStatus && elapsed.hasExpired(deadlineMs))
        return fail(StatusError::Timeout);

    const auto completed = std::exchange(request, std::nullopt);
    dispatch.stop();
    deadline.stop();
    standardOutput.clear();
    standardError.clear();
    if (process.state() != QProcess::NotRunning) {
        stopping = true;
        process.kill();
    }

    if (error == StatusError::None && !profiles.isEmpty()) {
        ListedProfiles listed{completed->generation, completed->executable, {}};
        for (const auto &profile : profiles) {
            if (!completed->action || !profile.isCurrent)
                listed.ids.insert(profile.id);
        }
        listedProfiles = std::move(listed);
    }

    if (completed->action)
        emit actionFinished(completed->generation,
            {*completed->action, error, outcome, completed->exitCode, std::move(profiles)});
    else if (completed->phase == Phase::Status)
        emit statusFinished(completed->generation, {error, std::move(status)});
    else
        emit profilesFinished(completed->generation, {error, std::move(profiles)});
}

} // namespace aurora::tailscale::system
