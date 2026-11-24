// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <cstdio>
#include <utility>

namespace {

void write(const QByteArray &bytes)
{
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

QByteArray reply(const QString &id, const QJsonValue &result)
{
    return QJsonDocument(QJsonObject{{"id", id}, {"ok", true}, {"result", result}, {"error", QJsonValue::Null}})
        .toJson(QJsonDocument::Compact) + '\n';
}

int runRpcFixture(const QString &scenario)
{
    QSet<QString> ids, leases;
    QList<QByteArray> probes;
    QByteArray delayedOpen;
    int nextLease = 0;
    bool deferredBatch = scenario == "rpc-batch" || scenario == "rpc-late";
    bool cancelScenario = scenario == "rpc-cancel";
    char input[1024];
    while (std::fgets(input, sizeof input, stdin)) {
        const QByteArray line(input);
        const auto request = QJsonDocument::fromJson(line).object();
        const auto id = request.value("id").toString();
        const auto method = request.value("method").toString();
        const auto params = request.value("params").toObject();
        if (id.isEmpty() || ids.contains(id) || request.size() != 3 || !request.value("params").isObject()
            || (ids.isEmpty() && method != "status")) {
            return 10;
        }
        ids.insert(id);
        if (method == "status") {
            const auto state = QByteArrayLiteral(R"({"BackendState":"Running","TailscaleIPs":["100.76.1.10"],)"
                R"("Self":{"ID":"self","UserID":9223372036854775807},)"
                R"("CurrentTailnet":{"StableID":"tailnet"},)"
                R"("Peer":{"key":{"ID":"peer","Online":true,"UserID":9223372036854775807,)"
                R"("TailscaleIPs":["100.76.1.20","fd7a:115c:a1e0::20"]}}})");
            write("{\"id\":\"" + id.toLatin1() + "\",\"ok\":true,\"result\":" + state + ",\"error\":null}\n");
        } else if (method == "presence") {
            const auto address = params.value("address").toString();
            auto packet = QByteArray::fromBase64(params.value("packet").toString().toLatin1());
            if (params.size() != 3 || params.value("peerId") != QJsonValue("peer")
                || (address != "100.76.1.20" && address != "fd7a:115c:a1e0::20")
                || packet.size() != 192 || !packet.startsWith("AURORA/PRESENCE/1") || packet.at(17) != 1) {
                return 11;
            }
            packet[17] = 2;
            const auto response = reply(id, QString::fromLatin1(packet.toBase64()));
            if (deferredBatch) {
                if (probes.size() == 8)
                    return 12;
                probes.append(response);
            } else if (cancelScenario && delayedOpen.isEmpty()) {
                probes.append(response);
            } else {
                write(response + std::exchange(delayedOpen, {}));
            }
        } else if (method == "host" || method == "publish") {
            const auto enabled = params.value("enabled");
            if (!enabled.isBool())
                return 13;
            if (method == "publish" && enabled.toBool()) {
                const auto packet = QByteArray::fromBase64(params.value("packet").toString().toLatin1());
                if (params.size() != 4 || params.value("nodeId") != QJsonValue("self")
                    || !line.contains("\"owner\":9223372036854775807") || packet.size() != 192
                    || !packet.startsWith("AURORA/PRESENCE/1") || packet.at(17) != 2) {
                    return 14;
                }
            } else if (params.size() != 1) {
                return 15;
            }
            auto responses = reply(id, QJsonObject{{"enabled", enabled}});
            if (deferredBatch) {
                if (probes.size() != 8)
                    return 16;
                for (auto probe = probes.crbegin(); probe != probes.crend(); ++probe)
                    responses += *probe;
                deferredBatch = false;
                probes.clear();
            }
            write(responses.left(10));
            write(responses.mid(10));
        } else if (method == "open") {
            if (params.size() != 2 || params.value("address") != QJsonValue("100.76.1.20")
                || params.value("port") != QJsonValue(48989)) {
                return 17;
            }
            const auto lease = QString::number(++nextLease, 16).rightJustified(32, QLatin1Char('0'));
            leases.insert(lease);
            const int port = scenario == "rpc-bad-open" ? 65515 : 55010 + nextLease;
            const auto response = reply(id, QJsonObject{{"lease", lease}, {"port", port}});
            if (cancelScenario) {
                if (probes.size() != 1)
                    return 18;
                delayedOpen = response;
                write(probes.takeFirst());
            } else {
                write(response);
            }
        } else if (method == "close") {
            if (params.size() != 1 || !leases.remove(params.value("lease").toString()))
                return 19;
            if (scenario == "rpc-close-fault")
                return 20;
            cancelScenario = false;
            write(reply(id, QJsonObject{}));
        } else {
            return 21;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 || args.at(1) != "--state-dir" || !QDir::isAbsolutePath(args.at(2)))
        return 1;

    const auto scenario = QFileInfo(args.at(2)).fileName();
    if (scenario.startsWith("rpc-"))
        return runRpcFixture(scenario);
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
