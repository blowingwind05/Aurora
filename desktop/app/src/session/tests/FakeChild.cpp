// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraStreamEntry.h"
#include "AuroraStreamSessionProtocol.h"
#ifdef Q_OS_MACOS
#include <QSocketNotifier>
#include <unistd.h>
#endif

#include <QCoreApplication>
#include <QFile>
#include <QSize>
#include <QTimer>

#include <cstdio>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

void report(const char *line)
{
    std::puts(line);
    std::fflush(stdout);
}

#ifdef Q_OS_WIN
bool matchesControlRoute(char **argv)
{
    const auto endpoint = QByteArray(argv[2]);
    const auto host = QByteArray(argv[3]);
    const auto relay = qgetenv("AURORA_TEST_STREAM_RELAY");
    const auto expectedPin = qgetenv("AURORA_TEST_STREAM_PIN");
    return (expectedPin.isEmpty() || expectedPin == qgetenv("AURORA_GAMESTREAM_HOST_CERTIFICATE_SHA256")) &&
        endpoint == qgetenv("AURORA_TEST_STREAM_ENDPOINT") && host == qgetenv("AURORA_TEST_STREAM_HOST") &&
        qgetenv("AURORA_GAMESTREAM_RELAY_BASE") == relay && !qEnvironmentVariableIsSet("AURORA_EMBEDDED_SESSION") &&
        qgetenv("AURORA_GAMESTREAM_HOST_UUID") == (relay.isEmpty() ? QByteArray() : host);
}

int fakeBitrate(int argc, char **argv)
{
    if (argc < 5 || !matchesControlRoute(argv))
        return 23;
    const QByteArray session(32, 'a');
    const bool pending = qEnvironmentVariableIsSet("AURORA_TEST_BITRATE_PENDING");
    QFile settled(qEnvironmentVariable("AURORA_TEST_PRIVACY_LOG") + QStringLiteral(".bitrate"));
    if (argc == 5 && QByteArray(argv[4]) == "status") {
        report(("status " + session + (pending && settled.exists() ? " 15000 12000 25000 8" : " 20000 16000 25000 7")).constData());
        return 0;
    }
    if (argc == 8 && QByteArray(argv[4]) == "set" && QByteArray(argv[5]) == session &&
        QByteArray(argv[6]) == "15000" && QByteArray(argv[7]) == "7") {
        if (pending) {
            if (!settled.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return 24;
            report("pending");
        } else {
            report(("applied " + session + " 15000 12000 25000 8").constData());
        }
        return 0;
    }
    return 24;
}

int fakePrivacy(int argc, char **argv)
{
    if (!matchesControlRoute(argv))
        return 23;
    const auto operation = QByteArray(argv[4]);
    if (operation != "status" && operation != "on" && operation != "off" && operation != "display-off")
        return 24;

    QCoreApplication app(argc, argv);
    QFile log(qEnvironmentVariable("AURORA_TEST_PRIVACY_LOG"));
    const auto record = operation + ' ' + QByteArray::number(app.applicationPid()) + '\n';
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append) || log.write(record) != record.size() || !log.flush())
        return 25;
    log.close();

    const auto mode = qgetenv("AURORA_TEST_PRIVACY_MODE");
    if (mode == "hold")
        return app.exec();
    if (mode == "failed")
        return 4;
    const auto reply = mode == "unavailable" || mode == "unsupported" ? mode :
        operation == "status" || (mode == "no-virtual-display" && operation == "display-off")
            ? QByteArray("off") : operation;
    QTimer::singleShot(120, &app, [&app, reply] {
        report(reply.constData());
        app.quit();
    });
    return app.exec();
}

LPCWSTR wide(const QString &text)
{
    return reinterpret_cast<LPCWSTR>(text.utf16());
}

bool recordVideo(const QByteArray &event, const QByteArray &fps)
{
    QFile log(qEnvironmentVariable("AURORA_TEST_VIDEO_LOG"));
    const auto line = event + ' ' + fps + '\n';
    return log.open(QIODevice::Append) && log.write(line) == line.size() && log.flush();
}

bool hasHost(HWND hwnd, DWORD expectedPid, const QString &property)
{
    DWORD pid = 0;
    return GetWindowThreadProcessId(hwnd, &pid) && pid == expectedPid
        && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD)
        && GetPropW(hwnd, wide(property)) == reinterpret_cast<HANDLE>(1);
}

HWND makeChild(HWND host, const wchar_t *windowClass)
{
    RECT bounds{};
    if (!GetClientRect(host, &bounds))
        return nullptr;
    const auto child = CreateWindowExW(0, windowClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                       0, 0, qMax(1L, bounds.right), qMax(1L, bounds.bottom),
                                       host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (child)
        ShowWindow(child, SW_SHOWNOACTIVATE);
    return child;
}

LRESULT CALLBACK paintVideo(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        const auto brush = CreateSolidBrush(RGB(19, 71, 103));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
#endif

} // namespace

int main(int argc, char **argv)
{
    const bool supervisor = argc > 1;
    if (supervisor) {
        const auto invocation = prepareAuroraStreamInvocation(argc, argv);
        if (invocation.operation == AuroraStreamOperation::Exit)
            return invocation.exitCode;
#ifdef Q_OS_WIN
        if (argc == 5 && std::strcmp(argv[1], "privacy") == 0)
            return fakePrivacy(argc, argv);
        if (std::strcmp(argv[1], "bitrate") == 0)
            return fakeBitrate(argc, argv);
#endif
        if (argc < 4 || std::strcmp(argv[1], "stream") || std::strcmp(argv[3], "Desktop")
            || QByteArray(argv[2]) != qgetenv("AURORA_TEST_STREAM_ENDPOINT")) {
            return 17;
        }
        const auto relay = qgetenv("AURORA_TEST_STREAM_RELAY");
        if ((!relay.isEmpty() && qgetenv("AURORA_GAMESTREAM_HOST_UUID") != qgetenv("AURORA_TEST_STREAM_HOST"))
            || qgetenv("AURORA_GAMESTREAM_RELAY_BASE") != relay) {
            return 18;
        }
        QFile launched(qEnvironmentVariable("AURORA_TEST_STREAM_LAUNCHED"));
        if (!launched.open(QIODevice::WriteOnly))
            return 19;
    }
    QCoreApplication app(argc, argv);
    const auto mode = qgetenv("AURORA_TEST_STREAM_MODE");
#ifdef Q_OS_WIN
    const bool recordPlayback = mode == "video" || mode == "video-ignore-stop";
    QByteArray fps;
    if (recordPlayback) {
        const auto args = app.arguments();
        fps = args.value(args.indexOf("--fps") + 1).toLatin1();
        // Keep the exclusive handle open until process exit. A successor that
        // starts before this process has exited cannot acquire it.
        const auto lock = CreateFileW(wide(qEnvironmentVariable("AURORA_TEST_VIDEO_LOCK")),
                                      GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock == INVALID_HANDLE_VALUE) {
            recordVideo("overlap", fps);
            return 75;
        }
        if (!recordVideo("start", fps))
            return 76;
        if (fps == "55") {
            recordVideo("failed", fps);
            return 74;
        }
    }
    const auto fields = qEnvironmentVariable("AURORA_EMBEDDED_SESSION").split(QLatin1Char(':'));
    if (fields.size() != 3 || fields[2].size() != 32)
        return 74;
    const auto host = reinterpret_cast<HWND>(static_cast<quintptr>(fields[0].toLongLong()));
    const DWORD pid = fields[1].toUInt();
    const auto tag = QStringLiteral("Aurora.Embedded.") + fields[2];
    if (!hasHost(host, pid, tag))
        return 74;

    const auto prefix = QStringLiteral("Local\\") + tag;
    const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide(prefix + QStringLiteral(".ready")));
    const auto stop = OpenEventW(SYNCHRONIZE, FALSE, wide(prefix + QStringLiteral(".stop")));
    const auto fullscreen = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide(prefix + QStringLiteral(".fullscreen")));
    if (!ready || !stop || !fullscreen)
        return 74;
    if (mode == "attach-fail")
        return 74;

    WNDCLASSW wc{};
    wc.lpfnWndProc = paintVideo;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AuroraEmbeddedStreamSurface";
    if (!RegisterClassW(&wc))
        return 74;

    const HWND stage = makeChild(host, L"STATIC");
    if (!stage || GetParent(stage) != host)
        return 74;
    report("aurora-connection-stage/1,qt");
    if (qEnvironmentVariableIsSet("AURORA_TEST_PINNED_CONTROL"))
        report(Aurora::StreamSession::PinnedControlAvailable);

    HWND video = nullptr;
    QSize lastSize;
    QByteArray pending;
    bool receivedStop = false;
    bool fixtureWeak = true;
    const auto showVideo = [&] {
        video = makeChild(host, wc.lpszClassName);
        if (!video || GetParent(video) != host) {
            app.exit(74);
            return;
        }
        ShowWindow(stage, SW_HIDE);
        if (recordPlayback)
            recordVideo("ready", fps);
        SetEvent(ready);
    };
    const bool deferReady = recordPlayback && fps == "50";
    if (supervisor) {
        if (!deferReady)
            QTimer::singleShot(40, &app, showVideo);
        QTimer::singleShot(75, &app, [] {
            const QByteArray noise(800, 'x');
            report(noise.constData());
            std::fputs("aurora-connection/1,1920,1080,2,", stdout);
            std::fflush(stdout);
        });
        QTimer::singleShot(105, &app, [] { report("5994,18000,100,4200,30,1"); });
        if (mode == "fullscreen")
            QTimer::singleShot(220, &app, [fullscreen] { SetEvent(fullscreen); });
    }
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (mode == "quality-reset") {
            const bool weak = !QFile::exists(qEnvironmentVariable("AURORA_TEST_PLAYBACK_EXIT"));
            if (weak != fixtureWeak) {
                fixtureWeak = weak;
                report(weak ? "aurora-connection/1,1920,1080,2,5994,18000,100,4200,30,1"
                            : "aurora-connection/1,1920,1080,2,5994,18000,0,4200,30,0");
            }
        }
        if ((mode == "exit-on-fixture" || mode == "transport-loss") &&
            QFile::exists(qEnvironmentVariable("AURORA_TEST_PLAYBACK_EXIT"))) {
            app.exit(mode == "transport-loss" ? Aurora::StreamSession::TransportLost : 0);
            return;
        }
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            if (mode == "ignore-stop" || mode == "video-ignore-stop")
                return;
            if (supervisor) {
                if (!receivedStop) {
                    receivedStop = true;
                    QTimer::singleShot(240, &app, &QCoreApplication::quit);
                }
                return;
            }
            report("fixture-stop-event");
            app.quit();
            return;
        }
        if (!hasHost(host, pid, tag)) {
            report("fixture-lost-host");
            app.quit();
            return;
        }
        if (deferReady && !video && QFile::exists(qEnvironmentVariable("AURORA_TEST_VIDEO_READY")))
            showVideo();

        const auto input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD queued = 0;
        if (PeekNamedPipe(input, nullptr, 0, nullptr, &queued, nullptr) && queued > 0) {
            char buffer[128];
            DWORD count = 0;
            if (ReadFile(input, buffer, qMin<DWORD>(queued, sizeof buffer), &count, nullptr))
                pending.append(buffer, static_cast<int>(count));
        }
        while (pending.contains('\n')) {
            const auto newline = pending.indexOf('\n');
            const auto command = pending.left(newline).trimmed();
            pending.remove(0, newline + 1);
            if (command == "ready" && !video) {
                showVideo();
            } else if (command == "fullscreen") {
                SetEvent(fullscreen);
            } else if (command == "exit74") {
                app.exit(74);
                return;
            }
        }

        RECT viewport{};
        if (GetClientRect(host, &viewport)) {
            const QSize size(viewport.right, viewport.bottom);
            if (size != lastSize) {
                SetWindowPos(video ? video : stage, nullptr, 0, 0, size.width(), size.height(),
                             SWP_NOACTIVATE | SWP_NOZORDER);
                std::printf("fixture-size %d %d\n", size.width(), size.height());
                std::fflush(stdout);
                lastSize = size;
            }
        }
    });
    tick.start(15);
    const int status = app.exec();
    if (video)
        DestroyWindow(video);
    DestroyWindow(stage);
    CloseHandle(fullscreen);
    CloseHandle(stop);
    CloseHandle(ready);
    if (recordPlayback)
        recordVideo("exit", fps);
    return status;
#else
    if (qEnvironmentVariableIsSet("AURORA_EMBEDDED_SESSION"))
        return 74;
#ifdef Q_OS_MACOS
    using namespace Aurora::StreamSession;
    if (mode.startsWith("route-switch-")) {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
            return 1;
        QSocketNotifier command(STDIN_FILENO, QSocketNotifier::Read);
        const bool supported = mode != "route-switch-legacy";
        command.setEnabled(supported && qEnvironmentVariableIntValue(SwitchChannel) == 1);
        QObject::connect(&command, &QSocketNotifier::activated, &app, [&] {
            if (input.readLine(sizeof(SwitchRequest)) != SwitchRequest) {
                app.exit(1);
            } else if (mode == "route-switch-deny") {
                report(SwitchDenied);
            } else {
                app.exit(RouteSwitched);
            }
        });
        QTimer::singleShot(0, &app, [&] {
            report(supported ? SwitchReady : PlaybackReady);
            if (mode == "route-switch-unsolicited")
                app.exit(RouteSwitched);
        });
        return app.exec();
    }
#endif
    QTimer::singleShot(40, &app, [] {
        report("aurora-stream-setting/1\tresolution\t1920x1080");
        report("aurora-stream-setting/1\tunknown-option\tbogus");
    });
    return app.exec();
#endif
}
