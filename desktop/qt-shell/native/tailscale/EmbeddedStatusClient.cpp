// SPDX-License-Identifier: GPL-3.0-only
#include "EmbeddedStatusClient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace aurora::tailscale {
namespace {
constexpr int requestTimeoutMs = 12000;
}

EmbeddedStatusClient::EmbeddedStatusClient(QString executable, QString stateDirectory, QObject *parent)
    : QObject(parent), executable(std::move(executable)), stateDirectory(QDir::cleanPath(stateDirectory))
{
    qRegisterMetaType<StatusError>();
    qRegisterMetaType<StatusResult>();
    qRegisterMetaType<LoginResult>();
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    connect(&deadline, &QTimer::timeout, this, [this] { finish(StatusError::Timeout); });
    connect(&process, &QProcess::started, this, &EmbeddedStatusClient::sendRequest);
    connect(&process, &QProcess::readyReadStandardOutput, this, &EmbeddedStatusClient::receive);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(StatusError::StartFailed);
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            finish(StatusError::Transport);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this] {
        stopping = false;
        finish(StatusError::Transport);
    });
}

EmbeddedStatusClient::~EmbeddedStatusClient()
{
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.closeWriteChannel();
        if (!process.waitForFinished(200))
            process.kill();
    }
}

QString EmbeddedStatusClient::bundledExecutable(const QString &applicationDirectory)
{
    if (!QDir::isAbsolutePath(applicationDirectory))
        return {};
    const QDir directory(applicationDirectory);
#ifdef Q_OS_WIN
    return directory.filePath(QStringLiteral("Components/Tailscale/AuroraTailnet.exe"));
#elif defined(Q_OS_MACOS)
    return directory.filePath(QStringLiteral("AuroraTailnet"));
#else
    return {};
#endif
}

bool EmbeddedStatusClient::isAvailable() const
{
    const QFileInfo file(executable);
    return file.isAbsolute() && file.isFile() && file.isExecutable();
}

bool EmbeddedStatusClient::requestStatus(quint64 generation)
{
    return startRequest(Operation::Status, generation);
}

bool EmbeddedStatusClient::requestLogin(quint64 generation)
{
    return startRequest(Operation::Login, generation);
}

bool EmbeddedStatusClient::requestLogout(quint64 generation)
{
    return startRequest(Operation::Logout, generation);
}

bool EmbeddedStatusClient::startRequest(Operation operation, quint64 generation)
{
    if (request || stopping)
        return false;
    request = Request{operation, generation, QString::number(++serial)};
    elapsed.start();
    deadline.start(requestTimeoutMs);
    QTimer::singleShot(0, this, &EmbeddedStatusClient::begin);
    return true;
}

void EmbeddedStatusClient::begin()
{
    if (!request)
        return;
    if (!isAvailable())
        return finish(StatusError::Unavailable);
    if (!QDir::isAbsolutePath(stateDirectory) || QDir(stateDirectory).isRoot())
        return finish(StatusError::InvalidStateDirectory);
    if (process.state() == QProcess::Running)
        sendRequest();
    else
        process.start(executable, {QStringLiteral("--state-dir"), stateDirectory});
}

void EmbeddedStatusClient::sendRequest()
{
    if (!request)
        return;
    if (elapsed.hasExpired(requestTimeoutMs))
        return finish(StatusError::Timeout);
    QString method;
    switch (request->operation) {
    case Operation::Status: method = QStringLiteral("status"); break;
    case Operation::Login: method = QStringLiteral("login"); break;
    case Operation::Logout: method = QStringLiteral("logout"); break;
    }
    const QJsonObject frame{
        {QStringLiteral("id"), request->id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), QJsonObject{}}
    };
    const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
    request->sent = true;
    if (process.write(bytes) != bytes.size())
        finish(StatusError::Transport);
}

void EmbeddedStatusClient::receive()
{
    if (!request)
        return stop();
    if (!request->sent)
        return finish(StatusError::InvalidResponse);
    if (elapsed.hasExpired(requestTimeoutMs))
        return finish(StatusError::Timeout);

    const int limit = request->operation == Operation::Status ? maxStatusReplyBytes : maxActionReplyBytes;
    output += process.read(limit + 1 - output.size());
    const auto newline = output.indexOf('\n');
    if (newline < 0) {
        if (output.size() > limit)
            finish(StatusError::ResponseTooLarge);
        return;
    }
    if (newline + 1 != output.size() || process.bytesAvailable() != 0)
        return finish(StatusError::InvalidResponse);

    const auto line = output.left(newline);
    switch (request->operation) {
    case Operation::Status: {
        auto result = decodeStatusReply(line, request->id);
        return finish(result.error, std::move(result.status));
    }
    case Operation::Login: {
        const auto result = decodeLoginReply(line, request->id);
        return finish(result.error, {}, result.pending);
    }
    case Operation::Logout:
        return finish(decodeLogoutReply(line, request->id));
    }
}

void EmbeddedStatusClient::finish(StatusError error, std::optional<Status> status, std::optional<bool> loginPending)
{
    if (!request)
        return;
    if (elapsed.hasExpired(requestTimeoutMs)) {
        error = StatusError::Timeout;
        status.reset();
        loginPending.reset();
    }

    const auto completed = std::exchange(request, std::nullopt);
    deadline.stop();
    output.clear();
    if (error == StatusError::Unavailable || error == StatusError::Transport || error == StatusError::Timeout ||
            error == StatusError::ResponseTooLarge || error == StatusError::InvalidResponse)
        stop();

    switch (completed->operation) {
    case Operation::Status:
        emit statusFinished(completed->generation, {error, std::move(status)});
        break;
    case Operation::Login:
        emit loginFinished(completed->generation, {error, loginPending});
        break;
    case Operation::Logout:
        emit logoutFinished(completed->generation, error);
        break;
    }
}

void EmbeddedStatusClient::stop()
{
    if (!stopping && process.state() != QProcess::NotRunning) {
        stopping = true;
        process.kill();
    }
}

} // namespace aurora::tailscale
