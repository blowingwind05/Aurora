// SPDX-License-Identifier: GPL-3.0-only
#include "KnownDevices.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace Aurora::Discovery;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir data;
    if (!data.isValid())
        return 1;

    // Match the legacy SHA-256 filenames and Guid.ToString("N") contents.
    const auto directory = QDir(data.path()).filePath(
        QStringLiteral("KnownDevices/22E30B623763F3882DBE3946D85DAC1BFA36842BEB0165ECC874E6BB6214D2CF"));
    if (!QDir().mkpath(directory))
        return 2;
    QFile identity(QDir(directory).filePath(
        QStringLiteral("9365724BA55A55F51FACAB0E6EAC23751551C97B27E559476C963807EB7E2A5E")));
    if (!identity.open(QIODevice::WriteOnly) || identity.write("213e55aa6fde4ac2a2f1954d278763a7") != 32)
        return 3;
    identity.close();

    NetworkSnapshot network{QHostAddress(QStringLiteral("100.64.0.1")), 42, QStringLiteral("local-node"), {}};
    Device peer;
    peer.id = QStringLiteral("remote-node");
    peer.address = QHostAddress(QStringLiteral("100.64.0.2"));
    network.devices.append(peer);
    const auto known = readKnownDevices(data.path(), network);
    if (known.size() != 1 || known.value(peer.id) != QUuid(QStringLiteral("{213e55aa-6fde-4ac2-a2f1-954d278763a7}")))
        return 4;
    ++network.owner;
    if (!readKnownDevices(data.path(), network).isEmpty())
        return 5;
    --network.owner;
    network.localId = QStringLiteral("other-node");
    if (!readKnownDevices(data.path(), network).isEmpty())
        return 6;

    network.localId = QStringLiteral("local-node");
    const auto replacement = QUuid(QStringLiteral("{c2f9e074-4aa0-467c-8ade-e943f848dbf2}"));
    const QList<PresentDevice> reported{{replacement, peer.address, QStringLiteral("new host"), QStringLiteral("windows"), true}};
    if (rememberKnownDevices(data.path(), network, reported).value(peer.id) != replacement ||
        readKnownDevices(data.path(), network).value(peer.id) != replacement)
        return 7;
    const auto ownIdentity = readOrCreatePresenceIdentity(data.path());
    if (ownIdentity.isNull() || ownIdentity != readOrCreatePresenceIdentity(data.path()))
        return 8;
    return 0;
}
