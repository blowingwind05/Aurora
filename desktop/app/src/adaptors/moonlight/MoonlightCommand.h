// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>

#include <functional>
#include <optional>

namespace Aurora::Stream {

enum class RuntimePlatform { Windows, MacOS, Unsupported };
RuntimePlatform currentPlatform();

struct RuntimeSearch
{
    RuntimePlatform platform = currentPlatform();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList macApplicationDirectories;
};

QString findMoonlight(const QString &applicationDirectory, const RuntimeSearch &search = {});
bool isBundledMoonlight(const QString &program);

struct Route
{
    QHostAddress address;
    int port = 0;
    QString hostId;
    bool viaEmbeddedTailnet = false;
    QByteArray certificateSha256;
};

enum class Operation { Stream, Pair, BackgroundPair, List, Wallpaper, Restart, Shutdown,
                       PrivacyStatus, PrivacyOn, PrivacyOff, PrivacyDisplayOff, BitrateStatus, BitrateSet, RouteProbe };
enum class Probe { Runtime, EmbeddedWindow, GameStreamRelay, BackgroundPair, HostIdentity };

struct BitrateChange
{
    QString session;
    int kbps;
    quint32 revision;
};

bool validBitrateSession(const QString &session);

// Three HTTPS checks use the same endpoint and pinned host identity.
struct RouteQuality
{
    int successfulChecks;
    int medianMilliseconds;
};

std::optional<RouteQuality> parseRouteQuality(const QByteArray &response);
bool betterRouteQuality(RouteQuality current, RouteQuality candidate);

struct PreferenceDefaults
{
    bool useViewerDefault;
    bool useBundledDefaults;
};

struct Request
{
    Operation operation = Operation::Stream;
    Route route;
    QString pairingId;
    QString pin;
    QString wallpaperRevision;

    // Playback options supplied by the preference and session owners.
    std::function<QStringList(PreferenceDefaults)> preferenceArguments;
    QString embeddedInvitation;
    bool readConnectionDetails = false;
    QString deviceName;
    QJsonObject sessionPreferences;
    std::optional<BitrateChange> bitrate;
    bool resumeOnly = false;
};

struct Command
{
    QString program;
    QString workingDirectory;
    QStringList arguments;
    QProcessEnvironment environment;
    bool captureOutput = false;
};

enum class CommandError { None, UnsupportedPlatform, InvalidProgram, BundledRuntimeRequired,
                          InvalidEndpoint, InvalidRelay, InvalidArguments };

struct CommandResult
{
    std::optional<Command> command;
    CommandError error;
};

CommandResult makeCommand(const QString &program, const Request &request,
    RuntimePlatform platform = currentPlatform(),
    const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment());
CommandResult makeProbeCommand(const QString &program, Probe probe,
    RuntimePlatform platform = currentPlatform(),
    const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment());
CommandResult makeClientProofCommand(const QString &program, const QByteArray &challenge,
    const QByteArray &hostFingerprint, RuntimePlatform platform = currentPlatform(),
    const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment());
inline constexpr int MaxClientProofOutputBytes = 8192;
struct ClientProofResult
{
    QByteArray certificate;
    QByteArray signature;
};
std::optional<ClientProofResult> parseClientProofOutput(const QByteArray &output);

bool probeSucceeded(Probe probe, int exitCode, const QByteArray &output);

} // namespace Aurora::Stream
