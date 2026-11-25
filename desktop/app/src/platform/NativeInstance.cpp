// SPDX-License-Identifier: GPL-3.0-only
#include "NativeInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr char activation = '\1';
constexpr int activationTimeout = 3000;

QString instanceName(const QString &dataDirectory)
{
    QByteArray scope = QDir::cleanPath(dataDirectory).toUtf8();
#ifdef Q_OS_WIN
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    scope = QString::fromUtf8(scope).toCaseFolded().toUtf8() + ':' + QByteArray::number(unsigned(sessionId));
#endif
    const auto digest = QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex().left(20);
    return QStringLiteral("aurora-native-") + QString::fromLatin1(digest);
}
}

NativeInstance::NativeInstance(const QString &dataDirectory)
    : directory(dataDirectory), name(instanceName(dataDirectory)),
      lock(QDir(directory).filePath(name + QStringLiteral(".lock")))
{
    lock.setStaleLockTime(0);
    server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server, &QLocalServer::newConnection, this, &NativeInstance::acceptConnections);
}

NativeInstance::Result NativeInstance::start()
{
    if (directory.isEmpty() || !QDir().mkpath(directory))
        return Result::Error;
    if (lock.tryLock(0))
        return listen();
    if (lock.error() != QLockFile::LockFailedError)
        return Result::Error;
    if (activatePrimary())
        return Result::Activated;
    // The first process may have exited while activation was being attempted.
    return lock.tryLock(0) ? listen() : Result::Error;
}

NativeInstance::Result NativeInstance::listen()
{
    // Only the lock owner can clean up the socket left by an interrupted process.
    QLocalServer::removeServer(name);
    if (server.listen(name))
        return Result::Primary;
    lock.unlock();
    return Result::Error;
}

bool NativeInstance::activatePrimary()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < activationTimeout) {
        QLocalSocket socket;
        socket.connectToServer(name);
        if (socket.waitForConnected(qMax(1, activationTimeout - int(timer.elapsed())))) {
#ifdef Q_OS_WIN
            ULONG processId = 0;
            if (GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()), &processId))
                AllowSetForegroundWindow(processId);
#endif
            if (socket.write(&activation, 1) != 1)
                return false;
            socket.flush();
            if (!socket.bytesAvailable())
                socket.waitForReadyRead(qMax(1, activationTimeout - int(timer.elapsed())));
            return socket.read(1) == QByteArray(1, activation);
        }
        // A concurrent primary can acquire the lock before it binds the socket.
        QThread::msleep(35);
    }
    return false;
}

void NativeInstance::acceptConnections()
{
    while (auto *socket = server.nextPendingConnection()) {
        socket->setReadBufferSize(1);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
#ifdef Q_OS_MACOS
        // Darwin does not enforce the access mode of an AF_UNIX socket file.
        uid_t peer = 0;
        gid_t group = 0;
        if (getpeereid(int(socket->socketDescriptor()), &peer, &group) != 0 || peer != geteuid()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
#endif
        const auto read = [this, socket] {
            if (socket->read(1) == QByteArray(1, activation)) {
                emit activationRequested();
                socket->write(&activation, 1);
            }
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, socket, read);
        QTimer::singleShot(1000, socket, [socket] { socket->abort(); });
        if (socket->bytesAvailable())
            read();
    }
}
