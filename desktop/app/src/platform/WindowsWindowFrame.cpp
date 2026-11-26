// SPDX-License-Identifier: GPL-3.0-only
#include "WindowsWindowFrame.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QWindow>

#include <windows.h>
#include <dwmapi.h>

namespace {

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

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
using NativeEventResult = qintptr;
#else
using NativeEventResult = long;
#endif

class WindowsFrameFilter final : public QObject, public QAbstractNativeEventFilter
{
public:
    WindowsFrameFilter(QWindow &window, HWND handle)
        : QObject(&window), handle_(handle)
    {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, NativeEventResult *result) override
    {
        if (eventType != "windows_generic_MSG")
            return false;
        const auto &event = *static_cast<const MSG *>(message);
        if (event.hwnd != handle_)
            return false;

        if (event.message == WM_NCCALCSIZE) {
            // Let Qt observe the adjusted rectangle so its stored margins match the Win32 client.
            auto &client = event.wParam ? reinterpret_cast<NCCALCSIZE_PARAMS *>(event.lParam)->rgrc[0]
                                        : *reinterpret_cast<RECT *>(event.lParam);
            fitMaximizedClient(handle_, client);
            *result = 0;
            return true;
        }
        if (event.message == WM_DWMCOMPOSITIONCHANGED)
            extendFrame(handle_);
        return false;
    }

private:
    const HWND handle_;
};

} // namespace

bool installWindowsWindowFrame(QWindow &window)
{
    const auto handle = reinterpret_cast<HWND>(window.winId());
    constexpr LONG_PTR requiredStyles = WS_CAPTION | WS_THICKFRAME;
    RECT client{};
    if (!handle || (GetWindowLongPtrW(handle, GWL_STYLE) & requiredStyles) != requiredStyles
        || !GetClientRect(handle, &client)) {
        return false;
    }

    auto *filter = new WindowsFrameFilter(window, handle);
    // Keep the requested client size when removing the original title bar.
    constexpr UINT flags = SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    if (SUCCEEDED(extendFrame(handle)) && SetWindowPos(handle, nullptr, 0, 0, client.right, client.bottom, flags))
        return true;

    delete filter;
    const MARGINS original{};
    DwmExtendFrameIntoClientArea(handle, &original);
    SetWindowPos(handle, nullptr, 0, 0, 0, 0, flags | SWP_NOSIZE);
    return false;
}
