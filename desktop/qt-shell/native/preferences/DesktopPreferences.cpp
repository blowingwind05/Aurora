// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopPreferences.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

#include <optional>

namespace Aurora::Preferences {
namespace {

const auto ExitOnClose = QStringLiteral("exit-on-close");
const auto HostSleepMode = QStringLiteral("host-sleep-mode");
const auto AllowManagedHost = QStringLiteral("allow-managed-host");
const auto StartupInitialized = QStringLiteral("startup-initialized");
const auto StreamFile = QStringLiteral("stream-preferences.json");
const auto Bitrate = QStringLiteral("bitrate");

constexpr int MaxStreamBytes = 16 * 1024;

QString preferencePath(const QString &directory, const QString &name)
{
    return directory.isEmpty() ? QString() : QDir(directory).filePath(name);
}

bool makeParentDirectory(const QString &path)
{
    return !path.isEmpty() && QDir().mkpath(QFileInfo(path).absolutePath());
}

bool saveAtomically(const QString &path, const QByteArray &data)
{
    if (!makeParentDirectory(path))
        return false;

    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

StartupClaim createMarker(const QString &path)
{
    if (QFileInfo(path).isFile())
        return StartupClaim::AlreadyInitialized;
    if (!makeParentDirectory(path))
        return StartupClaim::Failed;

    // The legacy app also uses exclusive creation for startup-initialized.
    QFile marker(path);
    if (marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return StartupClaim::Claimed;
    return QFileInfo(path).isFile() ? StartupClaim::AlreadyInitialized : StartupClaim::Failed;
}

bool setMarker(const QString &path, bool present)
{
    if (path.isEmpty())
        return false;
    if (present)
        return createMarker(path) != StartupClaim::Failed;
    if (QFile::remove(path))
        return true;
    const QFileInfo entry(path);
    return !entry.exists() && !entry.isSymLink();
}

const QMap<QString, QStringList> &streamOptions()
{
    static const QStringList defaultOn{QStringLiteral("on"), QStringLiteral("off")};
    static const QStringList defaultOff{QStringLiteral("off"), QStringLiteral("on")};
    // The first value is the desktop default from StreamPreferenceStore.
    static const QMap<QString, QStringList> options{
        {QStringLiteral("resolution"), {QStringLiteral("host"), QStringLiteral("1280x720"),
            QStringLiteral("1920x1080"), QStringLiteral("2560x1440"), QStringLiteral("3840x2160")}},
        {QStringLiteral("fps"), {QStringLiteral("60"), QStringLiteral("30")}},
        {QStringLiteral("display-mode"), {QStringLiteral("windowed"), QStringLiteral("fullscreen"),
            QStringLiteral("borderless")}},
        {QStringLiteral("vsync"), defaultOn},
        {QStringLiteral("frame-pacing"), defaultOff},
        {QStringLiteral("audio-config"), {QStringLiteral("stereo"), QStringLiteral("5.1-surround"),
            QStringLiteral("7.1-surround")}},
        {QStringLiteral("audio-on-host"), defaultOff},
        {QStringLiteral("mute-on-focus-loss"), defaultOff},
        {QStringLiteral("keep-awake"), defaultOn},
        {QStringLiteral("absolute-mouse"), defaultOn},
        {QStringLiteral("capture-system-keys"), {QStringLiteral("never"), QStringLiteral("fullscreen"),
            QStringLiteral("always")}},
        {QStringLiteral("touchscreen-trackpad"), defaultOff},
        {QStringLiteral("mouse-buttons-swap"), defaultOff},
        {QStringLiteral("reverse-scroll-direction"), defaultOff},
        {QStringLiteral("swap-gamepad-buttons"), defaultOff},
        {QStringLiteral("background-gamepad"), defaultOff},
        {QStringLiteral("video-decoder"), {QStringLiteral("auto"), QStringLiteral("hardware"),
            QStringLiteral("software")}},
        {QStringLiteral("video-codec"), {QStringLiteral("auto"), QStringLiteral("H.264"),
            QStringLiteral("HEVC"), QStringLiteral("AV1")}},
        {QStringLiteral("hdr"), defaultOff},
        {QStringLiteral("performance-overlay"), defaultOff},
        {QStringLiteral("game-optimization"), defaultOn},
        {QStringLiteral("quit-after"), defaultOff},
    };
    return options;
}

bool validStreamValue(const QString &option, const QString &value)
{
    if (option == Bitrate) {
        const int kbps = value.toInt();
        return kbps >= 500 && kbps <= 150000 && value == QString::number(kbps);
    }

    const auto &options = streamOptions();
    const auto entry = options.constFind(option);
    return entry != options.cend() && entry->contains(value);
}

const QStringList &sessionControlOptions()
{
    static const QStringList options{
        QStringLiteral("resolution"), QStringLiteral("fps"), Bitrate, QStringLiteral("video-codec"),
        QStringLiteral("hdr"), QStringLiteral("performance-overlay"), QStringLiteral("audio-config"),
        QStringLiteral("audio-on-host"), QStringLiteral("mute-on-focus-loss"), QStringLiteral("absolute-mouse"),
        QStringLiteral("mouse-buttons-swap"), QStringLiteral("reverse-scroll-direction")
    };
    return options;
}

std::optional<QJsonObject> readStreamFile(const QString &path)
{
    if (path.isEmpty())
        return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const QFileInfo entry(path);
        if (!entry.exists() && !entry.isSymLink())
            return QJsonObject{};
        return std::nullopt;
    }

    const QByteArray data = file.read(MaxStreamBytes + 1);
    if (file.error() != QFileDevice::NoError || data.size() > MaxStreamBytes)
        return std::nullopt;

    const auto document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return std::nullopt;
    return document.object();
}

} // namespace

QString defaultDirectory()
{
    // Both legacy application-data folders on macOS resolve to Application Support.
    // GenericDataLocation is LocalAppData for a normal Windows desktop process.
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return preferencePath(data, QStringLiteral("Aurora"));
}

GeneralPreferences::GeneralPreferences(const QString &directory)
    : directory_(QDir::isAbsolutePath(directory) ? directory : QString())
{
}

bool GeneralPreferences::closeToTray() const
{
    return !directory_.isEmpty() && !QFileInfo(preferencePath(directory_, ExitOnClose)).isFile();
}

bool GeneralPreferences::setCloseToTray(bool enabled) const
{
    return setMarker(preferencePath(directory_, ExitOnClose), !enabled);
}

SleepMode GeneralPreferences::sleepMode() const
{
    QFile file(preferencePath(directory_, HostSleepMode));
    if (!file.open(QIODevice::ReadOnly))
        return SleepMode::System;

    QTextStream text(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    text.setCodec("UTF-8");
#endif
    const QString mode = text.readAll().trimmed();
    if (file.error() != QFileDevice::NoError)
        return SleepMode::System;
    if (mode == QLatin1String("OnAc"))
        return SleepMode::OnAc;
    if (mode == QLatin1String("Always"))
        return SleepMode::Always;
    return SleepMode::System;
}

bool GeneralPreferences::setSleepMode(SleepMode mode) const
{
    const auto path = preferencePath(directory_, HostSleepMode);
    switch (mode) {
    case SleepMode::System:
        return saveAtomically(path, "System");
    case SleepMode::OnAc:
        return saveAtomically(path, "OnAc");
    case SleepMode::Always:
        return saveAtomically(path, "Always");
    }
    return false;
}

bool GeneralPreferences::managedHostAllowed() const
{
    return QFileInfo(preferencePath(directory_, AllowManagedHost)).isFile();
}

bool GeneralPreferences::setManagedHostAllowed(bool allowed) const
{
    return setMarker(preferencePath(directory_, AllowManagedHost), allowed);
}

bool GeneralPreferences::startupInitialized() const
{
    return QFileInfo(preferencePath(directory_, StartupInitialized)).isFile();
}

StartupClaim GeneralPreferences::claimStartupInitialization() const
{
    return createMarker(preferencePath(directory_, StartupInitialized));
}

QString StreamPreferenceSnapshot::value(const QString &option) const
{
    const auto entry = overrides.constFind(option);
    if (entry != overrides.cend() && validStreamValue(option, entry.value()))
        return entry.value();

    if (option == Bitrate) {
        const auto resolution = value(QStringLiteral("resolution"));
        int factor = 5;
        if (resolution == QLatin1String("3840x2160"))
            factor = 40;
        else if (resolution == QLatin1String("2560x1440"))
            factor = 20;
        else if (resolution == QLatin1String("1920x1080"))
            factor = 10;
        return QString::number(factor * (value(QStringLiteral("fps")) == QLatin1String("60") ? 2000 : 1000));
    }

    const auto &options = streamOptions();
    const auto defaults = options.constFind(option);
    return defaults == options.cend() ? QString() : defaults->first();
}

StreamPreferenceSnapshot StreamPreferenceSnapshot::withHostOverrides(const QMap<QString, QString> &hostOverrides) const
{
    auto snapshot = *this;
    for (auto entry = hostOverrides.cbegin(); entry != hostOverrides.cend(); ++entry) {
        if (validStreamValue(entry.key(), entry.value()))
            snapshot.overrides.insert(entry.key(), entry.value());
    }
    return snapshot;
}

QStringList StreamPreferenceSnapshot::arguments(bool useViewerDefault, bool useBundledDefaults) const
{
    QMap<QString, QString> effective;
    for (auto entry = overrides.cbegin(); entry != overrides.cend(); ++entry) {
        if (validStreamValue(entry.key(), entry.value()))
            effective.insert(entry.key(), entry.value());
    }
    if (effective.value(QStringLiteral("resolution")) == QLatin1String("host"))
        effective.remove(QStringLiteral("resolution"));

    const auto addDefault = [&effective](const QString &option) {
        if (!effective.contains(option))
            effective.insert(option, streamOptions().constFind(option)->first());
    };
    if (useViewerDefault)
        addDefault(QStringLiteral("display-mode"));
    addDefault(QStringLiteral("absolute-mouse"));
    if (useBundledDefaults) {
        addDefault(QStringLiteral("vsync"));
        addDefault(QStringLiteral("frame-pacing"));
        addDefault(QStringLiteral("keep-awake"));
    }
    if (effective.value(QStringLiteral("vsync")) == QLatin1String("off"))
        effective.insert(QStringLiteral("frame-pacing"), QStringLiteral("off"));
    if (effective.value(QStringLiteral("hdr")) == QLatin1String("on"))
        effective.insert(QStringLiteral("video-decoder"), QStringLiteral("auto"));

    QStringList arguments;
    for (auto entry = effective.cbegin(); entry != effective.cend(); ++entry) {
        const bool off = entry.value() == QLatin1String("off");
        arguments.append((off ? QStringLiteral("--no-") : QStringLiteral("--")) + entry.key());
        if (!off && entry.value() != QLatin1String("on"))
            arguments.append(entry.value());
    }
    return arguments;
}

QJsonObject StreamPreferenceSnapshot::sessionControlSnapshot() const
{
    QJsonObject snapshot;
    for (const auto &option : sessionControlOptions())
        snapshot.insert(option, value(option));
    return snapshot;
}

std::optional<SessionControlChange> SessionControlChange::fromLine(const QString &line)
{
    static const auto prefix = QStringLiteral("aurora-stream-setting/1\t");
    if (line.size() > 128 || !line.startsWith(prefix))
        return std::nullopt;
    const auto fields = line.mid(prefix.size()).split(QLatin1Char('\t'));
    if (fields.size() != 2)
        return std::nullopt;
    SessionControlChange change{fields.at(0), fields.at(1)};
    return change.isValid() ? std::optional<SessionControlChange>(change) : std::nullopt;
}

bool SessionControlChange::isValid() const
{
    return sessionControlOptions().contains(option) && validStreamValue(option, value);
}

StreamPreferences::StreamPreferences(const QString &directory)
    : path_(QDir::isAbsolutePath(directory) ? preferencePath(directory, StreamFile) : QString())
{
}

StreamPreferenceSnapshot StreamPreferences::read() const
{
    StreamPreferenceSnapshot snapshot;
    const auto document = readStreamFile(path_);
    if (!document)
        return snapshot;
    for (auto entry = document->constBegin(); entry != document->constEnd(); ++entry) {
        if (entry.value().isString() && validStreamValue(entry.key(), entry.value().toString()))
            snapshot.overrides.insert(entry.key(), entry.value().toString());
    }
    return snapshot;
}

bool StreamPreferences::set(const QString &option, const QString &value) const
{
    if (!validStreamValue(option, value) || !makeParentDirectory(path_))
        return false;

    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0))
        return false;
    auto document = readStreamFile(path_);
    if (!document)
        return false;

    document->insert(option, value);
    const auto data = QJsonDocument(*document).toJson(QJsonDocument::Compact);
    return data.size() <= MaxStreamBytes && saveAtomically(path_, data);
}

bool StreamPreferences::reset() const
{
    if (!makeParentDirectory(path_))
        return false;
    QLockFile lock(path_ + QStringLiteral(".lock"));
    return lock.tryLock(0) && saveAtomically(path_, "{}");
}

SessionControlSaveResult StreamPreferences::applySessionControlChange(const SessionControlChange &change) const
{
    if (!change.isValid())
        return SessionControlSaveResult::Ignored;
    return set(change.option, change.value) ? SessionControlSaveResult::Saved : SessionControlSaveResult::SaveFailed;
}

} // namespace Aurora::Preferences
