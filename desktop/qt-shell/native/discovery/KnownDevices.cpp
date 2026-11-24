// SPDX-License-Identifier: GPL-3.0-only
#include "KnownDevices.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#else
#include <unistd.h>
#endif

namespace Aurora::Discovery {
namespace {
QString digest(const QString &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex().toUpper());
}

QString accountDirectory(const QString &dataDirectory, const NetworkSnapshot &network)
{
    if (!QDir::isAbsolutePath(dataDirectory) || !network.isAuthenticated())
        return {};
    const auto account = digest(QString::number(network.owner) + QLatin1Char('/') + network.localId);
    return QDir(dataDirectory).filePath(QStringLiteral("KnownDevices/") + account);
}

QUuid readIdentity(const QString &path)
{
    const QFileInfo entry(path);
    if (!entry.isFile() || entry.isSymLink())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto text = file.read(33);
    if (text.size() != 32 || file.error() != QFileDevice::NoError)
        return {};
    const auto bytes = QByteArray::fromHex(text);
    return bytes.toHex() == text.toLower() ? QUuid::fromRfc4122(bytes) : QUuid{};
}

bool makePrivateDirectory(const QString &path)
{
    const QFileInfo entry(path);
    if (entry.isSymLink())
        return false;
    if (entry.isDir())
        return true;
    if (!QDir().mkpath(path))
        return false;
#ifdef Q_OS_UNIX
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#else
    return true;
#endif
}

void replaceIdentity(const QString &path, const QUuid &identity)
{
    if (QFileInfo(path).isSymLink())
        return;
    QSaveFile file(path);
    const auto value = identity.toRfc4122().toHex();
    if (file.open(QIODevice::WriteOnly) && file.write(value) == value.size())
        file.commit();
}

bool installIdentity(const QString &path, const QUuid &identity)
{
    QString temporaryPath;
    {
        QTemporaryFile temporary(path + QStringLiteral(".XXXXXX"));
        const auto value = identity.toRfc4122().toHex();
        if (!temporary.open() || temporary.write(value) != value.size() || !temporary.flush())
            return false;
        temporaryPath = temporary.fileName();
        temporary.setAutoRemove(false);
    }
#ifdef Q_OS_WIN
    const auto from = QDir::toNativeSeparators(temporaryPath);
    const auto to = QDir::toNativeSeparators(path);
    const bool installed = MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                                     MOVEFILE_WRITE_THROUGH) != 0;
#else
    // A hard link publishes all 32 bytes atomically and loses to a concurrent creator.
    const auto from = QFile::encodeName(temporaryPath);
    const auto to = QFile::encodeName(path);
    const bool installed = ::link(from.constData(), to.constData()) == 0;
#endif
    QFile::remove(temporaryPath);
    return installed;
}
}

KnownIdentities readKnownDevices(const QString &dataDirectory, const NetworkSnapshot &network)
{
    KnownIdentities known;
    const auto path = accountDirectory(dataDirectory, network);
    if (path.isEmpty())
        return known;
    const QDir directory(path);
    for (const auto &peer : network.devices) {
        const auto id = readIdentity(directory.filePath(digest(peer.id)));
        if (!id.isNull())
            known.insert(peer.id, id);
    }
    return known;
}

KnownIdentities rememberKnownDevices(const QString &dataDirectory, const NetworkSnapshot &network,
                                    const QList<PresentDevice> &live)
{
    if (!network.isAuthenticated())
        return {};
    const auto saved = readKnownDevices(dataDirectory, network);
    const auto known = resolveKnownIdentities(network.devices, live, saved);
    const auto path = accountDirectory(dataDirectory, network);
    if (path.isEmpty() || known == saved || !makePrivateDirectory(QFileInfo(path).absolutePath()) ||
        !makePrivateDirectory(path))
        return known;

    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        if (it.value() != saved.value(it.key()))
            replaceIdentity(QDir(path).filePath(digest(it.key())), it.value());
    }
    return known;
}

QUuid readOrCreatePresenceIdentity(const QString &dataDirectory)
{
    const auto fresh = QUuid::createUuid();
    if (!QDir::isAbsolutePath(dataDirectory) || !makePrivateDirectory(dataDirectory))
        return fresh;
    const auto path = QDir(dataDirectory).filePath(QStringLiteral("presence-id"));
    const auto existing = readIdentity(path);
    if (!existing.isNull())
        return existing;
    if (installIdentity(path, fresh))
        return fresh;
    const auto concurrent = readIdentity(path);
    return concurrent.isNull() ? fresh : concurrent;
}

}
