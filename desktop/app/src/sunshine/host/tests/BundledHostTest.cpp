// SPDX-License-Identifier: GPL-3.0-only
#include "BundledHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>

using namespace Aurora::Sunshine;

namespace {

const auto profileVariable = QStringLiteral("AURORA_SUNSHINE_PROFILE");
const auto stopVariable = QStringLiteral("AURORA_SUNSHINE_STOP_EVENT");
const QMap<QString, QString> expectedOptions{
    {QStringLiteral("port"), QStringLiteral("48989")},
    {QStringLiteral("origin_web_ui_allowed"), QStringLiteral("pc")},
    {QStringLiteral("upnp"), QStringLiteral("disabled")},
    {QStringLiteral("system_tray"), QStringLiteral("disabled")},
    {QStringLiteral("cert"), QStringLiteral("credentials/cacert.pem")},
    {QStringLiteral("pkey"), QStringLiteral("credentials/cakey.pem")},
    {QStringLiteral("file_state"), QStringLiteral("sunshine_state.json")},
    {QStringLiteral("credentials_file"), QStringLiteral("account.json")},
    {QStringLiteral("file_apps"), QStringLiteral("apps.json")},
    {QStringLiteral("log_path"), QStringLiteral("sunshine.log")}
};

void require(bool condition, const QString &message)
{
    if (!condition) {
        QTextStream(stderr) << message << '\n';
        std::exit(1);
    }
}

bool sameSequence(const QStringList &a, const QStringList &b)
{
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend());
}

void file(const QString &path, const QByteArray &contents = "Synthetic offline fixture.\n")
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), QStringLiteral("Cannot create test directory."));
    QFile handle(path);
    require(handle.open(QIODevice::WriteOnly | QIODevice::NewOnly) && handle.write(contents) == contents.size(),
            QStringLiteral("Cannot write test fixture."));
}

QByteArray readConfig(const QString &path)
{
    QFile handle(path);
    require(handle.open(QIODevice::ReadOnly), QStringLiteral("Cannot open synthetic sunshine.conf."));
    return handle.readAll();
}

HostLayout makeLayout(const QDir &workspace, const QString &label, HostPlatform platform)
{
    const QDir fixture(workspace.filePath(label));
    const auto appDirectory = fixture.absoluteFilePath(platform == HostPlatform::Windows
        ? QStringLiteral("Aurora App") : QStringLiteral("Aurora.app/Contents/MacOS"));
    const auto dataDirectory = fixture.absoluteFilePath(QStringLiteral("Fixture Local Data"));
    const auto layout = HostLayout::forDirectories(platform, appDirectory, dataDirectory);
    require(layout.has_value(), QStringLiteral("Cannot create host layout."));
    require(layout->profileDirectory() == QDir(dataDirectory).filePath(QStringLiteral("Aurora/Sunshine/Bundled/config")),
            QStringLiteral("Unexpected bundled profile location."));
    return *layout;
}

void seedPayload(const HostLayout &layout)
{
    const bool windows = layout.platform() == HostPlatform::Windows;
    const QDir payload(layout.payloadDirectory());
    require(!layout.hasPayload(), QStringLiteral("Missing payload was treated as complete."));
    file(layout.executable());
    file(payload.filePath(QStringLiteral("assets/apps.json")));
    file(payload.filePath(QStringLiteral("assets/web/index.html")));
    if (windows) {
        require(!layout.hasPayload(), QStringLiteral("Windows payload accepted without zlib1.dll."));
        file(payload.filePath(QStringLiteral("zlib1.dll")));
    }
    require(layout.hasPayload(), QStringLiteral("Synthetic payload was not found."));
}

void testProfile(const QDir &workspace)
{
    const auto win = makeLayout(workspace, QStringLiteral("Windows Layout"), HostPlatform::Windows);
    const auto mac = makeLayout(workspace, QStringLiteral("Mac Layout"), HostPlatform::MacOS);
    require(win.executable() == workspace.filePath(QStringLiteral("Windows Layout/Aurora App/Components/Sunshine/AuroraHost.exe")),
            QStringLiteral("Incorrect Windows executable."));
    const auto macHost = workspace.filePath(QStringLiteral("Mac Layout/Aurora.app/Contents/Helpers/AuroraHost.app/Contents"));
    require(mac.executable() == QDir(macHost).filePath(QStringLiteral("MacOS/AuroraHost")) &&
            mac.payloadDirectory() == QDir(macHost).filePath(QStringLiteral("Resources")),
            QStringLiteral("Incorrect macOS helper layout."));
    const QDir profile(win.profileDirectory());
    require(win.certificateFile() == profile.filePath(QStringLiteral("credentials/cacert.pem")) &&
            win.logFile() == profile.filePath(QStringLiteral("sunshine.log")),
            QStringLiteral("Unexpected managed certificate or log path."));
    require(!HostLayout::forDirectories(HostPlatform::Windows, QStringLiteral("relative"), win.profileDirectory()) &&
            !HostLayout::forDirectories(HostPlatform::MacOS, QStringLiteral("//example.invalid/app"), mac.profileDirectory()),
            QStringLiteral("Nonlocal or relative paths were accepted."));

    QString error;
    auto prepared = prepareBundledProfile(win, error);
    require(prepared && error.isEmpty(), QStringLiteral("Cannot initialize temporary profile. ") + error);
    QByteArray expected;
    for (auto option = expectedOptions.cbegin(); option != expectedOptions.cend(); ++option)
        expected += (option.key() + QStringLiteral(" = ") + option.value()).toUtf8() + '\n';
    require(readConfig(win.configFile()) == expected, QStringLiteral("Unexpected first-run config."));
    require(!QFileInfo::exists(profile.filePath(QStringLiteral("account.json"))) &&
            !QFileInfo::exists(profile.filePath(QStringLiteral("sunshine_state.json"))) &&
            !QFileInfo::exists(win.certificateFile()), QStringLiteral("Preparation created credential or state files."));
    require(!prepareBundledProfile(win, error) && !error.isEmpty(),
            QStringLiteral("Two staged hosts obtained the same profile lock."));
    prepared.reset();
    require(bool(prepareBundledProfile(win, error)), QStringLiteral("Profile lock was not released."));

    const QByteArray upstream = "# preserve upstream formatting\r\nport=1\r\ncredentials_file=sunshine_state.json\r\nfuture_option = yes\r\n";
    file(mac.configFile(), upstream);
    require(bool(prepareBundledProfile(mac, error)), QStringLiteral("Could not prepare an existing profile. ") + error);
    require(readConfig(mac.configFile()) == upstream, QStringLiteral("Existing configuration was rewritten."));

    const auto bad = makeLayout(workspace, QStringLiteral("Invalid Config"), HostPlatform::Windows);
    require(QDir().mkpath(bad.configFile()), QStringLiteral("Cannot create invalid config fixture."));
    require(!prepareBundledProfile(bad, error) && !error.isEmpty(), QStringLiteral("Config directory was accepted as a file."));
    require(QDir().rmdir(bad.configFile()), QStringLiteral("Cannot remove empty synthetic config directory."));
    require(bool(prepareBundledProfile(bad, error)), QStringLiteral("Failed preparation leaked the lock."));
}

void testAlias(const QDir &workspace)
{
    const auto layout = makeLayout(workspace, QStringLiteral("Aliased Files"), HostPlatform::Windows);
    const QDir profile(layout.profileDirectory());
    const auto state = profile.filePath(QStringLiteral("sunshine_state.json"));
    const auto account = profile.filePath(QStringLiteral("account.json"));
    file(state, "Synthetic pairing state.");

    std::error_code ec;
#if defined(Q_OS_WIN)
    std::filesystem::create_hard_link(std::filesystem::path(state.toStdWString()),
                                      std::filesystem::path(account.toStdWString()), ec);
#else
    std::filesystem::create_hard_link(std::filesystem::path(QFile::encodeName(state).constData()),
                                      std::filesystem::path(QFile::encodeName(account).constData()), ec);
#endif
    require(!ec, QStringLiteral("Cannot create synthetic account/state hard link."));
    QString error;
    require(!prepareBundledProfile(layout, error) && !error.isEmpty(),
            QStringLiteral("Hard-linked account and state were accepted."));
    require(!QFileInfo::exists(layout.configFile()), QStringLiteral("Rejected profile was seeded."));
    require(QFile::remove(account), QStringLiteral("Cannot remove synthetic account hard link."));
    file(account, "Synthetic separate account.");
    require(bool(prepareBundledProfile(layout, error)), QStringLiteral("Separate account and state were rejected."));
    require(QFileInfo(account).size() == 27 && QFileInfo(state).size() == 24,
            QStringLiteral("Existing synthetic account or state was altered."));
}

class FakeProcess final : public HostProcess
{
public:
    std::optional<HostLaunch> launch;
    QStringList calls;
    QByteArray frame;
    bool alive = false;
    bool failStartAfterSpawning = false;
    bool failWrite = false;
    bool acknowledgeStop = true;
    bool finishGracefully = true;
    bool finishWhenForced = true;
    bool forced = false;
    std::optional<int> status;

    bool start(const HostLaunch &request, QString &error) override
    {
        calls.append(QStringLiteral("start"));
        launch = request;
        status.reset();
        forced = false;
        alive = true;
        if (failStartAfterSpawning)
            error = QStringLiteral("Fixture containment failed.");
        return !failStartAfterSpawning;
    }

    bool writePasswordFrame(const QByteArray &packet, QString &error) override
    {
        calls.append(QStringLiteral("frame"));
        frame = packet;
        if (failWrite)
            error = QStringLiteral("Fixture pipe failed.");
        return !failWrite;
    }

    void closeInput() override { calls.append(QStringLiteral("close stdin")); }
    bool signalStopEvent() override
    {
        calls.append(QStringLiteral("signal event"));
        return acknowledgeStop;
    }
    bool running() override { return alive; }
    void waitForExit(int timeoutMs) override
    {
        calls.append(QStringLiteral("wait %1").arg(timeoutMs));
        if ((forced && finishWhenForced) || (!forced && finishGracefully)) {
            alive = false;
            status = 0;
        }
    }
    void forceStop() override
    {
        calls.append(QStringLiteral("force"));
        forced = true;
    }
    std::optional<int> exitCode() const override { return status; }
};

void testHandshake(const QDir &workspace, HostPlatform platform)
{
    const bool windows = platform == HostPlatform::Windows;
    const auto layout = makeLayout(workspace, windows ? QStringLiteral("Windows Launch") : QStringLiteral("Mac Launch"), platform);
    auto backend = std::make_unique<FakeProcess>();
    auto &fake = *backend;
    BundledHost host(layout, std::move(backend));
    QString error;
    require(!host.start(error) && fake.calls.isEmpty(), QStringLiteral("Missing payload attempted a launch."));
    seedPayload(layout);

    const QByteArray existing = "port = 1\ncredentials_file = sunshine_state.json\nfile_state = another.json\n";
    file(layout.configFile(), existing);
    require(host.start(error) && error.isEmpty() && host.running(), QStringLiteral("Offline launch failed. ") + error);
    const auto callsAfterStart = windows
        ? QStringList{QStringLiteral("start"), QStringLiteral("frame"), QStringLiteral("close stdin")}
        : QStringList{QStringLiteral("start"), QStringLiteral("frame")};
    require(sameSequence(fake.calls, callsAfterStart), QStringLiteral("Platform stdin handshake sequence differs."));
    require(host.start(error) && sameSequence(fake.calls, callsAfterStart), QStringLiteral("Start should reuse a running handshake."));

    const auto &launch = *fake.launch;
    require(launch.executable == layout.executable() && launch.workingDirectory == layout.payloadDirectory() &&
            launch.platform == platform && launch.environment.value(profileVariable) == layout.profileDirectory(),
            QStringLiteral("Launch target or environment differs."));
    QStringList overrides;
    for (auto option = expectedOptions.cbegin(); option != expectedOptions.cend(); ++option)
        overrides.append(option.key() + QLatin1Char('=') + option.value());
    require(sameSequence(launch.arguments, overrides), QStringLiteral("Launch failed to enforce managed overrides."));
    require(readConfig(layout.configFile()) == existing, QStringLiteral("Launch should leave existing config bytes alone."));
    const QRegularExpression eventPattern(QStringLiteral(R"(\ALocal\\AuroraSunshineStop-[a-f0-9]{32}\z)"));
    require(launch.environment.size() == (windows ? 2 : 1) &&
            (windows ? eventPattern.match(launch.environment.value(stopVariable)).hasMatch()
                     : !launch.environment.contains(stopVariable)),
            QStringLiteral("Wrong stop event environment."));

    const QRegularExpression framePattern(QStringLiteral(R"(\A[0-9A-F]{64}\z)"));
    require(framePattern.match(QString::fromLatin1(fake.frame)).hasMatch() && host.launchPassword() == fake.frame,
            QStringLiteral("The process frame must be exactly 64 uppercase hex bytes."));
    require(!prepareBundledProfile(layout, error), QStringLiteral("A running host must retain its profile lock."));
    const auto firstPassword = fake.frame;
    const auto firstEvent = launch.environment.value(stopVariable);

    fake.alive = false;
    fake.status = BundledHost::restartRequestedExitCode;
    require(host.exitCode() == 42 && host.launchPassword().isEmpty(),
            QStringLiteral("Exit must expose restart code and revoke credential access."));
    require(bool(prepareBundledProfile(layout, error)), QStringLiteral("Observed exit retained its profile lock."));
    require(host.start(error) && fake.frame != firstPassword &&
            (!windows || fake.launch->environment.value(stopVariable) != firstEvent),
            QStringLiteral("A new process must use a fresh frame and stop event."));

    fake.calls.clear();
    require(host.stop(error) && error.isEmpty() && host.launchPassword().isEmpty(), QStringLiteral("Graceful stop failed."));
    const auto stopRequest = windows ? QStringLiteral("signal event") : QStringLiteral("close stdin");
    require(sameSequence(fake.calls, QStringList{stopRequest, QStringLiteral("wait 11000")}),
            QStringLiteral("Incorrect platform graceful-stop sequence."));
}

void testFailures(const QDir &workspace)
{
    const auto layout = makeLayout(workspace, QStringLiteral("Launch Failures"), HostPlatform::Windows);
    seedPayload(layout);
    auto backend = std::make_unique<FakeProcess>();
    auto &fake = *backend;
    BundledHost host(layout, std::move(backend));
    QString error;

    fake.failStartAfterSpawning = true;
    require(!host.start(error) && error == QStringLiteral("Fixture containment failed.") && host.launchPassword().isEmpty(),
            QStringLiteral("Containment failure exposed a frame or lost its error."));
    require(sameSequence(fake.calls, QStringList{QStringLiteral("start"), QStringLiteral("force"), QStringLiteral("wait 3000")}),
            QStringLiteral("Containment failure must reap the spawned child before writing."));
    require(bool(prepareBundledProfile(layout, error)), QStringLiteral("Containment failure left a locked profile."));

    fake.calls.clear();
    fake.failStartAfterSpawning = false;
    fake.failWrite = true;
    require(!host.start(error) && error == QStringLiteral("Fixture pipe failed.") && host.launchPassword().isEmpty(),
            QStringLiteral("Pipe failure exposed a credential."));
    require(sameSequence(fake.calls, QStringList{QStringLiteral("start"), QStringLiteral("frame"), QStringLiteral("force"),
                                     QStringLiteral("wait 3000")}), QStringLiteral("Pipe failure cleanup differs."));

    fake.failWrite = false;
    require(host.start(error), QStringLiteral("Retry after failed frame must work."));
    fake.calls.clear();
    fake.finishGracefully = false;
    fake.finishWhenForced = false;
    require(!host.stop(error) && !error.isEmpty() && host.launchPassword().isEmpty(),
            QStringLiteral("Timed-out process must report stop failure and revoke credentials."));
    require(sameSequence(fake.calls, QStringList{QStringLiteral("signal event"), QStringLiteral("wait 11000"),
                                     QStringLiteral("force"), QStringLiteral("wait 3000")}),
            QStringLiteral("Stop must escalate after the graceful deadline."));
    require(!prepareBundledProfile(layout, error) && !host.start(error),
            QStringLiteral("Unreaped child must keep its lock and prevent relaunch."));
    fake.finishWhenForced = true;
    require(host.stop(error) && bool(prepareBundledProfile(layout, error)), QStringLiteral("Forced-stop retry leaked the profile."));
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QTemporaryDir fixture(QDir::current().filePath(QStringLiteral("sunshine-host-test-XXXXXX")));
    require(fixture.isValid(), QStringLiteral("Cannot create offline host fixture."));
    const QDir root(fixture.path());
    testProfile(root);
    testAlias(root);
    testHandshake(root, HostPlatform::Windows);
    testHandshake(root, HostPlatform::MacOS);
    testFailures(root);
    QTextStream(stdout) << "PASS: bundled profile and locks, credential-file separation, Windows and macOS fake "
                          "launch handshakes, restart indication and cleanup\n";
}
