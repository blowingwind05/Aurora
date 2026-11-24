// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
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
    int sessionStep = 0;
    const QStringList sessionMethods{"status", "login", "status", "status", "login", "logout", "status"};
    char input[256];
    while (std::fgets(input, sizeof input, stdin)) {
        const auto request = QJsonDocument::fromJson(input).object();
        const auto id = request.value("id").toString();
        const auto method = request.value("method").toString();
        if (id.isEmpty() || id == previousId || request.size() != 3 ||
                !request.value("params").isObject() || !request.value("params").toObject().isEmpty())
            return 2;
        const bool subsequent = !previousId.isEmpty();
        previousId = id;
        const bool firstAttempt = id == "1";
        if (scenario == "session") {
            if (sessionStep >= sessionMethods.size() || method != sessionMethods.at(sessionStep))
                return 2;
            QJsonObject result;
            if (method == "login")
                result.insert("pending", sessionStep == 1);
            else if (method == "status") {
                const QString state = sessionStep == 3 ? "Running" : sessionStep == 6 ? "Stopped" : "NeedsLogin";
                const QString url = sessionStep == 0 ? "https://login.tailscale.com.invalid/a/fixture" :
                    "https://login.tailscale.com/a/offline-fixture";
                result = QJsonObject{{"BackendState", state}, {"AuthURL", url}};
            }
            ++sessionStep;
            write(QJsonDocument(QJsonObject{{"id", id}, {"ok", true}, {"result", result}, {"error", QJsonValue::Null}})
                .toJson(QJsonDocument::Compact) + '\n');
            continue;
        }
        const bool login = firstAttempt && scenario.startsWith("login-");
        const bool logout = firstAttempt && scenario.startsWith("logout-");
        if (method != (login ? "login" : logout ? "logout" : "status"))
            return 2;
        if (firstAttempt && (scenario == "exit" || scenario == "logout-exit"))
            return 3;
        if (firstAttempt && (scenario == "silent" || scenario == "logout-silent")) {
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
        if (firstAttempt && scenario == "login-oversize") {
            write(QByteArray(4097, 'x'));
            continue;
        }
        const bool rejected = firstAttempt && (scenario == "rejected" || scenario == "login-rejected");
        QJsonObject result;
        if (login)
            result = QJsonObject{{"pending", scenario == "login-bad-shape" ? QJsonValue("true") : QJsonValue(true)}};
        else if (logout)
            result = QJsonObject{{"pending", true}};
        else
            result = QJsonObject{{"BackendState", subsequent ? "NeedsLogin" : "Starting"}};
        QJsonObject reply{
            {"id", firstAttempt && (scenario == "wrong-id" || scenario == "login-wrong-id") ? QStringLiteral("wrong") : id},
            {"ok", !rejected},
            {"result", result},
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
