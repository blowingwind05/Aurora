using System;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;

namespace Aurora;

internal sealed class WindowsStreamWindowSizing
{
    private const uint SizingMessage = 0x0214;
    private const uint KeepPositionOrderAndFocus = 0x0002 | 0x0004 | 0x0010;

    private readonly Window window;
    private readonly Control viewport;
    private double aspect;

    internal WindowsStreamWindowSizing(Window window, Control viewport)
    {
        this.window = window;
        this.viewport = viewport;
        Win32Properties.AddWndProcHookCallback(window, ConstrainDrag);
    }

    internal void SetAspect(double value)
    {
        if (aspect == value) return;
        aspect = value;
        FitSoon();
    }

    internal void FitSoon()
    {
        if (aspect > 0 && window.WindowState == WindowState.Normal)
            Dispatcher.UIThread.Post(FitWindow, DispatcherPriority.Loaded);
    }

    private bool CanFit => aspect > 0 && window.IsVisible && window.WindowState == WindowState.Normal;

    private void FitWindow()
    {
        if (!CanFit || window.TryGetPlatformHandle() is not { HandleDescriptor: "HWND" } handle) return;

        window.UpdateLayout();
        if (!TryMeasure(handle.Handle, out var bounds, out var chrome, out var minimum)) return;
        var size = Fit(bounds.Size, chrome, minimum, aspect, ResizeEdge.None);
        if (size == bounds.Size) return;

        // An unusually tall or wide stream may need to grow along one axis to keep the toolbar usable.
        var workArea = window.Screens.ScreenFromWindow(window)?.WorkingArea;
        if (workArea is { } area &&
            (size.Width > Math.Max(bounds.Width, area.Right - bounds.Left) ||
             size.Height > Math.Max(bounds.Height, area.Bottom - bounds.Top)))
            return;

        SetWindowPos(handle.Handle, 0, 0, 0, size.Width, size.Height, KeepPositionOrderAndFocus);
    }

    private nint ConstrainDrag(nint hwnd, uint message, nint wParam, nint lParam, ref bool handled)
    {
        if (message != SizingMessage || lParam == 0 || !CanFit) return 0;
        var edge = (ResizeEdge)wParam.ToInt32();
        if (edge is < ResizeEdge.Left or > ResizeEdge.BottomRight ||
            !TryMeasure(hwnd, out _, out var chrome, out var minimum))
            return 0;

        var proposed = Marshal.PtrToStructure<NativeRect>(lParam);
        var size = Fit(proposed.Size, chrome, minimum, aspect, edge);
        if (edge is ResizeEdge.Left or ResizeEdge.TopLeft or ResizeEdge.BottomLeft)
            proposed.Left = proposed.Right - size.Width;
        else
            proposed.Right = proposed.Left + size.Width;
        if (edge is ResizeEdge.Top or ResizeEdge.TopLeft or ResizeEdge.TopRight)
            proposed.Top = proposed.Bottom - size.Height;
        else
            proposed.Bottom = proposed.Top + size.Height;

        Marshal.StructureToPtr(proposed, lParam, false);
        handled = true;
        return 1;
    }

    private bool TryMeasure(nint hwnd, out NativeRect bounds, out PixelSize chrome, out PixelSize minimum)
    {
        bounds = default;
        chrome = minimum = default;
        if (viewport.Bounds.Width <= 0 || viewport.Bounds.Height <= 0 ||
            !GetWindowRect(hwnd, out bounds) || !GetClientRect(hwnd, out var client))
            return false;

        // Win32 sizes the outer rectangle in pixels. The toolbar and frame occupy part of its client area.
        var scale = window.RenderScaling;
        var clientSize = window.Bounds.Size;
        var nonClientWidth = bounds.Width - client.Width;
        var nonClientHeight = bounds.Height - client.Height;
        chrome = new PixelSize(
            nonClientWidth + (int)Math.Round((clientSize.Width - viewport.Bounds.Width) * scale),
            nonClientHeight + (int)Math.Round((clientSize.Height - viewport.Bounds.Height) * scale));
        minimum = new PixelSize(
            nonClientWidth + (int)Math.Ceiling(window.MinWidth * scale),
            nonClientHeight + (int)Math.Ceiling(window.MinHeight * scale));
        return true;
    }

    private static PixelSize Fit(PixelSize proposed, PixelSize chrome, PixelSize minimum, double aspect, ResizeEdge edge)
    {
        var width = proposed.Width - chrome.Width;
        var height = proposed.Height - chrome.Height;
        var targetHeight = edge switch
        {
            ResizeEdge.None => Math.Min(height, width / aspect),
            ResizeEdge.Left or ResizeEdge.Right => width / aspect,
            ResizeEdge.Top or ResizeEdge.Bottom => height,
            // Along a corner drag, choose the closest point on the fixed-aspect resize line.
            _ => (width * aspect + height) / (aspect * aspect + 1)
        };

        targetHeight = Math.Max(targetHeight, Math.Max(minimum.Height - chrome.Height, (minimum.Width - chrome.Width) / aspect));
        return new PixelSize(
            chrome.Width + (int)Math.Round(targetHeight * aspect),
            chrome.Height + (int)Math.Round(targetHeight));
    }

    private enum ResizeEdge
    {
        None, Left, Right, Top, TopLeft, TopRight, Bottom, BottomLeft, BottomRight
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;

        internal readonly int Width => Right - Left;
        internal readonly int Height => Bottom - Top;
        internal readonly PixelSize Size => new(Width, Height);
    }

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(nint window, out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(nint window, out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(nint window, nint insertAfter, int x, int y, int width, int height, uint flags);
}
