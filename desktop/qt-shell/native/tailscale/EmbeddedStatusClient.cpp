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
constexpr int statusTimeoutMs = 12000;
}

EmbeddedStatusClient::EmbeddedStatusClient(QString executable, QString stateDirectory, QObject *parent)
    : QObject(parent), executable(std::move(executable)), stateDirectory(QDir::cleanPath(stateDirectory))
{
    qRegisterMetaType<StatusResult>();
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    connect(&deadline, &QTimer::timeout, this, [this] { finish({StatusError::Timeout, {}}, true); });
    connect(&process, &QProcess::started, this, &EmbeddedStatusClient::sendRequest);
    connect(&process, &QProcess::readyReadStandardOutput, this, &EmbeddedStatusClient::receive);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({StatusError::StartFailed, {}});
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            finish({StatusError::Transport, {}}, true);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this] {
        stopping = false;
        finish({StatusError::Transport, {}});
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
    if (request || stopping)
        return false;
    request = Request{generation, QString::number(++serial)};
    elapsed.start();
    deadline.start(statusTimeoutMs);
    QTimer::singleShot(0, this, &EmbeddedStatusClient::begin);
    return true;
}

void EmbeddedStatusClient::begin()
{
    if (!request)
        return;
    if (!isAvailable())
        return finish({StatusError::Unavailable, {}}, true);
    if (!QDir::isAbsolutePath(stateDirectory) || QDir(stateDirectory).isRoot())
        return finish({StatusError::InvalidStateDirectory, {}});
    if (process.state() == QProcess::Running)
        sendRequest();
    else
        process.start(executable, {QStringLiteral("--state-dir"), stateDirectory});
}

void EmbeddedStatusClient::sendRequest()
{
    if (!request)
        return;
    if (elapsed.hasExpired(statusTimeoutMs))
        return finish({StatusError::Timeout, {}}, true);
    const QJsonObject frame{
        {QStringLiteral("id"), request->id},
        {QStringLiteral("method"), QStringLiteral("status")},
        {QStringLiteral("params"), QJsonObject{}}
    };
    const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
    request->sent = true;
    if (process.write(bytes) != bytes.size())
        finish({StatusError::Transport, {}}, true);
}

void EmbeddedStatusClient::receive()
{
    if (!request)
        return stop();
    if (!request->sent)
        return finish({StatusError::InvalidResponse, {}}, true);
    if (elapsed.hasExpired(statusTimeoutMs))
        return finish({StatusError::Timeout, {}}, true);

    output += process.read(maxStatusReplyBytes + 1 - output.size());
    const auto newline = output.indexOf('\n');
    if (newline < 0) {
        if (output.size() > maxStatusReplyBytes)
            finish({StatusError::ResponseTooLarge, {}}, true);
        return;
    }
    if (newline + 1 != output.size() || process.bytesAvailable() != 0)
        return finish({StatusError::InvalidResponse, {}}, true);

    const auto result = decodeStatusReply(output.left(newline), request->id);
    if (elapsed.hasExpired(statusTimeoutMs))
        return finish({StatusError::Timeout, {}}, true);
    finish(result, result.error == StatusError::InvalidResponse);
}

void EmbeddedStatusClient::finish(StatusResult result, bool stopHelper)
{
    if (!request)
        return;
    const auto generation = request->generation;
    request.reset();
    deadline.stop();
    output.clear();
    if (stopHelper)
        stop();
    emit statusFinished(generation, result);
}

void EmbeddedStatusClient::stop()
{
    if (!stopping && process.state() != QProcess::NotRunning) {
        stopping = true;
        process.kill();
    }
}

} // namespace aurora::tailscale
