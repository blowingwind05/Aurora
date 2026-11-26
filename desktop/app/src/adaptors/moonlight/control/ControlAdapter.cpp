// SPDX-License-Identifier: GPL-3.0-only
#include "ControlAdapter.h"

#include "ClientProtocol.h"

#include "EmbeddedStatusClient.h"
#include "MoonlightProcess.h"

#include <QThread>
#include <QtEndian>

#include <utility>

namespace Aurora::Stream::Control {
namespace {

constexpr int powerTimeoutMs = 12000;
constexpr int privacyTimeoutMs = 8000;
constexpr int relayProbeTimeoutMs = 4000;
constexpr int maxOutputBytes = 1024;
constexpr int maxDisplayOutput = 8194;
constexpr int maxAppearanceBytes = 8 * 1024 * 1024;
constexpr int maxAppearanceOutput = ((maxAppearanceBytes + 2) / 3) * 4 + 8;
constexpr int appearanceTimeoutMs = 20000;

bool isPower(Operation operation)
{
    return operation == Operation::Restart || operation == Operation::Shutdown;
}

bool isBitrate(Operation operation)
{
    return operation == Operation::BitrateStatus || operation == Operation::BitrateSet;
}

bool isAppearance(Operation operation)
{
    return operation == Operation::AppearanceRead || operation == Operation::AppearanceSave ||
           operation == Operation::AppearanceReset;
}

std::optional<Stream::Operation> moonlightOperation(Operation operation)
{
    switch (operation) {
    case Operation::Restart: return Stream::Operation::Restart;
    case Operation::Shutdown: return Stream::Operation::Shutdown;
    case Operation::PrivacyStatus: return Stream::Operation::PrivacyStatus;
    case Operation::PrivacyOn: return Stream::Operation::PrivacyOn;
    case Operation::PrivacyOff: return Stream::Operation::PrivacyOff;
    case Operation::PrivacyDisplayOff: return Stream::Operation::PrivacyDisplayOff;
    case Operation::BitrateStatus: return Stream::Operation::BitrateStatus;
    case Operation::BitrateSet: return Stream::Operation::BitrateSet;
    case Operation::AppearanceRead: return Stream::Operation::AppearanceRead;
    case Operation::AppearanceSave: return Stream::Operation::AppearanceSave;
    case Operation::AppearanceReset: return Stream::Operation::AppearanceReset;
    case Operation::DisplayControl: return Stream::Operation::DisplayControl;
    }
    return std::nullopt;
}

bool sameEndpoint(const Discovery::Endpoint &a, const Discovery::Endpoint &b)
{
    return a.port == b.port && a.address.protocol() == b.address.protocol() &&
        a.address.scopeId() == b.address.scopeId() && a.address == b.address;
}

Result parseBitrateResult(const Authorization &grant, const QByteArray &response)
{
    if (response == "unsupported")
        return {Status::CapabilityUnavailable};
    if (response == "stale")
        return {Status::BitrateStale};
    if (response == "busy")
        return {Status::BitrateBusy};
    if (response == "pending")
        return {Status::BitratePending};
    if (response == "expired")
        return {Status::BitrateExpired};
    if (response == "rejected")
        return {Status::BitrateRejected};
    if (response == "failed")
        return {Status::BitrateFailed};

    const bool setting = grant.operation == Operation::BitrateSet;
    AuroraBitrate::Request request;
    if (setting) {
        if (!grant.bitrateChange)
            return {Status::Unconfirmed};
        const auto &change = *grant.bitrateChange;
        request = {change.session.toLatin1(), QByteArray::number(change.kbps), change.revision};
    }
    AuroraBitrate::Confirmation reply;
    if (!AuroraBitrate::parseConfirmation(request, response, reply))
        return {Status::Unconfirmed};

    return {setting ? Status::BitrateApplied : Status::BitrateReported, {},
            VideoBitrate{QString::fromLatin1(reply.session), reply.requestedKbps, reply.encoderKbps,
                         reply.maximumKbps, reply.revision}};
}

Result parseAppearanceResult(Operation operation, const QByteArray &response)
{
    if (response == "unsupported")
        return {Status::CapabilityUnavailable};
    if (operation != Operation::AppearanceRead)
        return {response == "saved" ? Status::Accepted : Status::Unconfirmed};
    if (response == "default")
        return {Status::AppearanceReported};
    if (!response.startsWith("image="))
        return {Status::Unconfirmed};

    const auto encoded = response.mid(6);
    const auto png = QByteArray::fromBase64(encoded);
    if (png.isEmpty() || png.size() > maxAppearanceBytes || encoded != png.toBase64())
        return {Status::InvalidImage};
    if (png.size() < 33 || !png.startsWith(QByteArray::fromHex("89504e470d0a1a0a0000000d49484452")))
        return {Status::InvalidImage};
    const auto width = qFromBigEndian<quint32>(png.constData() + 16);
    const auto height = qFromBigEndian<quint32>(png.constData() + 20);
    if (!width || !height || width > 2880 || height > 2880)
        return {Status::InvalidImage};
    return {Status::AppearanceReported, {}, {}, encoded};
}

Result parseResult(const Authorization &grant, int exitCode, const QByteArray &output)
{
    const auto operation = grant.operation;
    if ((isPower(operation) || isBitrate(operation) || isAppearance(operation) || operation == Operation::DisplayControl) && exitCode == 3)
        return {Status::PairingRequired};
    if (isAppearance(operation) && exitCode == 5)
        return {Status::InvalidImage};
    if (exitCode != 0)
        return {Status::Unconfirmed};

    if (operation == Operation::DisplayControl) {
        const auto response = output.trimmed();
        if (response == "unsupported")
            return {Status::CapabilityUnavailable};
        return {Status::DisplayReported, {}, {}, {}, response};
    }
    if (isBitrate(operation))
        return parseBitrateResult(grant, output.trimmed());
    if (isAppearance(operation))
        return parseAppearanceResult(operation, output.trimmed());
    const auto response = QString::fromUtf8(output.constData(), output.size()).trimmed();
    if (isPower(operation))
        return {response == QLatin1String("accepted") ? Status::Accepted : Status::Unconfirmed};
    if (response == QLatin1String("off"))
        return {Status::PrivacyReported, PrivacyState::Off};
    if (response == QLatin1String("on"))
        return {Status::PrivacyReported, PrivacyState::On};
    if (response == QLatin1String("display-off"))
        return {Status::PrivacyReported, PrivacyState::DisplayOff};
    if (response == QLatin1String("unavailable"))
        return {Status::PrivacyReported, PrivacyState::Unavailable};
    if (operation == Operation::PrivacyStatus && response == QLatin1String("unsupported"))
        return {Status::PrivacyReported, PrivacyState::Unsupported};
    return {Status::Unconfirmed};
}

} // namespace

Adapter::Adapter(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<Result>();
    watchdog_.setTimerType(Qt::PreciseTimer);
    watchdog_.setInterval(25);
    connect(&watchdog_, &QTimer::timeout, this, [this] { checkCurrent(); });
}

Adapter::~Adapter()
{
    // MoonlightProcess kills and reaps in its destructor. Keep the grant and
    // its relay alive until the worker has exited.
    delete worker_;
}

bool Adapter::start(Authorization authorization)
{
    if (QThread::currentThread() != thread() || busy() || authorization.token.isNull())
        return false;

    const int timeout = (isAppearance(authorization.operation) || authorization.operation == Operation::DisplayControl) ? appearanceTimeoutMs
        : isPower(authorization.operation) ? powerTimeoutMs : privacyTimeoutMs;
    deadline_.setRemainingTime(timeout, Qt::PreciseTimer);
    authorization_ = std::move(authorization);
    QTimer::singleShot(0, this, &Adapter::begin);
    return true;
}

bool Adapter::cancel(const QUuid &token)
{
    if (QThread::currentThread() != thread() || !authorization_ || authorization_->token != token || stopped_)
        return false;

    if (worker_)
        stop(Status::Cancelled);
    else
        stopped_ = Status::Cancelled; // begin() will complete asynchronously.
    return true;
}

void Adapter::begin()
{
    if (stopped_) {
        finish({*stopped_});
        return;
    }

    const auto &grant = *authorization_;
    const auto operation = moonlightOperation(grant.operation);
    if (!operation || !grant.stillCurrent) {
        finish({Status::InvalidAuthorization});
        return;
    }
    if (!checkCurrent())
        return;

    const auto &selected = grant.selected;
    const auto &check = grant.revalidated;
    const bool power = isPower(grant.operation);
    const bool pinnedPlayback = !check && selected.publicIpv6 && !selected.viaEmbeddedTailnet &&
        selected.endpoint.address.protocol() == QAbstractSocket::IPv6Protocol &&
        grant.certificateSha256.size() == 32 && !power;
    if (check && check->status == Discovery::ServerInfo::Status::PowerUnavailable && power) {
        finish({Status::CapabilityUnavailable});
        return;
    }
    if (!pinnedPlayback && (!check || check->status != Discovery::ServerInfo::Status::Complete || !check->current ||
        !sameEndpoint(selected.endpoint, check->current->endpoint) ||
        !Discovery::sameHostIdentity(selected.id, check->current->id) ||
        selected.viaEmbeddedTailnet != check->current->viaEmbeddedTailnet)) {
        finish({Status::RouteChanged});
        return;
    }
    if (check && ((power && !check->current->supportsPower) ||
        (!power && !isBitrate(grant.operation) && grant.operation != Operation::DisplayControl && !check->current->supportsSessionPrivacy))) {
        finish({Status::CapabilityUnavailable});
        return;
    }

    auto endpoint = selected.endpoint;
    if (selected.viaEmbeddedTailnet) {
        if (!grant.lease || !grant.checkedRelay ||
            !sameEndpoint(*grant.checkedRelay, {grant.lease->address(), grant.lease->port()})) {
            finish({Status::RouteChanged});
            return;
        }
        endpoint = *grant.checkedRelay;
    } else if (grant.lease || grant.checkedRelay || endpoint.address.isLoopback()) {
        finish({Status::RouteChanged});
        return;
    }

    Request request;
    request.operation = *operation;
    request.route = {endpoint.address, endpoint.port, selected.id, selected.viaEmbeddedTailnet, grant.certificateSha256};
    request.bitrate = grant.bitrateChange;
    request.appearanceFile = grant.appearanceFile;
    request.display = grant.display;
    auto command = makeCommand(grant.executable, request);
    if (!command.command) {
        const bool runtimeError = command.error == CommandError::UnsupportedPlatform ||
            command.error == CommandError::InvalidProgram || command.error == CommandError::BundledRuntimeRequired;
        finish({runtimeError ? Status::RuntimeUnavailable : Status::RouteChanged});
        return;
    }

    if (selected.viaEmbeddedTailnet && power) {
        const auto probe = makeProbeCommand(grant.executable, Probe::GameStreamRelay);
        if (!probe.command) {
            finish({Status::RuntimeUnavailable});
            return;
        }
        pairedCommand_ = std::move(command.command);
        relayDeadline_.setRemainingTime(relayProbeTimeoutMs, Qt::PreciseTimer);
        probing_ = true;
        launch(*probe.command);
        return;
    }
    launch(*command.command);
}

void Adapter::launch(const Command &command)
{
    if (!checkCurrent())
        return;

    worker_ = new MoonlightProcess(this);
    connect(worker_, &QProcess::readyReadStandardOutput, this, [this] {
        if (!stopped_ && !drainOutput())
            stop(Status::OutputLimitExceeded);
    });
    connect(worker_, &QProcess::started, this, [this] {
        if (stopped_)
            worker_->cancel();
        else
            checkCurrent();
    });
    connect(worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            stop(probing_ ? Status::RelayProbeFailed : Status::StartFailed);
        else if (error == QProcess::ReadError)
            stop(probing_ ? Status::RelayProbeFailed : Status::Unconfirmed);
    });
    connect(worker_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &Adapter::workerFinished);
    watchdog_.start();
    if (!worker_->launch(command))
        stop(probing_ ? Status::RelayProbeFailed : Status::StartFailed);
}

bool Adapter::drainOutput()
{
    const int limit = authorization_->operation == Operation::AppearanceRead ? maxAppearanceOutput
        : authorization_->operation == Operation::DisplayControl ? maxDisplayOutput : maxOutputBytes;
    const auto bytes = worker_->read(limit - output_.size() + 1);
    if (bytes.size() > limit - output_.size()) {
        worker_->closeReadChannel(QProcess::StandardOutput);
        return false;
    }
    output_.append(bytes);
    return true;
}

void Adapter::workerFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (stopped_) {
        finish({*stopped_});
        return;
    }
    if (!checkCurrent())
        return;
    if (!drainOutput()) {
        finish({Status::OutputLimitExceeded});
        return;
    }
    if (probing_) {
        if (exitStatus != QProcess::NormalExit || !probeSucceeded(Probe::GameStreamRelay, exitCode, output_)) {
            finish({Status::RelayProbeFailed});
            return;
        }
        worker_->disconnect(this);
        worker_->deleteLater();
        worker_ = nullptr;
        output_.clear();
        probing_ = false;
        launch(*pairedCommand_);
        return;
    }
    finish(exitStatus == QProcess::NormalExit ? parseResult(*authorization_, exitCode, output_)
                                             : Result{Status::WorkerCrashed});
}

bool Adapter::checkCurrent()
{
    if (!authorization_ || stopped_)
        return false;

    const auto &grant = *authorization_;
    if (!grant.stillCurrent(grant.token))
        stop(Status::Cancelled);
    else if (grant.lease && !grant.lease->isValid())
        stop(Status::RouteChanged);
    else if (deadline_.hasExpired())
        stop(Status::TimedOut);
    else if (probing_ && relayDeadline_.hasExpired())
        stop(Status::RelayProbeFailed);
    else
        return true;
    return false;
}

void Adapter::stop(Status status)
{
    if (!stopped_)
        stopped_ = status;
    watchdog_.stop();
    if (worker_ && worker_->state() != QProcess::NotRunning)
        worker_->cancel();
    else
        finish({*stopped_});
}

void Adapter::finish(Result result)
{
    watchdog_.stop();
    if (worker_) {
        worker_->disconnect(this);
        worker_->deleteLater();
        worker_ = nullptr;
    }
    const auto token = authorization_->token;
    authorization_.reset();
    stopped_.reset();
    pairedCommand_.reset();
    output_.clear();
    probing_ = false;
    emit finished(token, result);
}

} // namespace Aurora::Stream::Control
