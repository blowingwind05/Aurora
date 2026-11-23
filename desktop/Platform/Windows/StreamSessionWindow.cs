using System;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform;
using Avalonia.Styling;

namespace Aurora;

internal sealed class StreamSessionWindow : Window
{
    private const int AttachmentFailed = 74;
    private readonly string id = Guid.NewGuid().ToString("N");
    private readonly WindowsStreamSurface surface;
    private readonly TextBlock status = new() { Text = "正在连接…", VerticalAlignment = VerticalAlignment.Center };
    private readonly Button fullscreen = new() { Content = "全屏", Padding = new Thickness(10, 4) };
    private readonly Button disconnect = new() { Content = "断开", Padding = new Thickness(10, 4) };
    private WindowState previousState;
    private bool allowClose;

    private string SessionName => "Aurora.Embedded." + id;

    internal StreamSessionWindow(string deviceName)
    {
        Title = "Aurora 串流";
        Width = 1000;
        Height = 620;
        MinWidth = 400;
        MinHeight = 280;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        RequestedThemeVariant = ThemeVariant.Light;
        Background = Brushes.Black;
        using var icon = AssetLoader.Open(new Uri("avares://Aurora/Assets/Aurora.ico"));
        Icon = new WindowIcon(icon);
        fullscreen.Classes.Add("action");
        disconnect.Classes.Add("action");

        surface = new WindowsStreamSurface(SessionName);
        var name = new TextBlock
        {
            Text = deviceName,
            FontWeight = FontWeight.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis,
            VerticalAlignment = VerticalAlignment.Center
        };
        var toolbar = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto,Auto"),
            Margin = new Thickness(12, 0),
            MinHeight = 40,
            ColumnSpacing = 10,
            Children = { name, status, fullscreen, disconnect }
        };
        Grid.SetColumn(status, 1);
        Grid.SetColumn(fullscreen, 2);
        Grid.SetColumn(disconnect, 3);

        var bar = new Border { Background = new SolidColorBrush(Color.Parse("#EDF1F8")), Child = toolbar };
        var content = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), Children = { bar, surface } };
        Grid.SetRow(surface, 1);
        Content = content;

        ToolTip.SetTip(status, "单击画面控制远端。按 Ctrl+Alt+Shift+Z 释放鼠标。");
        fullscreen.Click += (_, _) => ToggleFullscreen();
        disconnect.Click += (_, _) => Close();
        KeyDown += (_, args) =>
        {
            if (args.Key != Key.Escape || WindowState != WindowState.FullScreen) return;
            ToggleFullscreen();
            args.Handled = true;
        };
        PropertyChanged += (_, args) =>
        {
            if (args.Property == WindowStateProperty) fullscreen.Content = WindowState == WindowState.FullScreen ? "退出全屏" : "全屏";
        };
    }

    internal static async Task<bool> IsSupported(string runtime, CancellationToken cancellation)
    {
        if (Path.GetFileNameWithoutExtension(runtime) != "AuroraStream") return false;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(4));
        try
        {
            var (exitCode, output) = await Upstream.RunMoonlight(runtime, new[] { "--aurora-embedded-info" }, timeout.Token);
            return exitCode == 0 && output.Trim() == "aurora-embedded/1";
        }
        catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { return false; }
        catch (Win32Exception) { return false; }
    }

    internal async Task<(bool Fallback, int? ExitCode)> Run(string runtime, StreamHost route, CancellationToken cancellation)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        bool connected = false;
        Closing += (_, args) =>
        {
            if (allowClose) return;
            args.Cancel = true;
            stop.Cancel();
        };

        try
        {
            stop.Token.ThrowIfCancellationRequested();
            Show();
            if (StreamPreferences.Get("display-mode") is "fullscreen" or "borderless") ToggleFullscreen();
            var hwnd = await surface.Handle.WaitAsync(TimeSpan.FromSeconds(3), stop.Token);
            if (hwnd == 0) return (true, null);

            using var ready = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + SessionName + ".ready");
            using var disconnectSignal = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + SessionName + ".stop");
            using var fullscreenSignal = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\" + SessionName + ".fullscreen");
            var invitation = FormattableString.Invariant($"{hwnd.ToInt64()}:{Environment.ProcessId}:{id}");
            stop.Token.ThrowIfCancellationRequested();
            using var process = Upstream.LaunchMoonlight(runtime, route, pair: false, invitation);
            var exit = process.WaitForExitAsync();
            try
            {
                while (!exit.IsCompleted)
                {
                    stop.Token.ThrowIfCancellationRequested();
                    if (!connected && ready.WaitOne(0))
                    {
                        connected = true;
                        status.Text = "已连接";
                    }
                    if (fullscreenSignal.WaitOne(0)) ToggleFullscreen();
                    await Task.WhenAny(exit, Task.Delay(80, stop.Token));
                }
                await exit;
                return process.ExitCode == AttachmentFailed && !connected && !ready.WaitOne(0)
                    ? (true, null)
                    : (false, process.ExitCode);
            }
            finally
            {
                if (!exit.IsCompleted)
                {
                    status.Text = "正在断开…";
                    fullscreen.IsEnabled = disconnect.IsEnabled = false;
                    disconnectSignal.Set();
                    if (await Task.WhenAny(exit, Task.Delay(TimeSpan.FromSeconds(6))) != exit)
                    {
                        Upstream.StopProcess(process);
                        await Task.WhenAny(exit, Task.Delay(TimeSpan.FromSeconds(2)));
                    }
                }
            }
        }
        catch (TimeoutException) { return (true, null); }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { return (false, connected ? 0 : null); }
        finally
        {
            allowClose = true;
            Close();
        }
    }

    private void ToggleFullscreen()
    {
        if (WindowState == WindowState.FullScreen)
        {
            WindowState = previousState;
            return;
        }
        previousState = WindowState == WindowState.Maximized ? WindowState.Maximized : WindowState.Normal;
        WindowState = WindowState.FullScreen;
    }
}
