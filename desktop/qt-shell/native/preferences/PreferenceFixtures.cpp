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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QMap<QString, void (*)(const QString &)> fixtures{
        {"fresh", fresh}, {"migrated", migrated}, {"coexistence", coexistence}, {"failures", failures}
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
