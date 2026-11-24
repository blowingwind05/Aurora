// SPDX-License-Identifier: GPL-3.0-only
#include "MoonlightProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace Aurora::Stream;

#define CHECK(condition) do { \
    if (!(condition)) qFatal("%s:%d: %s", __FILE__, __LINE__, #condition); \
} while (false)

namespace {

constexpr auto windows = RuntimePlatform::Windows;
constexpr auto mac = RuntimePlatform::MacOS;
const auto uuid = QStringLiteral("01234567-89ab-cdef-0123-456789abcdef");
const auto pairId = QStringLiteral("0123456789abcdef0123456789abcdef");
const auto invitation = QStringLiteral("12345:123:") + pairId;
const auto inheritedKeys = {QStringLiteral("AURORA_EMBEDDED_SESSION"), QStringLiteral("AURORA_CONNECTION_DETAILS"),
    QStringLiteral("AURORA_STREAM_DEVICE_NAME"), QStringLiteral("AURORA_STREAM_PREFERENCES"),
    QStringLiteral("AURORA_GAMESTREAM_RELAY_BASE"), QStringLiteral("AURORA_GAMESTREAM_HOST_UUID")};

Route localRoute()
{
    return {QHostAddress(QStringLiteral("192.0.2.4")), 48989, uuid, false};
}

QProcessEnvironment testEnvironment()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : inheritedKeys)
        environment.insert(key, QStringLiteral("stale"));
    environment.insert(QStringLiteral("AURORA_STREAM_TEST_SENTINEL"), QStringLiteral("preserved"));
    return environment;
}

QString fixtureFile(const QDir &root, const QString &relative)
{
    const auto path = root.filePath(relative);
    CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    return path;
}

void checkDiscovery(const QDir &root)
{
    const auto desktop = root.filePath(QStringLiteral("Windows install"));
    const QDir install(desktop);
    const auto current = fixtureFile(install, QStringLiteral("Components/Moonlight/AuroraStream.exe"));
    const auto old = fixtureFile(install, QStringLiteral("Streamer/AuroraStream.exe"));
    const auto separate = fixtureFile(install, QStringLiteral("Moonlight/Moonlight.exe"));
    const auto registered = fixtureFile(root, QStringLiteral("Programs/Moonlight Game Streaming/Moonlight.exe"));
    const auto inPath = fixtureFile(root, QStringLiteral("Path folder/Moonlight.exe"));

    RuntimeSearch search;
    search.platform = windows;
    search.environment = QProcessEnvironment();
    search.environment.insert(QStringLiteral("ProgramW6432"), root.filePath(QStringLiteral("Programs")));
    search.environment.insert(QStringLiteral("PATH"), QStringLiteral("relative;\"%1\"").arg(QFileInfo(inPath).absolutePath()));
    CHECK(findMoonlight(desktop, search) == current);
    CHECK(findMoonlight(QFileInfo(current).absolutePath(), search) == current);
    CHECK(QFile::remove(current));
    CHECK(findMoonlight(QFileInfo(current).absolutePath(), search) == old);
    CHECK(findMoonlight(desktop, search) == old);
    CHECK(QFile::remove(old));
    CHECK(findMoonlight(desktop, search) == separate);
    CHECK(QFile::remove(separate));
    CHECK(findMoonlight(desktop, search) == registered);
    CHECK(QFile::remove(registered));
    CHECK(findMoonlight(desktop, search) == inPath);
    CHECK(QFile::remove(inPath));
    CHECK(findMoonlight(desktop, search).isEmpty());

    const auto macOS = root.filePath(QStringLiteral("Aurora Renamed.app/Contents/MacOS"));
    const auto macIntegrated = fixtureFile(QDir(macOS), QStringLiteral("AuroraStream"));
    const auto macFallback = fixtureFile(root, QStringLiteral("User Applications/Moonlight.app/Contents/MacOS/Moonlight"));
    search.platform = mac;
    search.macApplicationDirectories = QStringList{root.filePath(QStringLiteral("System Applications")),
                                                    root.filePath(QStringLiteral("User Applications"))};
    CHECK(findMoonlight(macOS, search) == macIntegrated);
    CHECK(QFile::remove(macIntegrated));
    CHECK(findMoonlight(macOS, search) == macFallback);
}

void checkCommands(const QDir &root)
{
    const auto program = root.filePath(QStringLiteral("AuroraStream.exe"));
    const auto fallback = root.filePath(QStringLiteral("Moonlight.exe"));
    const auto environment = testEnvironment();
    Request request;
    request.route = localRoute();
    const auto defaults = makeCommand(program, request, windows, environment);
    CHECK(defaults.command && defaults.command->arguments.join('|') ==
          "--aurora-runtime|stream|192.0.2.4:48989|Desktop|--absolute-mouse|--vsync|--no-frame-pacing|--keep-awake");
    request.preferenceArguments = [](PreferenceDefaults defaults) -> QStringList {
        if (defaults.useViewerDefault)
            return {QStringLiteral("--display-mode"), QStringLiteral("windowed"), QStringLiteral("--vsync")};
        return {defaults.useBundledDefaults ? QStringLiteral("--vsync") : QStringLiteral("--absolute-mouse")};
    };

    auto result = makeCommand(program, request, windows, environment);
    CHECK(result.error == CommandError::None && result.command);
    CHECK(result.command->arguments.join('|') == "--aurora-runtime|stream|192.0.2.4:48989|Desktop|--vsync");
    CHECK(!result.command->captureOutput);
    for (const auto &key : inheritedKeys)
        CHECK(!result.command->environment.contains(key));
    CHECK(result.command->environment.value(QStringLiteral("AURORA_STREAM_TEST_SENTINEL")) == "preserved");

    result = makeCommand(fallback, request, windows, environment);
    CHECK(result.command && result.command->arguments.join('|') == "stream|192.0.2.4:48989|Desktop|--absolute-mouse");
    request.embeddedInvitation = invitation;
    request.readConnectionDetails = true;
    result = makeCommand(program, request, windows, environment);
    CHECK(result.command && result.command->captureOutput);
    CHECK(result.command->arguments.join('|') == "--aurora-runtime|stream|192.0.2.4:48989|Desktop|--display-mode|windowed|--vsync");
    CHECK(result.command->environment.value(QStringLiteral("AURORA_EMBEDDED_SESSION")) == invitation);
    CHECK(result.command->environment.value(QStringLiteral("AURORA_CONNECTION_DETAILS")) == "1");
    CHECK(makeCommand(fallback, request, windows).error == CommandError::InvalidArguments);
    request.embeddedInvitation = QStringLiteral("invalid");
    CHECK(makeCommand(program, request, windows).error == CommandError::InvalidArguments);

    request.embeddedInvitation.clear();
    request.readConnectionDetails = false;
    request.deviceName = QStringLiteral("Device Name");
    request.sessionPreferences.insert(QStringLiteral("hdr"), QStringLiteral("off"));
    result = makeCommand(program, request, mac, environment);
    CHECK(result.command && result.command->captureOutput);
    CHECK(result.command->arguments.contains(QStringLiteral("windowed")));
    CHECK(result.command->environment.value(QStringLiteral("AURORA_STREAM_DEVICE_NAME")) == "Device Name");
    CHECK(result.command->environment.value(QStringLiteral("AURORA_STREAM_PREFERENCES")) == "{\"hdr\":\"off\"}");

    request = Request{};
    request.route = {QHostAddress(QStringLiteral("fd7a:115c:a1e0::123")), 48989, uuid, false};
    request.operation = Operation::Pair;
    result = makeCommand(program, request, mac, environment);
    CHECK(result.command && !result.command->captureOutput);
    CHECK(result.command->arguments.join('|') == "--aurora-runtime|pair|[fd7a:115c:a1e0::123]:48989");
    request.operation = Operation::BackgroundPair;
    request.pairingId = pairId;
    request.pin = QStringLiteral("0042");
    result = makeCommand(program, request, windows);
    CHECK(result.command && result.command->captureOutput);
    CHECK(result.command->arguments.join('|') == "--aurora-runtime|pair|--aurora-background|--aurora-pair-id|"
          + pairId + "|--pin|0042|[fd7a:115c:a1e0::123]:48989");
    request.pin = QStringLiteral("12345");
    CHECK(makeCommand(program, request, windows).error == CommandError::InvalidArguments);

    request.operation = Operation::Wallpaper;
    result = makeCommand(program, request, windows);
    CHECK(result.command && result.command->arguments.join('|') ==
          "--aurora-runtime|wallpaper|[fd7a:115c:a1e0::123]:48989|" + uuid + "|-");
    request.wallpaperRevision = QStringLiteral("--wrong");
    CHECK(makeCommand(program, request, windows).error == CommandError::InvalidArguments);
    for (auto operation : {Operation::Restart, Operation::Shutdown, Operation::PrivacyStatus,
                           Operation::PrivacyOn, Operation::PrivacyOff}) {
        request.operation = operation;
        result = makeCommand(program, request, windows);
        CHECK(result.command && result.command->arguments.at(3) == uuid);
        CHECK(makeCommand(fallback, request, windows).error == CommandError::BundledRuntimeRequired);
    }
    CHECK(result.command && result.command->arguments.join('|') ==
          "--aurora-runtime|privacy|[fd7a:115c:a1e0::123]:48989|" + uuid + "|off");

    request.operation = Operation::List;
    for (const auto *literal : {"example.invalid", "fe80::1%4", "0.0.0.0", "::", "239.0.0.1", "255.255.255.255"}) {
        request.route.address = QHostAddress(QString::fromLatin1(literal));
        CHECK(makeCommand(program, request, windows).error == CommandError::InvalidEndpoint);
    }
    request.route = {QHostAddress::LocalHost, 55001, uuid, true};
    result = makeCommand(program, request, windows, environment);
    CHECK(result.command && result.command->arguments.join('|') == "--aurora-runtime|list|127.0.0.1:55001");
    CHECK(result.command->environment.value(QStringLiteral("AURORA_GAMESTREAM_RELAY_BASE")) == "55001");
    CHECK(result.command->environment.value(QStringLiteral("AURORA_GAMESTREAM_HOST_UUID")) == uuid);
    CHECK(makeCommand(fallback, request, windows).error == CommandError::BundledRuntimeRequired);
    for (const auto *literal : {"127.0.0.2", "::1", "::ffff:127.0.0.1", "192.0.2.1"}) {
        request.route.address = QHostAddress(QString::fromLatin1(literal));
        CHECK(makeCommand(program, request, windows).error == CommandError::InvalidRelay);
    }
    request.route.address = QHostAddress::LocalHost;
    for (int port : {5, 65515}) {
        request.route.port = port;
        CHECK(makeCommand(program, request, windows).error == CommandError::InvalidRelay);
    }
    request.route.port = 6;
    CHECK(makeCommand(program, request, windows).command);
    request.route.port = 65514;
    CHECK(makeCommand(program, request, windows).command);
    request.route.hostId.clear();
    CHECK(makeCommand(program, request, windows).error == CommandError::InvalidRelay);
    CHECK(makeCommand(QStringLiteral("AuroraStream.exe"), request, windows).error == CommandError::InvalidProgram);

    CHECK(makeProbeCommand(program, Probe::EmbeddedWindow, mac).error == CommandError::UnsupportedPlatform);
    CHECK(!probeSucceeded(Probe::Runtime, 0, "aurora-stream/2"));
    CHECK(!probeSucceeded(Probe::GameStreamRelay, 3, "aurora-gamestream-relay/1"));
    CHECK(!probeSucceeded(Probe::BackgroundPair, 0, "--aurora-background"));
}

Command fakeCommand(const QString &program, QProcessEnvironment environment)
{
    Request request;
    request.route = {QHostAddress::LocalHost, 55001, uuid, true};
    request.readConnectionDetails = true;
    request.preferenceArguments = [](PreferenceDefaults) -> QStringList {
        return {QStringLiteral("--profile"), QStringLiteral("spaces \"quote\" and a trailing \\"), QString()};
    };
    const auto result = makeCommand(program, request, windows, environment);
    CHECK(result.command);
    return *result.command;
}

void waitForReport(QProcess &child)
{
    CHECK(child.bytesAvailable() != 0 || child.waitForReadyRead(5000));
}

void checkFake(const QString &program)
{
    const auto environment = testEnvironment();
    for (const auto probe : {Probe::Runtime, Probe::GameStreamRelay, Probe::BackgroundPair,
#ifdef Q_OS_WIN
                             Probe::EmbeddedWindow
#endif
         }) {
        const auto result = makeProbeCommand(program, probe, windows, environment);
        CHECK(result.command);
        MoonlightProcess child;
        CHECK(child.launch(*result.command));
        CHECK(child.waitForFinished(5000));
        CHECK(child.exitStatus() == QProcess::NormalExit);
        CHECK(probeSucceeded(probe, child.exitCode(), child.readAllStandardOutput()));
    }

    auto test = environment;
    test.insert(QStringLiteral("AURORA_STREAM_TEST_EXIT"), QStringLiteral("7"));
    const auto command = fakeCommand(program, test);
    MoonlightProcess child;
    CHECK(child.launch(command));
    CHECK(child.waitForFinished(5000));
    CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 7);
    CHECK(!child.launch(command));
    CHECK(child.readAllStandardError().isEmpty());
    const auto report = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
    CHECK(report.value(QStringLiteral("argv")).toArray() == QJsonArray::fromStringList(command.arguments));
    CHECK(report.value(QStringLiteral("verb")).toString() == "stream");
    CHECK(report.value(QStringLiteral("cwd")).toString() == QFileInfo(program).absolutePath());
    const auto reportedEnvironment = report.value(QStringLiteral("env")).toObject();
    CHECK(reportedEnvironment.value(QStringLiteral("AURORA_GAMESTREAM_RELAY_BASE")).toString() == "55001");
    CHECK(reportedEnvironment.value(QStringLiteral("AURORA_GAMESTREAM_HOST_UUID")).toString() == uuid);
    CHECK(reportedEnvironment.value(QStringLiteral("AURORA_CONNECTION_DETAILS")).toString() == "1");
    CHECK(!reportedEnvironment.contains(QStringLiteral("AURORA_EMBEDDED_SESSION")));
    CHECK(!reportedEnvironment.contains(QStringLiteral("AURORA_STREAM_DEVICE_NAME")));

    test.insert(QStringLiteral("AURORA_STREAM_TEST_HOLD"), QStringLiteral("1"));
    MoonlightProcess cancelled;
    CHECK(cancelled.launch(fakeCommand(program, test)));
    waitForReport(cancelled);
    cancelled.cancel();
    CHECK(cancelled.state() == QProcess::NotRunning || cancelled.waitForFinished(5000));

    auto missing = command;
    missing.program = QFileInfo(program).absoluteDir().filePath(QStringLiteral("Missing.exe"));
    MoonlightProcess failed;
    CHECK(failed.launch(missing));
    CHECK(!failed.waitForStarted(5000));
    CHECK(failed.error() == QProcess::FailedToStart);
}

#ifdef Q_OS_WIN
int superviseFake(const QString &program)
{
    MoonlightProcess child;
    auto environment = testEnvironment();
    environment.insert(QStringLiteral("AURORA_STREAM_TEST_HOLD"), QStringLiteral("1"));
    CHECK(child.launch(fakeCommand(program, environment)));
    waitForReport(child);
    const auto line = QByteArray::number(child.processId());
    std::puts(line.constData());
    std::fflush(stdout);
    return QCoreApplication::exec();
}

void checkWindowsJob(const QString &program)
{
    QProcess supervisor;
    supervisor.setProgram(QCoreApplication::applicationFilePath());
    supervisor.setArguments({QStringLiteral("--supervisor"), program});
    supervisor.start();
    waitForReport(supervisor);
    bool validPid = false;
    const DWORD pid = supervisor.readAllStandardOutput().trimmed().toULong(&validPid);
    CHECK(validPid && pid != 0);
    const HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
    CHECK(child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT);
    supervisor.kill();
    CHECK(supervisor.waitForFinished(5000));
    const DWORD result = WaitForSingleObject(child, 5000);
    CloseHandle(child);
    CHECK(result == WAIT_OBJECT_0);
}
#endif

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
#ifdef Q_OS_WIN
    if (arguments.size() == 3 && arguments.at(1) == "--supervisor")
        return superviseFake(arguments.at(2));
#endif
    CHECK(arguments.size() == 2);
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const QDir base(temp.path());
    checkDiscovery(base);
    checkCommands(base);
    const auto fakeDir = base.filePath(QStringLiteral("Aurora Stream test with spaces"));
    CHECK(QDir().mkpath(fakeDir));
#ifdef Q_OS_WIN
    const auto program = QDir(fakeDir).filePath(QStringLiteral("AuroraStream.exe"));
#else
    const auto program = QDir(fakeDir).filePath(QStringLiteral("AuroraStream"));
#endif
    CHECK(QFile::copy(arguments.at(1), program));
    checkFake(program);
#ifdef Q_OS_WIN
    checkWindowsJob(program);
#endif
}
