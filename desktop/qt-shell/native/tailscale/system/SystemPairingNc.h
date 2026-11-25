// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SystemStatus.h"
#include "WindowsInstallation.h"

#include <QDeadlineTimer>
#include <QProcess>
#include <QTimer>

class QIODevice;

namespace aurora::tailscale::system {

inline constexpr quint16 PairingPort = 43826;
inline constexpr int PairingNcTimeoutMs = 38000;

struct PairingNcTarget
{
    quint64 generation;
    AccountIdentity account;
    QString peerId;
    QHostAddress address;
};

enum class PairingNcError {
    None, AuthorizationChanged, InstallationUnavailable, StatusFailed, IdentityMismatch,
    StartFailed, Transport, LimitExceeded, Timeout, Cancelled
};

struct PairingNcResult
{
    PairingNcError error;
    std::optional<int> exitCode;
};

// Use on the owning event-loop thread. Owns one status preflight and one relay in sequence.
class SystemPairingNc final : public QObject
{
    Q_OBJECT

public:
    using Locate = std::function<Installation()>;

    explicit SystemPairingNc(QObject *parent = nullptr);
    SystemPairingNc(Locate locate, QObject *parent = nullptr);
    ~SystemPairingNc() override;

    // Pass a verified system-provider account only. Pass null on provider transitions or sign-out.
    void setAuthorization(quint64 generation, std::optional<AccountIdentity> account);
    // Zero means rejected or busy. An accepted ID completes asynchronously.
    quint64 start(const PairingNcTarget &target, int timeoutMs = PairingNcTimeoutMs);
    void cancel(quint64 request);

signals:
    // The borrowed sequential QIODevice is binary stdout/stdin. It is owned by the adapter.
    // On natural EOF it retains unread bytes until the next accepted request.
    void ready(quint64 request, QIODevice *channel);
    void finished(quint64 request, const aurora::tailscale::system::PairingNcResult &result);

private:
    class Channel;
    enum class Phase { Scheduled, Status, Verified, Relay };
    struct Request
    {
        quint64 id;
        quint64 revision;
        PairingNcTarget target;
        QDeadlineTimer deadline;
        Phase phase = Phase::Scheduled;
        QString program;
        QByteArray statusOutput;
        qint64 stdoutBytes = 0;
        qint64 stderrBytes = 0;
        qint64 stdinBytes = 0;
        std::optional<PairingNcError> stopping;
    };

    void preflight();
    void launchRelay();
    void launch(const QStringList &arguments);
    bool collectStdout();
    bool collectStderr();
    qint64 writeRelay(quint64 request, const char *data, qint64 size);
    void childFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void stop(PairingNcError error);
    void complete(PairingNcError error, std::optional<int> exitCode = {}, bool preserveOutput = false);

    const Locate locate_;
    QProcess process_;
    QTimer alarm_;
    Channel *channel_ = nullptr;
    std::optional<Request> request_;
    std::optional<AccountIdentity> account_;
    quint64 generation_ = 0;
    quint64 revision_ = 0;
    quint64 nextRequest_ = 0;
};

} // namespace aurora::tailscale::system

Q_DECLARE_METATYPE(aurora::tailscale::system::PairingNcResult)
