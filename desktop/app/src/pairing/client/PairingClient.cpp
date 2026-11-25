// SPDX-License-Identifier: GPL-3.0-only
#include "PairingClient.h"
#include "MoonlightPin.h"
#include "../PairingWire.h"
#include "../../stream/MoonlightProcess.h"

#include <QDeadlineTimer>
#include <QIODevice>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace Aurora::Pairing::Client {
namespace {

constexpr int OfferSize = 37;
constexpr int MaxProbeOutput = 1024;

void clearSecret(QByteArray &bytes)
{
    volatile char *data = bytes.data();
    for (decltype(bytes.size()) i = 0; i < bytes.size(); ++i)
        data[i] = 0;
    bytes.clear();
}

Stream::Request moonlightRequest(const Request &request, Stream::Operation operation)
{
    Stream::Request command;
    command.operation = operation;
    command.route = request.leasedRoute;
    return command;
}

bool stdioDevice(QIODevice *device, QThread *thread)
{
    return device && device->thread() == thread && device->isOpen() && device->isSequential() &&
        !qobject_cast<QAbstractSocket *>(device) && !(device->openMode() & QIODevice::Text);
}

} // namespace

struct Coordinator::Attempt : QObject
{
    enum class Phase { Challenge, Writing, Offer, Capability, Pairing, Trust, Stopping };

    Attempt(Coordinator *owner, quint64 id, quint64 revision, const Request &request,
            QIODevice *reader, QIODevice *writer, QTcpSocket *socket, QByteArray key, Timeouts timeouts)
        : QObject(owner), id(id), revision(revision), owner(owner), request(request),
          reader(reader), writer(writer), socket(socket), key(std::move(key)), timeouts(timeouts),
          overall(timeouts.overallMs, Qt::PreciseTimer), overallAlarm(this), stageAlarm(this),
          phase(socket ? Phase::Challenge : Phase::Writing)
    {
        overallAlarm.setSingleShot(true);
        overallAlarm.setTimerType(Qt::PreciseTimer);
        connect(&overallAlarm, &QTimer::timeout, this, [this] { stop(Status::Timeout); });
        overallAlarm.start(static_cast<int>(overall.remainingTime()));

        stageAlarm.setSingleShot(true);
        stageAlarm.setTimerType(Qt::PreciseTimer);
        connect(&stageAlarm, &QTimer::timeout, this, [this] { stop(overall.hasExpired() ? Status::Timeout : stageError); });

        connect(request.leaseLifetime, &QObject::destroyed, this, [this] { stop(Status::LeaseClosed); });
        connect(reader, &QIODevice::readyRead, this, [this] { readFrames(); }, Qt::QueuedConnection);
        connect(reader, &QIODevice::readChannelFinished, this, [this] { transportClosed(); }, Qt::QueuedConnection);
        connect(reader, &QIODevice::aboutToClose, this, [this] { transportClosed(); }, Qt::QueuedConnection);
        connect(reader, &QObject::destroyed, this, [this] { transportClosed(); });
        connect(writer, &QIODevice::bytesWritten, this, [this] { sendBytes(); }, Qt::QueuedConnection);
        if (reader != writer) {
            connect(writer, &QIODevice::aboutToClose, this, [this] { transportClosed(); }, Qt::QueuedConnection);
            connect(writer, &QObject::destroyed, this, [this] { transportClosed(); });
        }
        if (socket) {
            socket->setReadBufferSize(BridgeKeySize + OfferSize + 1);
            connect(socket, &QTcpSocket::disconnected, this, [this] { transportClosed(); }, Qt::QueuedConnection);
            connect(socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
                if (error != QAbstractSocket::RemoteHostClosedError)
                    transportClosed();
            }, Qt::QueuedConnection);
            armStage(timeouts.bridgeMs, Status::BridgeTimeout);
        } else {
            outgoing = requestFrame();
        }

        QTimer::singleShot(0, this, [this] {
            if (phase == Phase::Challenge)
                readFrames();
            else
                sendBytes();
        });
    }

    ~Attempt() override
    {
        closeControl();
        clearSecret(key);
        clearSecret(frame);
        clearSecret(outgoing);
        clearSecret(pendingOffer.pin);
    }

    void armStage(int milliseconds, Status expiry)
    {
        stage = QDeadlineTimer(milliseconds, Qt::PreciseTimer);
        stageError = expiry;
        stageAlarm.start(milliseconds);
    }

    bool timedOut()
    {
        if (overall.hasExpired()) {
            stop(Status::Timeout);
            return true;
        }
        if (stage.hasExpired()) {
            stop(stageError);
            return true;
        }
        return false;
    }

    bool receivingOffer() const
    {
        return phase == Phase::Challenge || phase == Phase::Writing || phase == Phase::Offer;
    }

    void transportClosed()
    {
        if (phase == Phase::Capability) {
            stop(Status::TransportError);
            return;
        }
        // The host commonly closes its side once the PIN was submitted.
        if (!receivingOffer())
            return;
        readFrames();
        if (receivingOffer())
            stop(Status::TransportError);
    }

    void sendBytes()
    {
        if (phase != Phase::Writing || timedOut())
            return;
        if (!writer || !writer->isWritable()) {
            stop(Status::TransportError);
            return;
        }

        while (sent < outgoing.size()) {
            const auto count = writer->write(outgoing.constData() + sent, outgoing.size() - sent);
            if (count < 0) {
                stop(Status::TransportError);
                return;
            }
            if (count == 0)
                return;
            sent += count;
        }
        if (socket)
            socket->flush();
        clearSecret(outgoing);
        phase = Phase::Offer;
        armStage(timeouts.offerMs, Status::OfferTimeout);
        readFrames();
    }

    bool readTo(int length)
    {
        if (!reader || !reader->isReadable()) {
            stop(Status::TransportError);
            return false;
        }
        const int previous = static_cast<int>(frame.size());
        frame.resize(length);
        const auto got = reader->read(frame.data() + previous, length - previous);
        if (got < 0) {
            stop(Status::TransportError);
            return false;
        }
        frame.resize(previous + static_cast<int>(got));
        return frame.size() == length;
    }

    void readFrames()
    {
        if (phase == Phase::Capability || phase == Phase::Pairing || phase == Phase::Trust) {
            if (reader && reader->bytesAvailable() > 0)
                stop(Status::InvalidOffer);
            return;
        }
        if ((phase != Phase::Challenge && phase != Phase::Offer) || timedOut())
            return;

        if (phase == Phase::Challenge) {
            if (!readTo(BridgeKeySize))
                return;
            outgoing = dialProof(key, frame);
            clearSecret(key);
            clearSecret(frame);
            if (outgoing.size() != BridgeKeySize) {
                stop(Status::TransportError);
                return;
            }
            outgoing += requestFrame();
            phase = Phase::Writing;
            sendBytes();
            return;
        }

        if (frame.isEmpty() && !readTo(1))
            return;
        if (frame.at(0) != 0 && !readTo(OfferSize))
            return;

        auto offer = parseOffer(frame);
        clearSecret(frame);
        if (offer.state == Offer::State::Rejected) {
            stop(Status::Rejected);
        } else if (offer.state != Offer::State::Ready || reader->bytesAvailable() > 0) {
            stop(Status::InvalidOffer);
        } else {
            stageAlarm.stop();
            stage = QDeadlineTimer(QDeadlineTimer::Forever);
            phase = Phase::Capability;
            pendingOffer = std::move(offer);
            const auto probe = Stream::makeProbeCommand(request.moonlightProgram, capabilityProbe(),
                                                         Stream::currentPlatform(), request.environment);
            if (!probe.command)
                stop(Status::RuntimeFailed);
            else
                launch(*probe.command);
        }
    }

    Stream::Probe capabilityProbe() const
    {
        return request.verifiedHost.viaEmbeddedTailnet ? Stream::Probe::GameStreamRelay : Stream::Probe::BackgroundPair;
    }

    void launch(const Stream::Request &command)
    {
        if (timedOut())
            return;
        const auto built = Stream::makeCommand(request.moonlightProgram, command, Stream::currentPlatform(),
                                                request.environment);
        if (!built.command) {
            stop(Status::RuntimeFailed);
            return;
        }

        launch(*built.command);
    }

    void launch(const Stream::Command &command)
    {
        if (timedOut())
            return;
        process = new Stream::MoonlightProcess(this);
        connect(process, &QProcess::readyReadStandardOutput, this, [this] {
            if (!collectOutput())
                stop(Status::RuntimeFailed);
        });
        connect(process, &QProcess::started, this, [this] {
            if (phase == Phase::Stopping)
                process->cancel();
        });
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                if (phase == Phase::Stopping)
                    finish();
                else
                    stop(Status::RuntimeFailed);
            }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) { processFinished(exitCode, exitStatus); });
        if (!process->launch(command))
            stop(Status::RuntimeFailed);
    }

    bool collectOutput()
    {
        if (!process)
            return true;
        const auto output = process->readAllStandardOutput();
        if (phase != Phase::Capability)
            return true;
        if (output.size() > MaxProbeOutput - probeOutput.size())
            return false;
        probeOutput += output;
        return true;
    }

    void processFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        const bool outputAccepted = collectOutput();
        process->deleteLater();
        process = nullptr;
        if (phase == Phase::Stopping) {
            finish();
            return;
        }
        if (timedOut())
            return;
        if (!outputAccepted || exitStatus != QProcess::NormalExit || exitCode != 0) {
            stop(phase == Phase::Trust ? Status::TrustFailed : Status::RuntimeFailed);
            return;
        }

        if (phase == Phase::Capability) {
            if (!Stream::probeSucceeded(capabilityProbe(), exitCode, probeOutput)) {
                stop(Status::RuntimeFailed);
                return;
            }
            phase = Phase::Pairing;
            auto command = moonlightRequest(request, Stream::Operation::BackgroundPair);
            command.pairingId = QString::fromLatin1(pendingOffer.marker);
            command.pin = QString::fromLatin1(pendingOffer.pin);
            clearSecret(pendingOffer.pin);
            launch(command);
            return;
        }

        // Exit zero from the bundled background worker means the store was synced.
        // Check the exact host entry and have Moonlight verify the pinned endpoint.
        if (phase == Phase::Pairing) {
            const QPointer<Attempt> alive(this);
            closeControl();
            if (!alive || phase == Phase::Stopping)
                return;
            const auto list = moonlightRequest(request, Stream::Operation::List);
            const auto built = Stream::makeCommand(request.moonlightProgram, list, Stream::currentPlatform(),
                                                    request.environment);
            if (!built.command) {
                stop(Status::TrustFailed);
                return;
            }
            pin = Detail::readPin(*built.command, request.verifiedHost.id);
            if (timedOut())
                return;
            if (pin.size() != 32) {
                stop(Status::TrustMissing);
                return;
            }
            phase = Phase::Trust;
            launch(list);
            return;
        }
        stop(Status::Paired);
    }

    void closeControl()
    {
        if (reader)
            disconnect(reader, nullptr, this, nullptr);
        if (writer && writer != reader)
            disconnect(writer, nullptr, this, nullptr);
        const auto control = socket;
        socket = nullptr;
        reader = nullptr;
        writer = nullptr;
        if (control)
            control->abort();
    }

    void stop(Status status)
    {
        if (phase == Phase::Stopping)
            return;
        phase = Phase::Stopping;
        terminal = status;
        overallAlarm.stop();
        stageAlarm.stop();
        const QPointer<Attempt> alive(this);
        closeControl();
        if (!alive)
            return;
        clearSecret(key);
        clearSecret(frame);
        clearSecret(outgoing);
        clearSecret(pendingOffer.pin);
        if (process && process->state() != QProcess::NotRunning)
            process->cancel();
        else
            finish();
    }

    void finish()
    {
        owner->complete(this, {terminal, terminal == Status::Paired ? pin : QByteArray{}});
    }

    const quint64 id;
    const quint64 revision;
    Coordinator *const owner;
    const Request request;
    QPointer<QIODevice> reader;
    QPointer<QIODevice> writer;
    QPointer<QTcpSocket> socket;
    QByteArray key;
    const Timeouts timeouts;
    const QDeadlineTimer overall;
    QDeadlineTimer stage{QDeadlineTimer::Forever};
    QTimer overallAlarm;
    QTimer stageAlarm;
    Phase phase;
    Status stageError = Status::BridgeTimeout;
    Status terminal = Status::Cancelled;
    QByteArray frame;
    QByteArray outgoing;
    qint64 sent = 0;
    QByteArray pin;
    QByteArray probeOutput;
    Offer pendingOffer;
    Stream::MoonlightProcess *process = nullptr;
};

Coordinator::Coordinator(QObject *parent, Timeouts timeouts)
    : QObject(parent), timeouts_{std::clamp(timeouts.bridgeMs, 0, BridgeTimeoutMs),
                                std::clamp(timeouts.offerMs, 0, OfferTimeoutMs),
                                std::clamp(timeouts.overallMs, 0, OverallTimeoutMs)}
{
    qRegisterMetaType<Result>();
}

Coordinator::~Coordinator()
{
    if (auto *attempt = std::exchange(active_, nullptr))
        delete attempt;
}

void Coordinator::setAuthorization(quint64 epoch, bool enabled)
{
    if (epoch < epoch_ || (epoch == epoch_ && enabled == authorized_))
        return;
    epoch_ = epoch;
    authorized_ = enabled;
    ++revision_;
    if (active_)
        active_->stop(Status::AuthorizationChanged);
}

bool Coordinator::busy() const
{
    return active_ != nullptr;
}

StartError Coordinator::validate(const Discovery::NetworkSnapshot &network, const Request &request, bool embedded) const
{
    if (active_)
        return StartError::Busy;
    if (!authorized_ || request.epoch != epoch_ || request.peerId.trimmed().isEmpty())
        return StartError::Unauthorized;
    const auto *peer = Discovery::findTailnetPeer(&network, request.verifiedHost.endpoint.address);
    if (!peer || peer->id != request.peerId)
        return StartError::Unauthorized;

    const auto &host = request.verifiedHost;
    const auto &route = request.leasedRoute;
    if (!request.leaseLifetime || request.leaseLifetime->thread() != thread() ||
        host.endpoint.port != Discovery::BundledSunshinePort || host.viaEmbeddedTailnet != embedded ||
        route.viaEmbeddedTailnet != embedded || host.id != route.hostId ||
        host.id.trimmed().isEmpty() || host.id.size() > 128)
        return StartError::InvalidRoute;
    if (embedded ? route.address != QHostAddress::LocalHost
                 : route.address.toString() != host.endpoint.address.toString() || route.port != host.endpoint.port)
        return StartError::InvalidRoute;

    auto command = moonlightRequest(request, Stream::Operation::BackgroundPair);
    command.pairingId = QString(32, QLatin1Char('0'));
    command.pin = QString(4, QLatin1Char('0'));
    const auto built = Stream::makeCommand(request.moonlightProgram, command, Stream::currentPlatform(), request.environment);
    if (!built.command)
        return built.error == Stream::CommandError::InvalidRelay ? StartError::InvalidRoute : StartError::InvalidRuntime;
    return StartError::None;
}

StartResult Coordinator::startLoopback(const Discovery::NetworkSnapshot &network, const Request &request,
                                       QTcpSocket *socket, quint16 bridgePort, QByteArray bridgeKey)
{
    const auto error = validate(network, request, true);
    if (error != StartError::None) {
        clearSecret(bridgeKey);
        return {0, error};
    }

    if (!socket || socket->thread() != thread() || socket->state() != QAbstractSocket::ConnectedState ||
        !socket->isReadable() || !socket->isWritable() || (socket->openMode() & QIODevice::Text) ||
        socket->peerAddress() != QHostAddress::LocalHost || socket->localAddress() != QHostAddress::LocalHost ||
        !bridgePort || socket->peerPort() != bridgePort || bridgeKey.size() != BridgeKeySize) {
        clearSecret(bridgeKey);
        return {0, StartError::InvalidTransport};
    }
    return start(request, socket, socket, socket, std::move(bridgeKey));
}

StartResult Coordinator::startStdio(const Discovery::NetworkSnapshot &network, const Request &request,
                                    QIODevice *reader, QIODevice *writer)
{
    const auto error = validate(network, request, false);
    if (error != StartError::None)
        return {0, error};
    if (!stdioDevice(reader, thread()) || !reader->isReadable() ||
        !stdioDevice(writer, thread()) || !writer->isWritable())
        return {0, StartError::InvalidTransport};
    return start(request, reader, writer, nullptr, {});
}

StartResult Coordinator::start(const Request &request, QIODevice *reader, QIODevice *writer,
                               QTcpSocket *socket, QByteArray bridgeKey)
{
    active_ = new Attempt(this, ++nextRequest_, revision_, request, reader, writer, socket, std::move(bridgeKey), timeouts_);
    return {active_->id, StartError::None};
}

void Coordinator::cancel(quint64 request)
{
    if (active_ && active_->id == request)
        active_->stop(Status::Cancelled);
}

void Coordinator::complete(Attempt *attempt, const Result &result)
{
    if (active_ != attempt)
        return;
    active_ = nullptr;
    QTimer::singleShot(0, this, [this, id = attempt->id, revision = attempt->revision, result] {
        if (revision != revision_)
            emit finished(id, {Status::AuthorizationChanged, {}});
        else
            emit finished(id, result);
    });
    attempt->deleteLater();
}

} // namespace Aurora::Pairing::Client
