// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QThread>

#include <cstdio>

namespace {

void write(FILE *stream, const QByteArray &bytes)
{
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
    std::fflush(stream);
}

const QByteArray status = R"({"BackendState":"Running","TailscaleIPs":["100.76.1.1"],
    "Self":{"ID":"system-local","UserID":9223372036854775807},
    "CurrentTailnet":{"Name":"fixture.example.invalid","StableID":"system-tailnet"},
    "User":{"9223372036854775807":{"ID":9223372036854775807,"DisplayName":"Fixture owner"}},
    "Peer":{"key0":{"ID":"same-owner","UserID":9223372036854775807,"TailscaleIPs":["100.76.1.2"]},
            "key1":{"ID":"different-owner","UserID":9223372036854775806,"TailscaleIPs":["100.76.1.3"]}}})";

const QByteArray profiles = R"([{"id":"home","account":"owner@example.invalid","nickname":"Home","tailnet":"home.test","selected":true},
    {"id":"work","account":"work@example.invalid","selected":false},
    {"id":"work","account":"duplicate@example.invalid"},
    {"id":"--delete","account":"malformed@example.invalid"}])";

const QByteArray legacy = "ID    Tailnet            Account\n"
    "home  home.test          Fixture personal device*\n"
    "work  work.test          work@example.invalid\n";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments().mid(1);
    const auto socket = QStringLiteral(R"(--socket=\\.\pipe\protectedprefix\administrators\tailscale\tailscaled)");
    const bool readStatus = arguments == QStringList{socket, "status", "--json"};
    const bool readProfiles = arguments == QStringList{socket, "switch", "--list", "--json"};
    const bool readLegacy = arguments == QStringList{socket, "switch", "--list"};
    if (!readStatus && !readProfiles && !readLegacy)
        return 91;

    const auto scenario = QFileInfo(app.applicationFilePath()).completeBaseName();
    if (scenario == "status" && readStatus) {
        write(stdout, "\xef\xbb\xbf" + status.left(169));
        QThread::msleep(35);
        write(stdout, status.mid(169));
        return 0;
    }
    if (scenario == "profiles" && readProfiles) {
        write(stdout, profiles);
        return 0;
    }
    if (scenario == "large-status" && readStatus) {
        const QByteArray chunk(32768, 'x');
        for (int i = 0; i < 129; ++i)
            write(stdout, chunk);
        return 0;
    }
    if (scenario == "large-stderr" && readProfiles) {
        write(stderr, QByteArray(64 * 1024 + 1, 'x'));
        return 1;
    }
    if (scenario == "bad-profiles" && readProfiles) {
        write(stdout, "{}");
        return 0;
    }
    if (readProfiles && (scenario == "legacy" || scenario == "legacy-deadline")) {
        if (scenario == "legacy-deadline")
            QThread::msleep(4500);
        write(stderr, "flag provided but not defined: -json\n");
        return 1;
    }
    if (scenario == "profile-denied" && readProfiles) {
        write(stderr, "fixture permission denied\n");
        return 1;
    }
    if (scenario == "legacy-deadline" && readLegacy) {
        write(stdout, legacy.left(11));
        QThread::msleep(5000);
        write(stdout, legacy.mid(11));
        return 0;
    }
    // These response paths let the caller detect an unintended fallback.
    if (readLegacy && (scenario == "legacy" || scenario == "bad-profiles" || scenario == "profile-denied")) {
        write(stdout, "\xef\xbb\xbf" + legacy);
        return 0;
    }
    return 92;
}
