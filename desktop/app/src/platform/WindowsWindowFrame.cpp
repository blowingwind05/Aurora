// SPDX-License-Identifier: GPL-3.0-only
#include "WindowsWindowFrame.h"

#include <QWindow>

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

namespace {

constexpr UINT_PTR frameSubclassId = 1;

HRESULT extendFrame(HWND window)
{
    const MARGINS margins{1, 1, 1, 1};
    return DwmExtendFrameIntoClientArea(window, &margins);
}

void fitMaximizedClient(HWND window, RECT &client)
{
    if (!IsZoomed(window) || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
        return;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof monitorInfo;
    const HMONITOR monitor = MonitorFromRect(&client, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        RECT workArea{};
        if (IntersectRect(&workArea, &client, &monitorInfo.rcWork))
            client = workArea;
    }
}

LRESULT CALLBACK windowFrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (message) {
    case WM_NCCALCSIZE: {
        // Keep the system window styles while letting QML fill the caption and frame.
        RECT &client = wParam ? reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam)->rgrc[0]
                              : *reinterpret_cast<RECT *>(lParam);
        fitMaximizedClient(window, client);
        return 0;
    }
    case WM_DWMCOMPOSITIONCHANGED:
        extendFrame(window);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, windowFrameProc, frameSubclassId);
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

bool installWindowsWindowFrame(QWindow &window)
{
    const auto handle = reinterpret_cast<HWND>(window.winId());
    constexpr LONG_PTR requiredStyles = WS_CAPTION | WS_THICKFRAME;
    RECT client{};
    if (!handle || (GetWindowLongPtrW(handle, GWL_STYLE) & requiredStyles) != requiredStyles
        || !GetClientRect(handle, &client) || !SetWindowSubclass(handle, windowFrameProc, frameSubclassId, 0)) {
        return false;
    }

    // Keep the requested client size when removing the original title bar.
    constexpr UINT flags = SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    if (SUCCEEDED(extendFrame(handle)) && SetWindowPos(handle, nullptr, 0, 0, client.right, client.bottom, flags))
        return true;

    RemoveWindowSubclass(handle, windowFrameProc, frameSubclassId);
    const MARGINS original{};
    DwmExtendFrameIntoClientArea(handle, &original);
    SetWindowPos(handle, nullptr, 0, 0, 0, 0, flags | SWP_NOSIZE);
    return false;
}
