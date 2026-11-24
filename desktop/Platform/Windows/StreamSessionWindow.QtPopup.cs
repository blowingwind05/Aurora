using System;
using System.ComponentModel;
using System.IO;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;

namespace Aurora;

internal sealed partial class StreamSessionWindow
{
    private QtShellBridge qtSessionPopup;
    private Button qtSessionAnchor;
    private StreamConnectionSample qtConnectionSample;
    private Action publishQtSessionPopup;

    private void WireQtSessionPopup(QtShellLaunch shell, Button floatingControls)
    {
        if (shell == null || Environment.GetEnvironmentVariable("AURORA_QT_SESSION") != "1")
            return;

        var launch = shell with
        {
            EntryPoint = Path.Combine(Path.GetDirectoryName(shell.EntryPoint)!, "WindowsSessionPopup.qml")
        };
        if (!File.Exists(launch.EntryPoint)) return;

        controls.Flyout = floatingControls.Flyout = null;
        statusButton.Flyout = null;
        controls.Click += async (_, _) => await OpenQtSessionPopup(launch, controls, "controls");
        floatingControls.Click += async (_, _) => await OpenQtSessionPopup(launch, floatingControls, "controls");
        statusButton.Click += async (_, _) => await OpenQtSessionPopup(launch, statusButton, "connection");
        PositionChanged += (_, _) => CloseQtSessionPopup();
        SizeChanged += (_, _) => CloseQtSessionPopup();
        Closed += (_, _) => CloseQtSessionPopup();
    }

    private void CloseQtSessionPopup()
    {
        var popup = qtSessionPopup;
        qtSessionPopup = null;
        qtSessionAnchor = null;
        publishQtSessionPopup = null;
        popup?.Dispose();
    }

    private async Task OpenQtSessionPopup(QtShellLaunch launch, Button anchor, string panelKind)
    {
        if (!connected || stop.IsCancellationRequested) return;
        if (qtSessionPopup != null)
        {
            var sameAnchor = qtSessionAnchor == anchor;
            CloseQtSessionPopup();
            if (sameAnchor)
            {
                anchor.Focus();
                return;
            }
        }

        void ShowNative()
        {
            if (connected && IsVisible && !stop.IsCancellationRequested && anchor.IsVisible)
                (panelKind == "controls" ? controlMenu : connectionFlyout).ShowAt(anchor);
        }

        if (TryGetPlatformHandle() is not { HandleDescriptor: "HWND" } owner)
        {
            ShowNative();
            return;
        }

        var metricsPanel = panelKind == "connection";
        var low = this.PointToScreen(new Point(8, 0));
        var high = this.PointToScreen(new Point(ClientSize.Width - 8, ClientSize.Height - 8));
        var anchorLeft = anchor.PointToScreen(new Point(0, anchor.Bounds.Height + 4));
        var anchorRight = anchor.PointToScreen(new Point(anchor.Bounds.Width, 0)).X;
        var popupWidth = (int)Math.Round((metricsPanel ? 292 : 288) * RenderScaling);
        var x = metricsPanel ? (ClientSize.Width < 490 ? low.X : anchorLeft.X)
            : (ClientSize.Width < 530 ? high.X : anchorRight) - popupWidth;
        x = Math.Clamp(x, low.X, Math.Max(low.X, high.X - popupWidth));
        var heightLimit = (int)Math.Floor((high.Y - anchorLeft.Y) / RenderScaling);
        if (heightLimit < 80)
        {
            ShowNative();
            return;
        }

        QtShellBridge popup = null;
        var shown = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            popup = QtShellBridge.StartWindowsSession(launch, owner.Handle, request =>
                Dispatcher.UIThread.Post(() =>
                {
                    if (qtSessionPopup != popup || !connected || stop.IsCancellationRequested) return;
                    switch (request.Action)
                    {
                        case "session-shown":
                            shown.TrySetResult();
                            break;
                        case "session-fullscreen" when !metricsPanel && request.FullScreen is bool full:
                            CloseQtSessionPopup();
                            if (full != (WindowState == WindowState.FullScreen))
                                ToggleFullscreen();
                            break;
                        case "session-native-settings" when !metricsPanel:
                            CloseQtSessionPopup();
                            ShowNative();
                            break;
                    }
                }));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or ArgumentException)
        {
            ShowNative();
            return;
        }

        qtSessionPopup = popup;
        qtSessionAnchor = anchor;
        publishQtSessionPopup = () =>
        {
            var sample = qtConnectionSample;
            popup.Publish(new WindowsQtSessionState(panelKind, x, anchorLeft.Y, heightLimit,
                WindowState == WindowState.FullScreen, sample?.Poor == true,
                new WindowsQtConnectionMetrics(sample?.RoundTrip > 0 ? $"{sample.RoundTrip} ms" : "")));
        };
        publishQtSessionPopup();

        try
        {
            var result = await Task.WhenAny(popup.Completion, shown.Task, Task.Delay(TimeSpan.FromSeconds(3)));
            if (qtSessionPopup != popup) return;
            if (result != shown.Task && !shown.Task.IsCompleted)
            {
                CloseQtSessionPopup();
                ShowNative();
                return;
            }
            await popup.Completion;
        }
        finally
        {
            if (qtSessionPopup == popup)
            {
                CloseQtSessionPopup();
                if (IsActive && anchor.IsVisible) anchor.Focus();
            }
        }
    }
}
