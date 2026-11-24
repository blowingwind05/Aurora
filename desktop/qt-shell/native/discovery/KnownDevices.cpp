// SPDX-License-Identifier: GPL-3.0-only
#include "KnownDevices.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>

namespace Aurora::Discovery {
namespace {
QString digest(const QString &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex().toUpper());
}

QUuid readIdentity(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto text = file.read(33);
    if (text.size() != 32 || file.error() != QFileDevice::NoError)
        return {};
    const auto bytes = QByteArray::fromHex(text);
    return bytes.toHex() == text.toLower() ? QUuid::fromRfc4122(bytes) : QUuid{};
}
}

KnownIdentities readKnownDevices(const QString &dataDirectory, const NetworkSnapshot &network)
{
    KnownIdentities known;
    if (!QDir::isAbsolutePath(dataDirectory) || !network.isAuthenticated())
        return known;
    const auto account = digest(QString::number(network.owner) + QLatin1Char('/') + network.localId);
    const QDir directory(QDir(dataDirectory).filePath(QStringLiteral("KnownDevices/") + account));
    for (const auto &peer : network.devices) {
        const auto id = readIdentity(directory.filePath(digest(peer.id)));
        if (!id.isNull())
            known.insert(peer.id, id);
    }
    return known;
}

}
