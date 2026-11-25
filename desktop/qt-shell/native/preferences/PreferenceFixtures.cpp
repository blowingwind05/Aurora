// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopPreferences.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <future>
#include <stdexcept>

using namespace Aurora::Preferences;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
    CHECK(file.flush());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

void fresh(const QString &directory)
{
    const GeneralPreferences general(directory);
    const StreamPreferences stream(directory);
    CHECK(general.closeToTray());
    CHECK(general.sleepMode() == SleepMode::System);
    CHECK(!general.managedHostAllowed());
    CHECK(!general.startupInitialized());
    const auto values = stream.read();
    CHECK(values.overrides.isEmpty());
    CHECK(values.value("resolution") == "host");
    CHECK(values.value("display-mode") == "windowed");
    CHECK(values.value("fps") == "60");
    CHECK(values.value("bitrate") == "10000");
    CHECK(values.value("vsync") == "on");
    CHECK(values.value("frame-pacing") == "off");
    CHECK(values.value("absolute-mouse") == "on");
    CHECK(values.value("video-codec") == "auto");
    CHECK(values.value("device-address").isEmpty());
    CHECK(!QFileInfo::exists(directory));

    CHECK(general.setCloseToTray(true));
    CHECK(general.setManagedHostAllowed(false));
    CHECK(!QFileInfo::exists(directory));

    CHECK(general.claimStartupInitialization() == StartupClaim::Claimed);
    CHECK(general.claimStartupInitialization() == StartupClaim::AlreadyInitialized);
    CHECK(readFile(QDir(directory).filePath("startup-initialized")).isEmpty());
    CHECK(general.setCloseToTray(false));
    CHECK(general.setSleepMode(SleepMode::OnAc));
    CHECK(general.setManagedHostAllowed(true));
    CHECK(stream.set("resolution", "3840x2160"));
    CHECK(stream.set("fps", "30"));
    CHECK(stream.read().value("bitrate") == "40000");
    CHECK(stream.set("bitrate", "42500"));
    CHECK(stream.set("vsync", "on"));

    const GeneralPreferences reopened(directory);
    CHECK(!reopened.closeToTray() && reopened.managedHostAllowed() && reopened.startupInitialized());
    CHECK(reopened.sleepMode() == SleepMode::OnAc);
    CHECK(readFile(QDir(directory).filePath("host-sleep-mode")) == "OnAc");
    CHECK(readFile(QDir(directory).filePath("exit-on-close")).isEmpty());
    CHECK(readFile(QDir(directory).filePath("allow-managed-host")).isEmpty());

    const auto saved = StreamPreferences(directory).read();
    CHECK(saved.value("bitrate") == "42500");
    CHECK(saved.overrides.value("vsync") == "on");
    CHECK(stream.reset());
    CHECK(StreamPreferences(directory).read().value("bitrate") == "10000");

    QStandardPaths::setTestModeEnabled(true);
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    CHECK(defaultDirectory() == QDir(data).filePath("Aurora"));
    QStandardPaths::setTestModeEnabled(false);
}

void migrated(const QString &directory)
{
    CHECK(QDir().mkpath(directory));
    const QDir profile(directory);
    writeFile(profile.filePath("exit-on-close"), "marker");
    writeFile(profile.filePath("allow-managed-host"), "");
    writeFile(profile.filePath("startup-initialized"), "old");
    writeFile(profile.filePath("host-sleep-mode"), "\xef\xbb\xbf  Always\r\n");
    const QByteArray legacyStream = R"({"resolution":"2560x1440","fps":"30","bitrate":"0500",
        "display-mode":"borderless","audio-on-host":true,"video-codec":"AV1","unknown":{"value":1}})";
    writeFile(profile.filePath("stream-preferences.json"), legacyStream);

    const GeneralPreferences general(directory);
    const StreamPreferences stream(directory);
    CHECK(!general.closeToTray());
    CHECK(general.managedHostAllowed());
    CHECK(general.sleepMode() == SleepMode::Always);
    CHECK(general.startupInitialized());
    CHECK(general.claimStartupInitialization() == StartupClaim::AlreadyInitialized);
    CHECK(readFile(profile.filePath("startup-initialized")) == "old");

    const auto saved = stream.read();
    CHECK(saved.value("resolution") == "2560x1440");
    CHECK(saved.value("fps") == "30");
    CHECK(saved.value("bitrate") == "20000");
    CHECK(saved.value("display-mode") == "borderless");
    CHECK(saved.value("video-codec") == "AV1");
    CHECK(saved.value("audio-on-host") == "off");
    CHECK(!saved.overrides.contains("unknown") && !saved.overrides.contains("bitrate"));
    CHECK(readFile(profile.filePath("stream-preferences.json")) == legacyStream);

    CHECK(general.setCloseToTray(false));
    CHECK(readFile(profile.filePath("exit-on-close")) == "marker");
    CHECK(general.setCloseToTray(true));
    CHECK(!QFileInfo::exists(profile.filePath("exit-on-close")));
    CHECK(general.setManagedHostAllowed(false));
    CHECK(!QFileInfo::exists(profile.filePath("allow-managed-host")));
    CHECK(general.setSleepMode(SleepMode::System));
    CHECK(readFile(profile.filePath("host-sleep-mode")) == "System");
}

void coexistence(const QString &directory)
{
    const GeneralPreferences general(directory);
    auto first = std::async(std::launch::async, [&] { return general.claimStartupInitialization(); });
    auto second = std::async(std::launch::async, [&] { return general.claimStartupInitialization(); });
    const auto a = first.get();
    const auto b = second.get();
    CHECK((a == StartupClaim::Claimed && b == StartupClaim::AlreadyInitialized) ||
          (b == StartupClaim::Claimed && a == StartupClaim::AlreadyInitialized));

    const QDir profile(directory);
    const QString streamPath = profile.filePath("stream-preferences.json");
    const StreamPreferences stream(directory);
    CHECK(stream.set("resolution", "1920x1080"));

    // Simulate updates from the already-installed app after construction.
    writeFile(profile.filePath("exit-on-close"), "");
    writeFile(profile.filePath("allow-managed-host"), "");
    writeFile(profile.filePath("host-sleep-mode"), "OnAc");
    writeFile(streamPath, R"({"resolution":"1280x720","future-option":"keep"})");
    CHECK(!general.closeToTray() && general.managedHostAllowed());
    CHECK(general.sleepMode() == SleepMode::OnAc);
    CHECK(stream.read().value("resolution") == "1280x720");

    QLockFile otherWriter(streamPath + ".lock");
    CHECK(otherWriter.tryLock(0));
    CHECK(!stream.set("fps", "30") && !stream.reset());
    CHECK(stream.read().value("fps") == "60");
    otherWriter.unlock();
    CHECK(stream.set("fps", "30"));
    const auto updated = QJsonDocument::fromJson(readFile(streamPath)).object();
    CHECK(updated.value("resolution").toString() == "1280x720");
    CHECK(updated.value("fps").toString() == "30");
    CHECK(updated.value("future-option").toString() == "keep");
    CHECK(!QFileInfo::exists(streamPath + ".lock"));
}

void failures(const QString &directory)
{
    const GeneralPreferences unavailable(QString{});
    const StreamPreferences unavailableStream(QString{});
    CHECK(!unavailable.closeToTray());
    CHECK(!unavailable.managedHostAllowed());
    CHECK(unavailable.sleepMode() == SleepMode::System);
    CHECK(unavailable.claimStartupInitialization() == StartupClaim::Failed);
    CHECK(!unavailable.setCloseToTray(false));
    CHECK(!unavailable.setManagedHostAllowed(true));
    CHECK(!unavailable.setSleepMode(SleepMode::OnAc));
    CHECK(unavailableStream.read().value("fps") == "60");
    CHECK(!unavailableStream.set("fps", "30") && !unavailableStream.reset());

    const GeneralPreferences general(directory);
    const StreamPreferences stream(directory);
    CHECK(!stream.set("fps", " 30") && !stream.set("unknown", "on"));
    CHECK(!stream.set("bitrate", "+500") && !stream.set("bitrate", "150001"));
    CHECK(!general.setSleepMode(static_cast<SleepMode>(99)));
    CHECK(!QFileInfo::exists(directory));
    CHECK(QDir().mkpath(directory));

    const QDir profile(directory);
    const QString streamPath = profile.filePath("stream-preferences.json");
    for (const QByteArray &invalid : {QByteArray("{oops"), QByteArray("[]"), QByteArray(16385, 'x')}) {
        writeFile(streamPath, invalid);
        CHECK(stream.read().overrides.isEmpty());
        CHECK(stream.read().value("bitrate") == "10000");
        CHECK(!stream.set("fps", "30"));
        CHECK(readFile(streamPath) == invalid);
        CHECK(stream.reset());
        CHECK(readFile(streamPath) == "{}");
    }
    CHECK(QFile::remove(streamPath));
    CHECK(profile.mkdir("stream-preferences.json"));
    CHECK(!stream.set("fps", "30") && !stream.reset());
    CHECK(profile.mkdir("exit-on-close"));
    CHECK(profile.mkdir("allow-managed-host"));
    CHECK(profile.mkdir("startup-initialized"));
    CHECK(profile.mkdir("host-sleep-mode"));
    CHECK(general.closeToTray() && !general.managedHostAllowed() && !general.startupInitialized());
    CHECK(!general.setCloseToTray(false));
    CHECK(!general.setCloseToTray(true));
    CHECK(!general.setManagedHostAllowed(true));
    CHECK(!general.setManagedHostAllowed(false));
    CHECK(general.claimStartupInitialization() == StartupClaim::Failed);
    CHECK(!general.setSleepMode(SleepMode::Always));
    CHECK(general.sleepMode() == SleepMode::System);
    CHECK(profile.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size() == 5);
}

void arguments(const QString &directory)
{
    const StreamPreferences stream(directory);
    const auto fresh = stream.read();
    CHECK(fresh.arguments().join('|') == "--absolute-mouse");
    CHECK(fresh.arguments(true, false).join('|') == "--absolute-mouse|--display-mode|windowed");
    CHECK(fresh.arguments(false, true).join('|') == "--absolute-mouse|--no-frame-pacing|--keep-awake|--vsync");
    CHECK(fresh.arguments(true, true).join('|') ==
          "--absolute-mouse|--display-mode|windowed|--no-frame-pacing|--keep-awake|--vsync");
    CHECK(!QFileInfo::exists(directory));

    CHECK(QDir().mkpath(directory));
    const auto path = QDir(directory).filePath("stream-preferences.json");
    const QByteArray saved = R"({"resolution":"3840x2160","fps":"30","display-mode":"borderless",
        "video-decoder":"software","hdr":"on","vsync":"off","frame-pacing":"on","absolute-mouse":"off",
        "bitrate":"040000","unknown":"on","audio-on-host":true,"video-codec":"HEVC","keep-awake":"off"})";
    writeFile(path, saved);
    const auto base = stream.read();
    const auto baseArgs = base.arguments(true, true).join('|');
    CHECK(baseArgs == "--no-absolute-mouse|--display-mode|borderless|--fps|30|--no-frame-pacing|--hdr|"
                       "--no-keep-awake|--resolution|3840x2160|--video-codec|HEVC|--video-decoder|auto|--no-vsync");
    CHECK(base.value("bitrate") == "40000");
    CHECK(base.value("video-decoder") == "software" && base.value("frame-pacing") == "on");
    CHECK(base.arguments().contains("--no-frame-pacing"));

    const auto host = base.withHostOverrides({{"resolution", "host"}, {"vsync", "on"}, {"hdr", "off"},
        {"fps", "060"}, {"bitrate", "+500"}, {"video-codec", "--untrusted"}, {"unknown", "off"}});
    const auto hostArgs = host.arguments(true, false).join('|');
    CHECK(hostArgs == "--no-absolute-mouse|--display-mode|borderless|--fps|30|--frame-pacing|--no-hdr|"
                      "--no-keep-awake|--video-codec|HEVC|--video-decoder|software|--vsync");
    CHECK(host.value("bitrate") == "5000");
    CHECK(base.value("resolution") == "3840x2160" && base.value("vsync") == "off");
    CHECK(base.arguments(true, true).join('|') == baseArgs);

    const auto negotiated = host.withHostOverrides({{"resolution", "2560x1440"}, {"fps", "60"}, {"bitrate", "150000"}});
    const auto negotiatedArgs = negotiated.arguments().join('|');
    CHECK(negotiatedArgs.contains("--resolution|2560x1440"));
    CHECK(negotiatedArgs.contains("--bitrate|150000"));
    CHECK(negotiatedArgs.contains("--fps|60"));
    CHECK(negotiated.value("bitrate") == "150000");
    CHECK(readFile(path) == saved);

    const StreamPreferenceSnapshot unvalidated{{{"--aurora-runtime", "on"}, {"fps", " 30"}}};
    CHECK(unvalidated.value("--aurora-runtime").isEmpty());
    CHECK(unvalidated.value("fps") == "60");
    CHECK(unvalidated.arguments().join('|') == "--absolute-mouse");
}

void sessionControl(const QString &directory)
{
    const StreamPreferences stream(directory);
    const auto fresh = stream.read();
    const QJsonObject defaults{
        {"resolution", "host"}, {"fps", "60"}, {"bitrate", "10000"}, {"video-codec", "auto"},
        {"hdr", "off"}, {"performance-overlay", "off"}, {"audio-config", "stereo"},
        {"audio-on-host", "off"}, {"mute-on-focus-loss", "off"}, {"absolute-mouse", "on"},
        {"mouse-buttons-swap", "off"}, {"reverse-scroll-direction", "off"}
    };
    CHECK(fresh.sessionControlSnapshot() == defaults);
    CHECK(!QFileInfo::exists(directory));

    for (const auto &line : {QString(), QStringLiteral("aurora-stream-setting/2\tfps\t30"),
            QStringLiteral("aurora-stream-setting/1\tfps\t 30"), QStringLiteral("aurora-stream-setting/1\tfps\t"),
            QStringLiteral("aurora-stream-setting/1\tfps\t30\tz"), QStringLiteral("aurora-stream-setting/1\tfps\t30\r\n"),
            QStringLiteral("aurora-stream-setting/1\tvsync\tOff"), QStringLiteral("aurora-stream-setting/1\tvsync\toff"),
            QStringLiteral("aurora-stream-setting/1\tvideo-decoder\tauto"), QStringLiteral("aurora-stream-setting/1\tfps"),
            QStringLiteral("aurora-stream-setting/1\tbitrate\t") + QString(128, QLatin1Char('0'))}) {
        CHECK(!SessionControlChange::fromLine(line));
    }
    CHECK(stream.applySessionControlChange({"fps", "--pin"}) == SessionControlSaveResult::Ignored);
    CHECK(stream.applySessionControlChange({"vsync", "on"}) == SessionControlSaveResult::Ignored);
    CHECK(stream.applySessionControlChange({"bitrate", "0500"}) == SessionControlSaveResult::Ignored);
    CHECK(!QFileInfo::exists(directory));

    CHECK(QDir().mkpath(directory));
    const auto path = QDir(directory).filePath("stream-preferences.json");
    writeFile(path, R"({"resolution":"1920x1080","hdr":"on","audio-on-host":"off","unrelated":{"value":4}})");
    const auto base = stream.read();
    auto expected = defaults;
    expected.insert("resolution", "1920x1080");
    expected.insert("hdr", "on");
    expected.insert("bitrate", "20000");
    CHECK(base.sessionControlSnapshot() == expected);

    const auto host = base.withHostOverrides({{"fps", "30"}, {"audio-on-host", "on"}, {"resolution", "3840x2160"}});
    auto hostExpected = expected;
    hostExpected.insert("fps", "30");
    hostExpected.insert("resolution", "3840x2160");
    hostExpected.insert("bitrate", "40000");
    hostExpected.insert("audio-on-host", "on");
    CHECK(host.sessionControlSnapshot() == hostExpected);

    const auto change = SessionControlChange::fromLine(QStringLiteral("aurora-stream-setting/1\tfps\t30"));
    CHECK(change && change->option == "fps" && change->value == "30");
    CHECK(stream.applySessionControlChange(*change) == SessionControlSaveResult::Saved);
    CHECK(base.sessionControlSnapshot() == expected);
    expected.insert("fps", "30");
    expected.insert("bitrate", "10000");
    CHECK(stream.read().sessionControlSnapshot() == expected);
    const auto saved = QJsonDocument::fromJson(readFile(path)).object();
    CHECK(saved.value("unrelated").toObject().value("value").toInt() == 4);
    CHECK(saved.value("fps").toString() == "30" && saved.value("resolution").toString() == "1920x1080");

    QLockFile otherWriter(path + ".lock");
    CHECK(otherWriter.tryLock(0));
    const auto before = readFile(path);
    const SessionControlChange blocked{"bitrate", "500"};
    CHECK(blocked.isValid());
    CHECK(stream.applySessionControlChange(blocked) == SessionControlSaveResult::SaveFailed);
    CHECK(readFile(path) == before);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QMap<QString, void (*)(const QString &)> fixtures{
        {"fresh", fresh}, {"migrated", migrated}, {"coexistence", coexistence}, {"failures", failures},
        {"arguments", arguments}, {"session-control", sessionControl}
    };

    try {
        CHECK(app.arguments().size() == 2);
        const auto run = fixtures.value(app.arguments().at(1));
        CHECK(run);
        const QTemporaryDir fixture(QDir::current().filePath("aurora-preferences-XXXXXX"));
        CHECK(fixture.isValid());
        run(QDir(fixture.path()).filePath(QStringLiteral("Aurora profile \u6d4b\u8bd5")));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Preference fixture failed: %s\n", error.what());
        return 1;
    }
}
