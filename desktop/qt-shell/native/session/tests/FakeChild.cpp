// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QSize>
#include <QTimer>

#include <cstdio>
#include <windows.h>

namespace {

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

void report(const char *line)
{
    std::puts(line);
    std::fflush(stdout);
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
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
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
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
                video = makeChild(host, wc.lpszClassName);
                if (!video || GetParent(video) != host) {
                    app.exit(74);
                    return;
                }
                ShowWindow(stage, SW_HIDE);
                SetEvent(ready);
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
}
