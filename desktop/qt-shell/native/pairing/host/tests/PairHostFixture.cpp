// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

#include <cstdio>

namespace {

bool save(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool append(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write(bytes) == bytes.size();
}

int run(const QDir &dir)
{
    bool leased = false;
    char line[2048];
    while (std::fgets(line, sizeof line, stdin)) {
        const auto request = QJsonDocument::fromJson(line).object();
        const auto id = request.value("id").toString();
        const auto params = request.value("params").toObject();
        const auto method = request.value("method").toString();
        if (id.isEmpty())
            return 2;

        const auto node = dir.exists("swap-peer") ? QStringLiteral("different-peer") : QStringLiteral("peer");
        const auto account = params.value("owner").toInt() == 77 && params.value("nodeId").toString() == "self";
        QJsonObject result;
        if (method == "status") {
            result = {{"BackendState", "Running"}, {"TailscaleIPs", QJsonArray{"100.76.1.10"}},
                      {"Self", QJsonObject{{"ID", "self"}, {"UserID", 77}}},
                      {"CurrentTailnet", QJsonObject{{"StableID", "test-tailnet"}}},
                      {"Peer", QJsonObject{{"entry", QJsonObject{{"ID", node}, {"UserID", 77},
                                  {"HostName", "Fixture device"}, {"Online", true},
                                  {"TailscaleIPs", QJsonArray{"100.76.1.20"}}}}}}};
            if (!append(dir.filePath("statuses"), "ok\n"))
                return 3;
        } else if (method == "pair-host") {
            const auto port = params.value("localPort").toInt();
            const auto encoded = params.value("key").toString();
            if (!account || leased || port <= 0 || port > 65535 ||
                QByteArray::fromBase64(encoded.toLatin1()).size() != 32)
                return 4;
            leased = true;
            result = {{"leaseId", "00000000000000000000000000000001"}};
            if (!save(dir.filePath("bridge.json"), QJsonDocument(QJsonObject{{"port", port}, {"key", encoded}}).toJson()))
                return 5;
        } else if (method == "pair-source") {
            const auto port = params.value("sourcePort").toInt();
            if (!leased || !account || params.value("peerId").toString() != node ||
                params.value("address").toString() != "100.76.1.20" || port <= 0 || port > 65535)
                return 6;
            if (!append(dir.filePath("proofs"), QByteArray::number(port) + '\n'))
                return 7;
            result = {{"verified", port == 51007}};
        } else if (method == "close") {
            if (!leased || params.value("leaseId") != QJsonValue("00000000000000000000000000000001"))
                return 8;
            leased = false;
            if (!append(dir.filePath("closes"), "ok\n"))
                return 9;
        } else {
            return 10;
        }

        const auto response = QJsonDocument(QJsonObject{{"id", id}, {"ok", true}, {"result", result},
                                                        {"error", QJsonValue::Null}})
            .toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(response.constData(), 1, size_t(response.size()), stdout);
        std::fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 || args.at(1) != "--state-dir" || !QDir::isAbsolutePath(args.at(2)))
        return 1;
    return run(QDir(args.at(2)));
}
