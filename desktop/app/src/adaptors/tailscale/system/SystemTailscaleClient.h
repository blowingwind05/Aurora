// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SystemStatus.h"
#include "WindowsInstallation.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace aurora::tailscale::system {

enum class Action { Login, Resume, Logout, SwitchProfile };
enum class ActionOutcome { Failed, Completed, ReadStatus, AlreadyCurrent, Revalidated };

struct ActionResult
{
    Action action = Action::Login;
    StatusError error = StatusError::InvalidResponse;
    ActionOutcome outcome = ActionOutcome::Failed;
    std::optional<int> exitCode;
    QVector<Profile> verifiedProfiles;
};

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
    bool requestLogin(quint64 generation);
    bool requestResume(quint64 generation);
    bool requestLogout(quint64 generation);
    // The ID must have been returned by the latest profile listing for this generation.
    bool requestProfileSwitch(const QString &id, quint64 generation);

signals:
    void statusFinished(quint64 generation, const aurora::tailscale::StatusResult &result);
    void profilesFinished(quint64 generation, const aurora::tailscale::system::ProfilesResult &result);
    void actionFinished(quint64 generation, const aurora::tailscale::system::ActionResult &result);

private:
    enum class Phase { Status, ListProfiles, CheckProfile, Up, Logout, SwitchProfile, VerifyProfile };

    struct Request
    {
        Phase phase;
        quint64 generation;
        std::optional<Action> action;
        QString executable;
        QString profileId;
        bool legacy = false;
        StatusError switchError = StatusError::None;
        std::optional<int> exitCode;
    };

    struct ListedProfiles
    {
        quint64 generation;
        QString executable;
        QSet<QString> ids;
    };

    bool enqueue(Phase phase, quint64 generation, std::optional<Action> action = {}, const QString &id = {});
    void begin();
    void resetDeadline(Phase phase);
    void startCommand();
    bool collect(QProcess::ProcessChannel channel);
    void commandFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void profilesRead(ProfilesResult result);
    void verifyProfile(StatusError switchError);
    bool sameRunningInstallation() const;
    void fail(StatusError error);
    void finish(StatusError error, std::optional<Status> status = {}, QVector<Profile> profiles = {},
        ActionOutcome outcome = ActionOutcome::Failed);

    const Locate locate;
    QProcess process;
    QTimer deadline;
    QTimer dispatch;
    QElapsedTimer elapsed;
    int deadlineMs = 0;
    QByteArray standardOutput;
    QByteArray standardError;
    std::optional<Request> request;
    std::optional<ListedProfiles> listedProfiles;
    bool stopping = false;
};

} // namespace aurora::tailscale::system

Q_DECLARE_METATYPE(aurora::tailscale::system::ActionResult)
