// SPDX-License-Identifier: GPL-3.0-only
#include "MoonlightCommand.h"
#include "../../../../protocol/identity/ClientProof.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>

#include <utility>

namespace Aurora::Stream {
namespace {

const auto embeddedKey = QStringLiteral("AURORA_EMBEDDED_SESSION");
const auto deviceNameKey = QStringLiteral("AURORA_STREAM_DEVICE_NAME");
const auto preferencesKey = QStringLiteral("AURORA_STREAM_PREFERENCES");
const auto detailsKey = QStringLiteral("AURORA_CONNECTION_DETAILS");
const auto relayBaseKey = QStringLiteral("AURORA_GAMESTREAM_RELAY_BASE");
const auto relayHostKey = QStringLiteral("AURORA_GAMESTREAM_HOST_UUID");
const auto pairedHostKey = QStringLiteral("AURORA_GAMESTREAM_PAIRED_HOST_UUID");
const auto hostCertificateKey = QStringLiteral("AURORA_GAMESTREAM_HOST_CERTIFICATE_SHA256");
const auto resumeOnlyKey = QStringLiteral("AURORA_STREAM_RESUME_ONLY");
const auto runtimeFlag = QStringLiteral("--aurora-runtime");

bool absoluteFilesystemPath(const QString &path)
{
    if (path.contains(QChar::Null))
        return false;
#ifdef Q_OS_WIN
    static const QRegularExpression root(QStringLiteral(R"(\A(?:[A-Za-z]:/|//[^/]+/[^/]+(?:/|\z)))"));
    return root.match(QDir::fromNativeSeparators(path)).hasMatch();
#else
    return path.startsWith(QLatin1Char('/'));
#endif
}

QString existingProgram(const QString &directory, const QString &relativePath)
{
    if (!absoluteFilesystemPath(directory))
        return {};
    const auto file = QDir(directory).filePath(relativePath);
    return QFileInfo(file).isFile() ? file : QString();
}

QString windowsInstallRoot(const QString &directory)
{
    QDir root(QDir::cleanPath(QDir::fromNativeSeparators(directory)));
    if (root.dirName().compare(QStringLiteral("Moonlight"), Qt::CaseInsensitive) == 0
        && root.cdUp()
        && root.dirName().compare(QStringLiteral("Components"), Qt::CaseInsensitive) == 0
        && root.cdUp()) {
        return root.path();
    }
    return directory;
}

QString endpointArgument(const Route &route)
{
    const auto family = route.address.protocol();
    if (route.port <= 0 || route.port > 65535 || !route.address.scopeId().isEmpty()
        || route.address.isMulticast() || route.address.isBroadcast()) {
        return {};
    }
    if (family == QAbstractSocket::IPv4Protocol && route.address != QHostAddress::AnyIPv4)
        return QStringLiteral("%1:%2").arg(route.address.toString()).arg(route.port);
    if (family == QAbstractSocket::IPv6Protocol && route.address != QHostAddress::AnyIPv6)
        return QStringLiteral("[%1]:%2").arg(route.address.toString()).arg(route.port);
    return {};
}

bool validHostId(const QString &id)
{
    return !id.trimmed().isEmpty() && id.size() <= 128 && !id.contains(QChar::Null);
}

Command baseCommand(const QString &program, QProcessEnvironment environment)
{
    for (const auto &key : {embeddedKey, deviceNameKey, preferencesKey, detailsKey, relayBaseKey, relayHostKey, pairedHostKey, hostCertificateKey, resumeOnlyKey})
        environment.remove(key);
    return {program, QFileInfo(program).absolutePath(), {}, std::move(environment), true};
}

QStringList operationArguments(const Request &request, const QString &endpoint)
{
    switch (request.operation) {
    case Operation::Stream:
        return {QStringLiteral("stream"), endpoint, QStringLiteral("Desktop")};
    case Operation::Pair:
        return {QStringLiteral("pair"), endpoint};
    case Operation::BackgroundPair: {
        static const QRegularExpression id(QStringLiteral(R"(\A[0-9a-f]{32}\z)"));
        static const QRegularExpression pin(QStringLiteral(R"(\A[0-9]{4}\z)"));
        if (!id.match(request.pairingId).hasMatch() || !pin.match(request.pin).hasMatch())
            return {};
        return {QStringLiteral("pair"), QStringLiteral("--aurora-background"),
                QStringLiteral("--aurora-pair-id"), request.pairingId, QStringLiteral("--pin"), request.pin, endpoint};
    }
    case Operation::List:
        return {QStringLiteral("list"), endpoint};
    case Operation::Wallpaper: {
        static const QRegularExpression revision(QStringLiteral(R"(\A[A-F0-9]{64}\z)"));
        if (!validHostId(request.route.hostId)
            || (!request.wallpaperRevision.isEmpty() && !revision.match(request.wallpaperRevision).hasMatch())) {
            return {};
        }
        return {QStringLiteral("wallpaper"), endpoint, request.route.hostId,
                request.wallpaperRevision.isEmpty() ? QStringLiteral("-") : request.wallpaperRevision};
    }
    case Operation::Restart:
    case Operation::Shutdown:
    case Operation::PrivacyStatus:
    case Operation::PrivacyOn:
    case Operation::PrivacyOff:
    case Operation::PrivacyDisplayOff:
    case Operation::AppearanceRead:
    case Operation::AppearanceSave:
    case Operation::AppearanceReset:
    case Operation::BitrateStatus:
    case Operation::BitrateSet:
    case Operation::DisplayControl:
    case Operation::RouteProbe:
        break;
    }

    static const QRegularExpression uuid(QStringLiteral(R"(\A[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\z)"));
    if (!uuid.match(request.route.hostId).hasMatch() || request.route.port <= 5)
        return {};
    const auto &id = request.route.hostId;
    switch (request.operation) {
    case Operation::RouteProbe:
        return request.route.certificateSha256.size() == 32
            ? QStringList{QStringLiteral("route-probe"), endpoint, id, QStringLiteral("sample")} : QStringList{};
    case Operation::Restart: return {QStringLiteral("power"), endpoint, id, QStringLiteral("restart")};
    case Operation::Shutdown: return {QStringLiteral("power"), endpoint, id, QStringLiteral("shutdown")};
    case Operation::PrivacyStatus: return {QStringLiteral("privacy"), endpoint, id, QStringLiteral("status")};
    case Operation::PrivacyOn: return {QStringLiteral("privacy"), endpoint, id, QStringLiteral("on")};
    case Operation::PrivacyOff: return {QStringLiteral("privacy"), endpoint, id, QStringLiteral("off")};
    case Operation::PrivacyDisplayOff: return {QStringLiteral("privacy"), endpoint, id, QStringLiteral("display-off")};
    case Operation::AppearanceRead:
        return {QStringLiteral("privacy-appearance"), endpoint, id, QStringLiteral("read")};
    case Operation::AppearanceReset:
        return {QStringLiteral("privacy-appearance"), endpoint, id, QStringLiteral("reset")};
    case Operation::AppearanceSave:
        if (!absoluteFilesystemPath(request.appearanceFile) || !QFileInfo(request.appearanceFile).isFile())
            return {};
        return {QStringLiteral("privacy-appearance"), endpoint, id, QStringLiteral("save"), request.appearanceFile};
    case Operation::DisplayControl: {
        const auto &display = request.display;
        QStringList args{QStringLiteral("display-control"), endpoint, id, display.operation};
        if (display.id == 0 && (display.operation == QLatin1String("list") || display.operation == QLatin1String("create")))
            return args;
        if (display.id != 0 && (display.operation == QLatin1String("select") || display.operation == QLatin1String("remove"))) {
            args.append(QString::number(display.id));
            return args;
        }
        return {};
    }
    case Operation::BitrateStatus: return {QStringLiteral("bitrate"), endpoint, id, QStringLiteral("status")};
    case Operation::BitrateSet: {
        if (!request.bitrate || !validBitrateSession(request.bitrate->session) ||
            request.bitrate->kbps < 500 || request.bitrate->kbps > 150000)
            return {};
        return {QStringLiteral("bitrate"), endpoint, id, QStringLiteral("set"), request.bitrate->session,
                QString::number(request.bitrate->kbps), QString::number(request.bitrate->revision)};
    }
    default: return {};
    }
}

bool validInvitation(const QString &invitation)
{
    static const QRegularExpression pattern(QStringLiteral(R"(\A(-?[0-9]+):([0-9]+):[0-9a-f]{32}\z)"));
    const auto match = pattern.match(invitation);
    return match.hasMatch() && match.captured(1).toLongLong() != 0 && match.captured(2).toUInt() != 0;
}

QStringList playbackArguments(const Request &request, PreferenceDefaults defaults)
{
    if (request.preferenceArguments)
        return request.preferenceArguments(defaults);

    // Same flags as the .NET preference store with no saved overrides.
    QStringList arguments;
    if (defaults.useViewerDefault)
        arguments << QStringLiteral("--display-mode") << QStringLiteral("windowed");
    arguments << QStringLiteral("--absolute-mouse");
    if (defaults.useBundledDefaults)
        arguments << QStringLiteral("--vsync") << QStringLiteral("--no-frame-pacing") << QStringLiteral("--keep-awake");
    return arguments;
}

CommandResult failure(CommandError error)
{
    return {std::nullopt, error};
}

} // namespace

bool validBitrateSession(const QString &session)
{
    static const QRegularExpression pattern(QStringLiteral(R"(\A[0-9a-f]{32}\z)"));
    return pattern.match(session).hasMatch();
}

std::optional<RouteQuality> parseRouteQuality(const QByteArray &response)
{
    static const QRegularExpression pattern(QStringLiteral(R"(\Aaurora-route/1,([0-3]),([0-9]{1,5})\z)"));
    const auto match = pattern.match(QString::fromLatin1(response.trimmed()));
    if (!match.hasMatch())
        return std::nullopt;

    const int successful = match.captured(1).toInt();
    const int median = match.captured(2).toInt();
    if ((successful == 0 && median != 0) || median > 5000)
        return std::nullopt;
    return RouteQuality{successful, median};
}

bool betterRouteQuality(RouteQuality current, RouteQuality candidate)
{
    if (candidate.successfulChecks < 2 || candidate.successfulChecks < current.successfulChecks)
        return false;
    if (!current.successfulChecks)
        return true;
    const int threshold = qMax(15, current.medianMilliseconds / 5);
    const int improvement = current.medianMilliseconds - candidate.medianMilliseconds;
    return improvement >= threshold ||
        (candidate.successfulChecks > current.successfulChecks && improvement >= 0);
}

RuntimePlatform currentPlatform()
{
#ifdef Q_OS_WIN
    return RuntimePlatform::Windows;
#elif defined(Q_OS_MACOS)
    return RuntimePlatform::MacOS;
#else
    return RuntimePlatform::Unsupported;
#endif
}

bool isBundledMoonlight(const QString &program)
{
    return QFileInfo(program).completeBaseName() == QStringLiteral("AuroraStream");
}

QString findMoonlight(const QString &applicationDirectory, const RuntimeSearch &search)
{
    if (search.platform == RuntimePlatform::MacOS) {
        const auto integrated = existingProgram(applicationDirectory, QStringLiteral("AuroraStream"));
        if (!integrated.isEmpty())
            return integrated;
        auto roots = search.macApplicationDirectories;
        if (roots.isEmpty())
            roots = QStringList{QStringLiteral("/Applications"), QDir::home().filePath(QStringLiteral("Applications"))};
        for (const auto &root : roots) {
            const auto bundle = QDir(root).filePath(QStringLiteral("Moonlight.app"));
            if (absoluteFilesystemPath(root) && QFileInfo(bundle).isDir())
                return existingProgram(bundle, QStringLiteral("Contents/MacOS/Moonlight"));
        }
        return {};
    }

    if (search.platform != RuntimePlatform::Windows)
        return {};
    const auto root = windowsInstallRoot(applicationDirectory);
    for (const auto &relativePath : {QStringLiteral("Components/Moonlight/AuroraStream.exe"),
                                     QStringLiteral("Streamer/AuroraStream.exe"),
                                     QStringLiteral("Moonlight/Moonlight.exe")}) {
        const auto program = existingProgram(root, relativePath);
        if (!program.isEmpty())
            return program;
    }
    for (const auto &key : {QStringLiteral("ProgramW6432"), QStringLiteral("ProgramFiles"),
                            QStringLiteral("ProgramFiles(x86)")}) {
        const auto program = existingProgram(search.environment.value(key),
                                             QStringLiteral("Moonlight Game Streaming/Moonlight.exe"));
        if (!program.isEmpty())
            return program;
    }
    for (auto path : search.environment.value(QStringLiteral("PATH")).split(QLatin1Char(';'))) {
        path = path.trimmed();
        if (path.size() >= 2 && path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')))
            path = path.mid(1, path.size() - 2);
        const auto program = existingProgram(path, QStringLiteral("Moonlight.exe"));
        if (!program.isEmpty())
            return program;
    }
    return {};
}

CommandResult makeCommand(const QString &program, const Request &request, RuntimePlatform platform,
                          const QProcessEnvironment &environment)
{
    if (platform == RuntimePlatform::Unsupported)
        return failure(CommandError::UnsupportedPlatform);
    if (!absoluteFilesystemPath(program) || QFileInfo(program).fileName().isEmpty())
        return failure(CommandError::InvalidProgram);

    const auto endpoint = endpointArgument(request.route);
    if (endpoint.isEmpty())
        return failure(CommandError::InvalidEndpoint);

    const bool bundled = isBundledMoonlight(program);
    const bool playback = request.operation == Operation::Stream;
    const bool upstreamCommand = playback || request.operation == Operation::Pair || request.operation == Operation::List;
    if (!bundled && (!upstreamCommand || request.route.viaEmbeddedTailnet))
        return failure(CommandError::BundledRuntimeRequired);

    auto command = baseCommand(program, environment);
    if (!request.route.certificateSha256.isEmpty()) {
        if (!bundled || request.route.certificateSha256.size() != 32 || !validHostId(request.route.hostId))
            return failure(CommandError::InvalidArguments);
        command.environment.insert(hostCertificateKey, QString::fromLatin1(request.route.certificateSha256.toHex()));
    }
    if (request.route.viaEmbeddedTailnet) {
        // The worker reserves base - 5 through base + 21. Match AuroraStreamEntry.h.
        if (request.route.address.protocol() != QAbstractSocket::IPv4Protocol
            || request.route.address != QHostAddress::LocalHost
            || request.route.port <= 5 || request.route.port > 65535 - 21
            || !validHostId(request.route.hostId)) {
            return failure(CommandError::InvalidRelay);
        }
        command.environment.insert(relayBaseKey, QString::number(request.route.port));
        command.environment.insert(relayHostKey, request.route.hostId);
    }

    if (bundled && (playback || request.operation == Operation::List) && validHostId(request.route.hostId))
        command.environment.insert(pairedHostKey, request.route.hostId);

    command.arguments = operationArguments(request, endpoint);
    if (command.arguments.isEmpty())
        return failure(CommandError::InvalidArguments);
    if (bundled)
        command.arguments.prepend(runtimeFlag);

    if (playback) {
        if (request.resumeOnly) {
            if (!bundled || request.route.certificateSha256.size() != 32)
                return failure(CommandError::InvalidArguments);
            command.environment.insert(resumeOnlyKey, QStringLiteral("1"));
        }
        const bool embedded = !request.embeddedInvitation.isEmpty();
        if (embedded && (platform != RuntimePlatform::Windows || !bundled || !validInvitation(request.embeddedInvitation)))
            return failure(CommandError::InvalidArguments);
        if (embedded)
            command.environment.insert(embeddedKey, request.embeddedInvitation);

        const bool viewer = bundled && (platform == RuntimePlatform::MacOS || embedded);
        const auto arguments = playbackArguments(request, {viewer, bundled});
        for (const auto &argument : arguments) {
            if (argument.contains(QChar::Null))
                return failure(CommandError::InvalidArguments);
        }
        command.arguments.append(arguments);

        command.captureOutput = request.readConnectionDetails || platform == RuntimePlatform::MacOS;
        if (request.readConnectionDetails)
            command.environment.insert(detailsKey, QStringLiteral("1"));
        if (platform == RuntimePlatform::MacOS && !request.deviceName.trimmed().isEmpty()) {
            if (request.deviceName.contains(QChar::Null))
                return failure(CommandError::InvalidArguments);
            command.environment.insert(deviceNameKey, request.deviceName);
            command.environment.insert(preferencesKey,
                QString::fromUtf8(QJsonDocument(request.sessionPreferences).toJson(QJsonDocument::Compact)));
        }
    } else if (request.operation == Operation::Pair) {
        command.captureOutput = false;
    }
    return {std::move(command), CommandError::None};
}

CommandResult makeClientProofCommand(const QString &program, const QByteArray &challenge,
    const QByteArray &hostFingerprint, RuntimePlatform platform, const QProcessEnvironment &environment)
{
    if (platform == RuntimePlatform::Unsupported)
        return failure(CommandError::UnsupportedPlatform);
    if (!isBundledMoonlight(program) || !absoluteFilesystemPath(program))
        return failure(CommandError::BundledRuntimeRequired);
    if (challenge.size() != ClientProof::ChallengeSize || hostFingerprint.size() != ClientProof::DigestSize ||
        challenge.mid(ClientProof::DigestSize, ClientProof::DigestSize) != hostFingerprint)
        return failure(CommandError::InvalidArguments);

    auto command = baseCommand(program, environment);
    command.environment.insert(hostCertificateKey, QString::fromLatin1(hostFingerprint.toHex()));
    command.arguments = QStringList{runtimeFlag, QStringLiteral("client-proof"), QString::fromLatin1(challenge.toHex())};
    return {std::move(command), CommandError::None};
}

std::optional<ClientProofResult> parseClientProofOutput(const QByteArray &output)
{
    if (output.size() > MaxClientProofOutputBytes)
        return {};
    const auto fields = output.trimmed().split(',');
    if (fields.size() != 3 || fields.at(0) != ClientProof::Domain)
        return {};
    ClientProofResult proof{QByteArray::fromBase64(fields.at(1)), QByteArray::fromBase64(fields.at(2))};
    if (proof.certificate.isEmpty() || proof.certificate.size() > ClientProof::MaxCertificateBytes ||
        proof.signature.isEmpty() || proof.signature.size() > ClientProof::MaxSignatureBytes ||
        proof.certificate.toBase64() != fields.at(1) || proof.signature.toBase64() != fields.at(2))
        return {};
    return proof;
}

CommandResult makeProbeCommand(const QString &program, Probe probe, RuntimePlatform platform,
                               const QProcessEnvironment &environment)
{
    if (platform == RuntimePlatform::Unsupported || (probe == Probe::EmbeddedWindow && platform != RuntimePlatform::Windows))
        return failure(CommandError::UnsupportedPlatform);
    if (!absoluteFilesystemPath(program))
        return failure(CommandError::InvalidProgram);
    if (!isBundledMoonlight(program))
        return failure(CommandError::BundledRuntimeRequired);

    auto command = baseCommand(program, environment);
    switch (probe) {
    case Probe::HostIdentity: command.arguments = QStringList{QStringLiteral("--aurora-host-identity-info")}; break;
    case Probe::Runtime: command.arguments = QStringList{QStringLiteral("--aurora-runtime-info")}; break;
    case Probe::EmbeddedWindow: command.arguments = QStringList{QStringLiteral("--aurora-embedded-info")}; break;
    case Probe::GameStreamRelay: command.arguments = QStringList{QStringLiteral("--aurora-gamestream-relay-info")}; break;
    case Probe::BackgroundPair:
        command.arguments = QStringList{runtimeFlag, QStringLiteral("pair"), QStringLiteral("--aurora-background"),
                                        QStringLiteral("--help")};
        break;
    default: return failure(CommandError::InvalidArguments);
    }
    return {std::move(command), CommandError::None};
}

bool probeSucceeded(Probe probe, int exitCode, const QByteArray &output)
{
    if (exitCode != 0)
        return false;
    switch (probe) {
    case Probe::HostIdentity: return output.trimmed() == "aurora-host-identity/1";
    case Probe::Runtime: return output.trimmed() == "aurora-stream/1";
    case Probe::EmbeddedWindow: return output.trimmed() == "aurora-embedded/1";
    case Probe::GameStreamRelay: return output.trimmed() == "aurora-gamestream-relay/1";
    case Probe::BackgroundPair: return output.contains("--aurora-background") && output.contains("--aurora-pair-id");
    }
    return false;
}

} // namespace Aurora::Stream
