// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 2 && args.at(1) == "--aurora-gamestream-relay-info") {
        std::puts(qEnvironmentVariableIsSet("PAIR_FIXTURE_BAD_RELAY") ? "unexpected" : "aurora-gamestream-relay/1");
        return 0;
    }
    if (args.mid(1) == QStringList{"--aurora-runtime", "pair", "--aurora-background", "--help"}) {
        std::puts(qEnvironmentVariableIsSet("PAIR_FIXTURE_BAD_RELAY") ? "unexpected" : "--aurora-background --aurora-pair-id");
        return 0;
    }
    if (args.size() == 4 && args.at(1) == "--aurora-runtime" && args.at(2) == "client-proof") {
        if (!QFileInfo::exists("portable.dat") || args.at(3).size() != 192 ||
            args.at(3).mid(64, 64) != qEnvironmentVariable("AURORA_GAMESTREAM_HOST_CERTIFICATE_SHA256") ||
            qEnvironmentVariableIsSet("AURORA_GAMESTREAM_RELAY_BASE"))
            return 5;
        QFile trace(QStringLiteral("trace"));
        if (!trace.open(QIODevice::WriteOnly | QIODevice::Append))
            return 6;
        trace.write("proof\n");
        std::puts("aurora-client-proof/1,Y2VydA==,c2ln");
        return 0;
    }
    const auto base = qEnvironmentVariable("AURORA_GAMESTREAM_RELAY_BASE");
    const auto uuid = qEnvironmentVariable("PAIR_FIXTURE_HOST");
    if (args.size() < 4 || args.at(1) != "--aurora-runtime" ||
        args.last() != qEnvironmentVariable("PAIR_FIXTURE_ENDPOINT") || !QFileInfo::exists("portable.dat"))
        return 5;
    if (qEnvironmentVariableIsSet("PAIR_FIXTURE_DIRECT")) {
        if (qEnvironmentVariableIsSet("AURORA_GAMESTREAM_RELAY_BASE") ||
            qEnvironmentVariableIsSet("AURORA_GAMESTREAM_HOST_UUID"))
            return 5;
    } else if (base != qEnvironmentVariable("PAIR_FIXTURE_ROUTE_BASE") ||
               qEnvironmentVariable("AURORA_GAMESTREAM_HOST_UUID") != uuid) {
        return 5;
    }

    QFile trace(QStringLiteral("trace"));
    if (!trace.open(QIODevice::WriteOnly | QIODevice::Append))
        return 6;
    if (args.at(2) == "list") {
        if (args.size() != 4)
            return 7;
        trace.write("list\n");
        trace.close();
        return qEnvironmentVariableIntValue("PAIR_FIXTURE_LIST_EXIT");
    }
    if (args.size() != 9 || args.at(2) != "pair" || args.at(3) != "--aurora-background" ||
        args.at(4) != "--aurora-pair-id" || args.at(5) != QString(32, QLatin1Char('a')) ||
        args.at(6) != "--pin" || args.at(7) != "0234")
        return 8;

    trace.write("pair\n");
    trace.close();
    if (qEnvironmentVariableIsSet("PAIR_FIXTURE_HOLD"))
        return app.exec();

    const auto mode = qEnvironmentVariable("PAIR_FIXTURE_STORE");
    if (mode != "missing") {
#ifdef Q_OS_MACOS
        const auto org = QStringLiteral("moonlight-stream.com");
#else
        const auto org = QStringLiteral("Moonlight Game Streaming Project");
#endif
        QFile cert(qEnvironmentVariable("PAIR_FIXTURE_CERT"));
        if (!cert.open(QIODevice::ReadOnly))
            return 9;
        const auto pem = cert.readAll();
        QSettings settings(QDir::current().filePath(org + QStringLiteral("/Moonlight.ini")), QSettings::IniFormat);
        settings.beginWriteArray(mode == "backup" ? "hostsbackup" : "hosts");
        for (int i = 0; i < (mode == "duplicate" ? 2 : 1); ++i) {
            settings.setArrayIndex(i);
            settings.setValue("uuid", mode == "different" ? QStringLiteral("different-host") : uuid);
            settings.setValue("srvcert", mode == "bad-cert" ? QByteArray("not a certificate") : pem);
        }
        settings.endArray();
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return 10;
    }
    return qEnvironmentVariableIntValue("PAIR_FIXTURE_EXIT");
}
