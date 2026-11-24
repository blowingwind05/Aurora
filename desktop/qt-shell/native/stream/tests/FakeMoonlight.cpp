// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraStreamEntry.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QVector>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto original = app.arguments();

    // Use the real private entry gate. All accepted commands end in the fake below.
    QList<QByteArray> storage;
    for (const auto &argument : original)
        storage.append(argument.toUtf8());
    QVector<char *> arguments;
    for (auto &argument : storage)
        arguments.append(argument.data());
    arguments.append(nullptr);
    int count = original.size();
    const int gate = prepareAuroraStreamInvocation(count, arguments.data());
    if (gate >= 0)
        return gate;
    if (original.contains(QStringLiteral("--help"))) {
        std::puts("--aurora-background --aurora-pair-id");
        return 0;
    }

    QJsonObject environment;
    for (const auto *key : {"AURORA_EMBEDDED_SESSION", "AURORA_CONNECTION_DETAILS", "AURORA_STREAM_DEVICE_NAME",
                            "AURORA_STREAM_PREFERENCES", "AURORA_GAMESTREAM_RELAY_BASE",
                            "AURORA_GAMESTREAM_HOST_UUID", "AURORA_STREAM_TEST_SENTINEL"}) {
        if (qEnvironmentVariableIsSet(key))
            environment.insert(QString::fromLatin1(key), qEnvironmentVariable(key));
    }
    const auto report = QJsonDocument(QJsonObject{
        {QStringLiteral("argv"), QJsonArray::fromStringList(original.mid(1))},
        {QStringLiteral("verb"), QString::fromUtf8(arguments[1])},
        {QStringLiteral("cwd"), QDir::currentPath()},
        {QStringLiteral("env"), environment},
    }).toJson(QJsonDocument::Compact);
    const QByteArray noise(1024, 'e');
    for (int index = 0; index < 128; ++index) {
        std::fwrite(noise.constData(), 1, noise.size(), stderr);
    }
    std::puts(report.constData());
    std::fflush(stdout);
    return qEnvironmentVariableIsSet("AURORA_STREAM_TEST_HOLD") ? app.exec()
                                                               : qEnvironmentVariableIntValue("AURORA_STREAM_TEST_EXIT");
}
