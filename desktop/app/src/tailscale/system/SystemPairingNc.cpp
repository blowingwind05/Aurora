// SPDX-License-Identifier: GPL-3.0-only
#include "SystemPairingNc.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <qt_windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace aurora::tailscale::system {
namespace {

constexpr int statusTimeoutMs = 8000;
constexpr qint64 maxRelayBytes = 64 * 1024;
const auto systemSocket = QStringLiteral(R"(--socket=\\.\pipe\protectedprefix\administrators\tailscale\tailscaled)");

bool validIdentity(const AccountIdentity &account)
{
    return account.ownerId > 0 && !account.nodeId.trimmed().isEmpty() && !account.tailnetId.trimmed().isEmpty();
}

QHostAddress normalized(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const QHostAddress ipv4(address.toIPv4Address());
        if (address.isEqual(ipv4, QHostAddress::ConvertV4MappedToIPv4))
            return ipv4;
    }
    return address;
}

bool usableAddress(const QHostAddress &address)
{
    return (address.protocol() == QAbstractSocket::IPv4Protocol ||
            address.protocol() == QAbstractSocket::IPv6Protocol) &&
        address.scopeId().isEmpty() && !address.isNull() && !address.isLoopback() && !address.isMulticast() &&
        address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6 && address != QHostAddress::Broadcast;
}

bool matchesPeer(const Status &status, const PairingNcTarget &target)
{
    if (status.state != BackendState::Connected || !status.account || !(*status.account == target.account) ||
        !usableAddress(status.localAddress) || target.peerId == status.account->nodeId)
        return false;
    const auto wanted = target.address.toString();
    if (wanted == normalized(status.localAddress).toString())
        return false;

    const Peer *match = nullptr;
    for (const auto &peer : status.peers) {
        if (normalized(peer.address).toString() != wanted &&
            normalized(peer.alternateAddress).toString() != wanted)
            continue;
        if (match)
            return false;
        match = &peer;
    }
    return match && match->nodeId == target.peerId;
}

bool localAbsolutePath(const QString &path)
{
    const auto normalizedPath = QDir::fromNativeSeparators(path);
    if (normalizedPath.size() < 3 || normalizedPath.at(1) != QLatin1Char(':') ||
        normalizedPath.at(2) != QLatin1Char('/') || normalizedPath.contains(QChar::Null))
        return false;
    const auto letter = normalizedPath.at(0).unicode();
    return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
}

QString runningProgram(const Installation &installation)
{
    if (installation.service != ServiceState::Running || !localAbsolutePath(installation.executable))
        return {};
    const QFileInfo file(installation.executable);
    const auto canonical = file.canonicalFilePath();
    return localAbsolutePath(canonical) && file.isFile() ? canonical : QString{};
}

std::optional<QProcessEnvironment> privateEnvironment(const QString &program)
{
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const auto rootSize = GetSystemWindowsDirectoryW(buffer.data(), UINT(buffer.size()));
    if (!rootSize || rootSize >= buffer.size())
        return std::nullopt;
    const auto root = QString::fromWCharArray(buffer.data(), int(rootSize));
    const auto systemSize = GetSystemDirectoryW(buffer.data(), UINT(buffer.size()));
    if (!systemSize || systemSize >= buffer.size())
        return std::nullopt;
    const auto system = QString::fromWCharArray(buffer.data(), int(systemSize));
    if (!localAbsolutePath(root) || !localAbsolutePath(system))
        return std::nullopt;

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("SystemRoot"), root);
    environment.insert(QStringLiteral("WINDIR"), root);
    environment.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(QFileInfo(program).absolutePath()) +
        QLatin1Char(';') + QDir::toNativeSeparators(system));
    return environment;
}

} // namespace

class SystemPairingNc::Channel final : public QIODevice
{
public:
    Channel(SystemPairingNc *owner, quint64 request) : QIODevice(owner), owner_(owner), request_(request)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return pending_.size() + QIODevice::bytesAvailable(); }

    void append(const QByteArray &bytes)
    {
        if (!bytes.isEmpty()) {
            pending_ += bytes;
            emit readyRead();
        }
    }

    void eof()
    {
        setOpenMode(QIODevice::ReadOnly | QIODevice::Unbuffered);
        emit readChannelFinished();
    }

    void shutdown()
    {
        pending_.clear();
        if (isOpen())
            QIODevice::close();
    }

    void close() override
    {
        shutdown();
        owner_->cancel(request_);
    }

protected:
    qint64 readData(char *data, qint64 size) override
    {
        const auto count = std::min(size, static_cast<qint64>(pending_.size()));
        std::memcpy(data, pending_.constData(), static_cast<size_t>(count));
        pending_.remove(0, static_cast<int>(count));
        return count;
    }

    qint64 writeData(const char *data, qint64 size) override
    {
        return owner_->writeRelay(request_, data, size);
    }

private:
    SystemPairingNc *const owner_;
    const quint64 request_;
    QByteArray pending_;
};

SystemPairingNc::SystemPairingNc(QObject *parent) : SystemPairingNc(inspectWindowsInstallation, parent)
{
}

SystemPairingNc::SystemPairingNc(Locate locate, QObject *parent)
    : QObject(parent), locate_(std::move(locate)), process_(this), alarm_(this)
{
    qRegisterMetaType<PairingNcResult>();
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.setInputChannelMode(QProcess::ManagedInputChannel);
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    alarm_.setSingleShot(true);
    alarm_.setTimerType(Qt::PreciseTimer);
    connect(&alarm_, &QTimer::timeout, this, [this] { stop(PairingNcError::Timeout); });
    connect(&process_, &QProcess::started, this, [this] {
        if (!request_ || request_->stopping) {
            process_.kill();
        } else if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
            stop(PairingNcError::Timeout);
        } else if (request_->phase == Phase::Status) {
            process_.closeWriteChannel();
        } else if (request_->phase == Phase::Relay) {
            channel_ = new Channel(this, request_->id);
            emit ready(request_->id, channel_);
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        if (!request_ || request_->stopping)
            return;
        if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0)
            stop(PairingNcError::Timeout);
        else if (!collectStdout())
            stop(PairingNcError::LimitExceeded);
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        if (request_ && !request_->stopping && !collectStderr())
            stop(PairingNcError::LimitExceeded);
    });
    connect(&process_, &QProcess::bytesWritten, this, [this](qint64 count) {
        if (request_ && !request_->stopping && request_->phase == Phase::Relay && channel_)
            emit channel_->bytesWritten(count);
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SystemPairingNc::childFinished);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!request_)
            return;
        if (error == QProcess::FailedToStart)
            complete(request_->stopping.value_or(PairingNcError::StartFailed));
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            stop(PairingNcError::Transport);
    });
}

SystemPairingNc::~SystemPairingNc()
{
    alarm_.stop();
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(200);
    }
}

void SystemPairingNc::setAuthorization(quint64 generation, std::optional<AccountIdentity> account)
{
    if (generation < generation_)
        return;
    if (account && !validIdentity(*account))
        account.reset();
    if (generation == generation_ && account == account_)
        return;

    generation_ = generation;
    account_ = std::move(account);
    ++revision_;
    if (request_)
        stop(PairingNcError::AuthorizationChanged);
    else if (channel_)
        channel_->shutdown();
}

quint64 SystemPairingNc::start(const PairingNcTarget &target, int timeoutMs)
{
    const auto address = normalized(target.address);
    if (request_ || !account_ || target.generation != generation_ || !(target.account == *account_) ||
        target.peerId.trimmed().isEmpty() || target.peerId == account_->nodeId ||
        !target.address.scopeId().isEmpty() || !usableAddress(address))
        return 0;

    if (auto *previous = std::exchange(channel_, nullptr)) {
        // Retire the previous EOF channel before exposing the next one.
        previous->deleteLater();
    }
    const auto id = ++nextRequest_;
    const int timeout = std::clamp(timeoutMs, 0, PairingNcTimeoutMs);
    request_.emplace(Request{id, revision_, {target.generation, target.account, target.peerId, address},
                             QDeadlineTimer(timeout, Qt::PreciseTimer)});
    alarm_.start(std::min(timeout, statusTimeoutMs));
    QTimer::singleShot(0, this, [this, id] {
        if (request_ && request_->id == id)
            preflight();
    });
    return id;
}

void SystemPairingNc::preflight()
{
    if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
        stop(PairingNcError::Timeout);
        return;
    }
    const auto program = runningProgram(locate_());
    if (program.isEmpty()) {
        stop(PairingNcError::InstallationUnavailable);
        return;
    }
    request_->program = program;
    request_->phase = Phase::Status;
    launch({systemSocket, QStringLiteral("status"), QStringLiteral("--json")});
}

void SystemPairingNc::launchRelay()
{
    if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
        stop(PairingNcError::Timeout);
        return;
    }
    const auto program = runningProgram(locate_());
    if (program.isEmpty() || program.compare(request_->program, Qt::CaseInsensitive) != 0) {
        stop(PairingNcError::InstallationUnavailable);
        return;
    }

    if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
        stop(PairingNcError::Timeout);
        return;
    }
    request_->phase = Phase::Relay;
    request_->statusOutput.clear();
    request_->stdoutBytes = request_->stderrBytes = 0;
    alarm_.start(static_cast<int>(request_->deadline.remainingTime()));
    launch({systemSocket, QStringLiteral("nc"), request_->target.address.toString(), QString::number(PairingPort)});
}

void SystemPairingNc::launch(const QStringList &arguments)
{
    if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
        stop(PairingNcError::Timeout);
        return;
    }
    const auto environment = privateEnvironment(request_->program);
    if (!environment) {
        stop(PairingNcError::InstallationUnavailable);
        return;
    }
    process_.setWorkingDirectory(QFileInfo(request_->program).absolutePath());
    process_.setProcessEnvironment(*environment);
    if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
        stop(PairingNcError::Timeout);
        return;
    }
    process_.start(request_->program, arguments);
}

bool SystemPairingNc::collectStdout()
{
    const auto limit = request_->phase == Phase::Status ? maxSystemStatusBytes : maxRelayBytes;
    process_.setReadChannel(QProcess::StandardOutput);
    const auto bytes = process_.read(limit + 1 - request_->stdoutBytes);
    request_->stdoutBytes += bytes.size();
    if (request_->stdoutBytes > limit)
        return false;
    if (request_->phase == Phase::Status)
        request_->statusOutput += bytes;
    else if (channel_ && channel_->isOpen())
        channel_->append(bytes);
    return true;
}

bool SystemPairingNc::collectStderr()
{
    process_.setReadChannel(QProcess::StandardError);
    request_->stderrBytes += process_.read(maxRelayBytes + 1 - request_->stderrBytes).size();
    return request_->stderrBytes <= maxRelayBytes;
}

qint64 SystemPairingNc::writeRelay(quint64 request, const char *data, qint64 size)
{
    if (!request_ || request_->id != request || request_->stopping || request_->phase != Phase::Relay ||
        process_.state() != QProcess::Running)
        return -1;
    if (request_->deadline.hasExpired()) {
        stop(PairingNcError::Timeout);
        return -1;
    }
    if (size > maxRelayBytes - request_->stdinBytes) {
        stop(PairingNcError::LimitExceeded);
        return -1;
    }
    const auto written = process_.write(data, size);
    if (written < 0)
        stop(PairingNcError::Transport);
    else if (request_ && request_->id == request && !request_->stopping)
        request_->stdinBytes += written;
    return written;
}

void SystemPairingNc::childFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!request_)
        return;
    if (request_->stopping) {
        complete(*request_->stopping);
        return;
    }
    if (request_->deadline.hasExpired() || alarm_.remainingTime() == 0) {
        complete(PairingNcError::Timeout);
        return;
    }

    const auto id = request_->id;
    const auto valid = collectStdout();
    if (!request_ || request_->id != id)
        return;
    if (!valid || !collectStderr()) {
        complete(PairingNcError::LimitExceeded);
        return;
    }
    if (request_->phase == Phase::Relay) {
        if (exitStatus == QProcess::NormalExit)
            complete(exitCode == 0 ? PairingNcError::None : PairingNcError::Transport, exitCode, true);
        else
            complete(PairingNcError::Transport);
        return;
    }

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        complete(PairingNcError::StatusFailed);
        return;
    }
    const auto decoded = decodeSystemStatus(request_->statusOutput);
    if (decoded.error != StatusError::None || !decoded.status) {
        complete(PairingNcError::StatusFailed);
        return;
    }
    if (!matchesPeer(*decoded.status, request_->target)) {
        complete(PairingNcError::IdentityMismatch);
        return;
    }
    request_->phase = Phase::Verified;
    QTimer::singleShot(0, this, [this, id] {
        if (request_ && request_->id == id)
            launchRelay();
    });
}

void SystemPairingNc::cancel(quint64 request)
{
    if (request_ && request_->id == request)
        stop(PairingNcError::Cancelled);
}

void SystemPairingNc::stop(PairingNcError error)
{
    if (!request_ || request_->stopping)
        return;
    request_->stopping = error;
    alarm_.stop();
    if (channel_)
        channel_->shutdown();
    if (process_.state() != QProcess::NotRunning)
        process_.kill();
    else
        complete(error);
}

void SystemPairingNc::complete(PairingNcError error, std::optional<int> exitCode, bool preserveOutput)
{
    if (!request_)
        return;
    const auto id = request_->id;
    const auto revision = request_->revision;
    request_.reset();
    alarm_.stop();
    if (channel_) {
        if (preserveOutput)
            channel_->eof();
        else
            channel_->shutdown();
    }
    QTimer::singleShot(0, this, [this, id, revision, result = PairingNcResult{error, exitCode}] {
        if (revision != revision_)
            emit finished(id, {PairingNcError::AuthorizationChanged, {}});
        else
            emit finished(id, result);
    });
}

} // namespace aurora::tailscale::system
