// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
    const bool up = arguments == QStringList{socket, "up"};
    const bool logout = arguments == QStringList{socket, "logout"};
    const bool switchProfile = arguments == QStringList{socket, "switch", "--", "work"};
    if (!readStatus && !readProfiles && !readLegacy && !up && !logout && !switchProfile)
        return 91;

    const QFileInfo executable(app.applicationFilePath());
    const auto scenario = executable.completeBaseName();
    QFile trace(executable.dir().filePath("fixture-commands.txt"));
    if (!trace.open(QIODevice::ReadWrite))
        return 93;
    const auto prior = trace.readAll();
    trace.write(arguments.mid(1).join(' ').toUtf8() + '\n');
    trace.close();

    if (scenario.startsWith("action-")) {
        if (readStatus && scenario == "action-up-timeout") {
            write(stdout, R"({"BackendState":"NeedsLogin","AuthURL":"https://login.tailscale.com/a/fixture"})");
            return 0;
        }
        if (up && scenario == "action-up") {
            write(stdout, "fixture up completed");
            return 0;
        }
        if (up && scenario == "action-up-denied") {
            write(stderr, "fixture up refusal");
            return 17;
        }
        if (up && scenario == "action-up-timeout") {
            write(stdout, "fixture up waiting");
            QThread::msleep(7000);
            return 0;
        }
        if (logout && scenario == "action-logout")
            return 0;
        if (logout && scenario == "action-logout-denied")
            return 23;
        return 94;
    }
    if (scenario.startsWith("select-")) {
        const bool changed = prior.contains("switch -- work\n") &&
            scenario != "select-unverified" && scenario != "select-unconfirmed";
        if (readProfiles || readLegacy) {
            if (scenario == "select-legacy" && readProfiles) {
                write(stderr, "flag provided but not defined: -json");
                return 1;
            }
            if (scenario == "select-disappeared" && !prior.isEmpty()) {
                write(stdout, R"([{"id":"home","account":"owner@example.invalid","selected":true}])");
                return 0;
            }
            auto list = readLegacy ? legacy : profiles;
            if (changed) {
                if (readLegacy) {
                    list.replace("device*", "device");
                    list.replace("work@example.invalid", "work@example.invalid*");
                } else {
                    list.replace("\"selected\":true", "\"selected\":false");
                    list.replace("\"account\":\"work@example.invalid\",\"selected\":false",
                        "\"account\":\"work@example.invalid\",\"selected\":true");
                }
            }
            write(stdout, list);
            return 0;
        }
        if (switchProfile) {
            if (scenario == "select-timeout") {
                QThread::msleep(14000);
                return 0;
            }
            return scenario == "select-recovered" || scenario == "select-unverified" || scenario == "select-legacy"
                ? 37 : 0;
        }
        return 95;
    }
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
