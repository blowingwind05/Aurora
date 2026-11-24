// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraStreamEntry.h"

#include <QCoreApplication>
#include <QFile>
#include <QSize>
#include <QTimer>

#include <cstdio>
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
LPCWSTR wide(const QString &text)
{
    return reinterpret_cast<LPCWSTR>(text.utf16());
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
        const int guard = prepareAuroraStreamInvocation(argc, argv);
        if (guard >= 0)
            return guard;
        if (argc < 4 || std::strcmp(argv[1], "stream") || std::strcmp(argv[3], "Desktop")
            || QByteArray(argv[2]) != qgetenv("AURORA_TEST_STREAM_ENDPOINT")) {
            return 17;
        }
        const auto relay = qgetenv("AURORA_TEST_STREAM_RELAY");
        if ((!relay.isEmpty() && qgetenv("AURORA_GAMESTREAM_HOST_UUID") != "host-fixture")
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

    HWND video = nullptr;
    QSize lastSize;
    QByteArray pending;
    bool receivedStop = false;
    const auto showVideo = [&] {
        video = makeChild(host, wc.lpszClassName);
        if (!video || GetParent(video) != host) {
            app.exit(74);
            return;
        }
        ShowWindow(stage, SW_HIDE);
        SetEvent(ready);
    };
    if (supervisor) {
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
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            if (mode == "ignore-stop")
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
    return status;
#else
    if (qEnvironmentVariableIsSet("AURORA_EMBEDDED_SESSION"))
        return 74;
    QTimer::singleShot(40, &app, [] {
        report("aurora-stream-setting/1\tresolution\t1920x1080");
        report("aurora-stream-setting/1\tunknown-option\tbogus");
    });
    return app.exec();
#endif
}
