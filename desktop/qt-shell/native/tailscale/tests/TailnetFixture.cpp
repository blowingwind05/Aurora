// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <cstdio>

namespace {

void write(const QByteArray &bytes)
{
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 || args.at(1) != "--state-dir" || !QDir::isAbsolutePath(args.at(2)))
        return 1;

    const auto scenario = QFileInfo(args.at(2)).fileName();
    QString previousId;
    char input[256];
    while (std::fgets(input, sizeof input, stdin)) {
        const auto request = QJsonDocument::fromJson(input).object();
        const auto id = request.value("id").toString();
        if (id.isEmpty() || id == previousId || request.size() != 3 || request.value("method").toString() != "status" ||
                !request.value("params").isObject() || !request.value("params").toObject().isEmpty())
            return 2;
        const bool subsequent = !previousId.isEmpty();
        previousId = id;
        const bool firstAttempt = id == "1";
        if (firstAttempt && scenario == "exit")
            return 3;
        if (firstAttempt && scenario == "silent") {
            write("{");
            QThread::msleep(30000);
            return 4;
        }
        if (firstAttempt && scenario == "oversize") {
            const QByteArray chunk(32768, 'x');
            for (int i = 0; i < 129; ++i)
                write(chunk);
            continue;
        }
        const bool rejected = firstAttempt && scenario == "rejected";
        QJsonObject reply{
            {"id", firstAttempt && scenario == "wrong-id" ? QStringLiteral("wrong") : id},
            {"ok", !rejected},
            {"result", QJsonObject{{"BackendState", subsequent ? "NeedsLogin" : "Starting"}}},
            {"error", QJsonValue::Null}
        };
        if (rejected) {
            reply.insert("result", QJsonValue::Null);
            reply.insert("error", "fixture rejection");
        }
        const auto bytes = QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
        if (firstAttempt && scenario == "extra-frame") {
            write(bytes + bytes);
            continue;
        }
        write(bytes.left(9));
        QThread::msleep(15);
        write(bytes.mid(9));
    }
    return 0;
}
