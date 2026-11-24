// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SystemStatus.h"
#include "WindowsInstallation.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

namespace aurora::tailscale::system {

class SystemTailscaleClient final : public QObject
{
    Q_OBJECT

public:
    using Locate = std::function<Installation()>;

    explicit SystemTailscaleClient(QObject *parent = nullptr);
    SystemTailscaleClient(Locate locate, QObject *parent = nullptr);
    ~SystemTailscaleClient() override;

    // A rejected request leaves no queued work. Accepted requests echo the caller's generation.
    bool requestStatus(quint64 generation);
    bool requestProfiles(quint64 generation);

signals:
    void statusFinished(quint64 generation, const aurora::tailscale::StatusResult &result);
    void profilesFinished(quint64 generation, const aurora::tailscale::system::ProfilesResult &result);

private:
    enum class Operation { Status, Profiles };

    bool enqueue(Operation operation, quint64 generation);
    void begin();
    void startCommand();
    bool collect(QProcess::ProcessChannel channel);
    void commandFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void finish(StatusError error, std::optional<Status> status = {}, QVector<Profile> profiles = {});

    struct Request
    {
        Operation operation;
        quint64 generation;
        QString executable;
        bool legacy = false;
    };

    const Locate locate;
    QProcess process;
    QTimer deadline;
    QElapsedTimer elapsed;
    QByteArray standardOutput;
    QByteArray standardError;
    std::optional<Request> request;
    bool stopping = false;
};

} // namespace aurora::tailscale::system
