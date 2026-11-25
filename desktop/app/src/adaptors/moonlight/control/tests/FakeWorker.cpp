// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments().mid(1);
    const auto mode = qgetenv("AURORA_CONTROL_FIXTURE_MODE");
    const auto record = QJsonDocument(QJsonObject{
        {"args", QJsonArray::fromStringList(args)},
        {"base", qEnvironmentVariable("AURORA_GAMESTREAM_RELAY_BASE")},
        {"host", qEnvironmentVariable("AURORA_GAMESTREAM_HOST_UUID")}
    }).toJson(QJsonDocument::Compact) + '\n';
    QFile log(qEnvironmentVariable("AURORA_CONTROL_FIXTURE_LOG"));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append) || log.write(record) != record.size() || !log.flush())
        return 70;
    log.close();

    if (args == QStringList{QStringLiteral("--aurora-gamestream-relay-info")}) {
        if (mode == "probe-hold")
            return app.exec();
        std::puts(mode == "probe-bad" ? "aurora-gamestream-relay/0" : "aurora-gamestream-relay/1");
        return 0;
    }
    if (args.size() >= 5 && args.first() == "--aurora-runtime" && args.at(1) == "bitrate") {
        const QByteArray token(32, 'a');
        if (mode == "bitrate-unsupported" || mode == "bitrate-unavailable" || mode == "bitrate-stale" ||
            mode == "bitrate-busy" || mode == "bitrate-pending" || mode == "bitrate-expired" ||
            mode == "bitrate-rejected" || mode == "bitrate-failed") {
            std::puts(mode.constData() + sizeof("bitrate-") - 1);
        } else if (args.size() == 5 && args.last() == "status") {
            std::puts(("status " + token + (mode == "bitrate-invalid" ? " 020000 16000 25000 7" : " 20000 16000 25000 7")).constData());
        } else if (args.size() == 8 && args.at(4) == "set" && args.at(5).toLatin1() == token) {
            const auto reply = mode == "bitrate-mismatch" ? QByteArray(32, 'b') : token;
            const auto revision = mode == "bitrate-revision-mismatch" ? args.last().toUInt() : args.last().toUInt() + 1;
            std::puts(("applied " + reply + " " + args.at(6).toLatin1() + " 12000 25000 " + QByteArray::number(revision)).constData());
        } else {
            return 71;
        }
        return 0;
    }
    if (args.size() != 5 || args.first() != "--aurora-runtime" ||
        (args.at(1) != "power" && args.at(1) != "privacy"))
        return 71;
    if (mode == "hold")
        return app.exec();
    if (mode == "overflow") {
        std::fputs(QByteArray(65536, 'x').constData(), stdout);
        std::fflush(stdout);
        return app.exec();
    }
    if (mode == "split") {
        std::fputs("acc", stdout);
        std::fflush(stdout);
        QTimer::singleShot(40, &app, [&app] {
            std::puts("epted ");
            app.exit(0);
        });
        return app.exec();
    }
    if (mode == "missing-pairing")
        return 3;
    if (mode == "rejected") {
        std::puts("accepted");
        return 4;
    }
    if (mode == "stderr") {
        for (int i = 0; i < 128; ++i)
            std::fputs(QByteArray(1024, 'x').constData(), stderr);
    } else if (mode == "extra-line") {
        std::puts("accepted\nextra");
        return 0;
    } else if (mode == "nul-suffix") {
        const char response[] = "accepted\0extra";
        std::fwrite(response, 1, sizeof response - 1, stdout);
        return 0;
    }

    if (args.at(1) == "power")
        std::puts("  accepted ");
    else if (args.last() == "display-off")
        std::puts(" display-off ");
    else if (args.last() == "status")
        std::puts(" unavailable ");
    else
        std::puts(args.last() == "on" ? " on " : " off ");
    return 0;
}
