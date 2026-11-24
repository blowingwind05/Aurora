using System;
using System.ComponentModel;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Primitives.PopupPositioning;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Styling;
using Avalonia.VisualTree;

namespace Aurora;

internal sealed partial class StreamSessionWindow : Window
{
    private const int AttachmentFailed = 74;
    private const string DisconnectIcon = "M10 17l5-5-5-5 M15 12H3 M12 3h6a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-6";
    private readonly string id = Guid.NewGuid().ToString("N");
    private CancellationTokenSource stop;
    private readonly WindowsStreamSurface surface;
    private readonly WindowsStreamWindowSizing sizing;
    private readonly TextBlock status = new()
    {
        Text = "正在连接", Classes = { "muted" }, FontSize = 12, VerticalAlignment = VerticalAlignment.Center
    };
    private readonly Ellipse statusDot = new() { Width = 7, Height = 7, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button statusButton = new() { Height = 28, Padding = new Thickness(3, 0), CornerRadius = new CornerRadius(6),
        HorizontalContentAlignment = HorizontalAlignment.Center, VerticalContentAlignment = VerticalAlignment.Center,
        Background = Brushes.Transparent, IsHitTestVisible = false, Focusable = false };
    private readonly Control statusArrow;
    private readonly DisplayTab displayTab;
    private readonly TextBlock displayCaption;
    private readonly Flyout connectionFlyout;
    private readonly StreamConnectionDetails connectionDetails = new();
    private readonly TextBlock stageTitle = new()
    {
        Text = "正在连接", FontSize = 18, FontWeight = FontWeight.SemiBold, TextAlignment = TextAlignment.Center
    };
    private readonly TextBlock stageMessage = new()
    {
        Text = "正在检查设备连接…", Classes = { "muted" }, FontSize = 13,
        TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap
    };
    private readonly ProgressBar progress = new() { IsIndeterminate = true, Width = 96, Height = 3, Margin = new Thickness(0, 6, 0, 0) };
    private readonly Button retryButton = new()
    {
        Content = "重试", Classes = { "action", "accent" }, HorizontalAlignment = HorizontalAlignment.Center, IsVisible = false
    };
    private readonly Border stage;
    private readonly Button controls = new() { Height = 32, VerticalAlignment = VerticalAlignment.Center, Classes = { "icon", "streamTool" } };
    private readonly Button disconnect = new() { Height = 32, VerticalAlignment = VerticalAlignment.Center, Classes = { "icon", "streamTool", "streamDisconnect" } };
    private readonly TextBlock controlsText = new() { Text = "控制中心", FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock disconnectText = new() { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button maximize;
    private readonly Grid toolbar;
    private readonly Grid disconnectSlot = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly Popup fullscreenControls;
    private readonly Flyout controlMenu;
    private readonly WindowsStreamControlCenter controlCenter;
    private Func<SessionPrivacyCommand, Task<SessionPrivacyState>> privacyRequest;
    private SessionPrivacyState? privacyState;
    private bool privacyBusy;
    private WindowState previousState;
    private bool connected;
    private bool allowClose;

    private sealed class DisplayTab : Decorator
    {
        internal const double Foot = 11;
        private const double TopRadius = 10;

        public override void Render(DrawingContext context)
        {
            var width = Bounds.Width;
            var height = Bounds.Height;
            var outline = new StreamGeometry();
            using (var path = outline.Open())
            {
                var top = new Size(TopRadius, TopRadius);
                var bottom = new Size(Foot, Foot);
                // Draw the inset top corners and the outward feet as one outline to avoid seams.
                path.BeginFigure(new Point(0, height), true);
                path.ArcTo(new Point(Foot, height - Foot), bottom, 0, false, SweepDirection.CounterClockwise);
                path.LineTo(new Point(Foot, TopRadius));
                path.ArcTo(new Point(Foot + TopRadius, 0), top, 0, false, SweepDirection.Clockwise);
                path.LineTo(new Point(width - Foot - TopRadius, 0));
                path.ArcTo(new Point(width - Foot, TopRadius), top, 0, false, SweepDirection.Clockwise);
                path.LineTo(new Point(width - Foot, height - Foot));
                path.ArcTo(new Point(width, height), bottom, 0, false, SweepDirection.CounterClockwise);
                path.EndFigure(true);
            }
            context.DrawGeometry(Brushes.White, null, outline);
        }
    }

    private string SessionName => "Aurora.Embedded." + id;

    internal StreamSessionWindow(string deviceName, Func<StreamSessionWindow, Task> retry, QtShellLaunch qtShell = null)
    {
        Title = deviceName + " · Aurora";
        Width = 1000;
        Height = 620;
        MinWidth = 400;
        MinHeight = 280;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        RequestedThemeVariant = ThemeVariant.Light;
        FontFamily = new FontFamily("Segoe UI, Microsoft YaHei UI");
        ExtendClientAreaToDecorationsHint = true;
        ExtendClientAreaChromeHints = ExtendClientAreaChromeHints.NoChrome;
        ExtendClientAreaTitleBarHeightHint = 44;
        Background = Brush.Parse("#F9FAFC");
        Foreground = Brush.Parse("#19283F");
        using var icon = AssetLoader.Open(new Uri("avares://Aurora/Assets/Aurora.ico"));
        Icon = new WindowIcon(icon);

        var mark = new Image { Width = 20, Height = 20 };
        using (var iconStream = AssetLoader.Open(new Uri("avares://Aurora/Assets/Aurora-Windows-32.png")))
            mark.Source = new Bitmap(iconStream);
        var name = new TextBlock
        {
            Text = deviceName, FontSize = 14, FontWeight = FontWeight.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center
        };
        ToolTip.SetTip(name, deviceName);
        statusArrow = AuroraVisual.Icon(AuroraVisual.ChevronDown, statusButton, size: 11);
        statusArrow.IsVisible = false;
        statusButton.Content = new StackPanel
        {
            Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center,
            Children = { statusDot, status, statusArrow }
        };
        connectionDetails.Update(null);
        connectionFlyout = new Flyout { Placement = PlacementMode.BottomEdgeAlignedLeft, VerticalOffset = 4, Content = connectionDetails };
        connectionFlyout.FlyoutPresenterClasses.Add("streamDetails");
        statusButton.Flyout = connectionFlyout;
        displayCaption = new TextBlock { Text = "显示器 1", FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
        displayTab = new DisplayTab
        {
            Width = 152 + 2 * DisplayTab.Foot, Height = 37,
            Margin = new Thickness(14, 0, 0, 0), VerticalAlignment = VerticalAlignment.Bottom,
            Child = new StackPanel
            {
                Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(DisplayTab.Foot + 10, 0, 0, 0), Spacing = 6,
                Children = { AuroraVisual.Icon(AuroraVisual.Monitor, size: 16), displayCaption }
            }
        };
        ToolTip.SetTip(displayTab, "当前串流显示器");
        var identity = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto,Auto"), ColumnSpacing = 7, Height = 44,
            Margin = new Thickness(10, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Left,
            Children = { mark, name, statusButton, displayTab }
        };
        mark.VerticalAlignment = statusButton.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(name, 1);
        Grid.SetColumn(statusButton, 2);
        Grid.SetColumn(displayTab, 3);

        disconnect.Click += (_, _) => Close();
        retryButton.Click += async (_, _) => await retry(this);
        SetToolContent(controls, controlsText, "控制中心", WindowsStreamControlCenter.Sliders);
        var floatingControls = new Button
        {
            Width = 40, Height = 40, Classes = { "streamTool" }, Padding = new Thickness(0), CornerRadius = new CornerRadius(13),
            HorizontalContentAlignment = HorizontalAlignment.Center, VerticalContentAlignment = VerticalAlignment.Center,
            Background = Brush.Parse("#F7F9FD"), BorderBrush = Brush.Parse("#C6D1E0"), BorderThickness = new Thickness(1),
            Foreground = Brush.Parse("#19283F")
        };
        floatingControls.Content = AuroraVisual.Icon(WindowsStreamControlCenter.Sliders, floatingControls, size: 18);
        AuroraVisual.Label(floatingControls, "控制中心");
        controlCenter = new WindowsStreamControlCenter(() =>
        {
            controlMenu.Hide();
            ToggleFullscreen();
        }, () =>
        {
            controlMenu.Hide();
            connectionFlyout.ShowAt(floatingControls);
        }, () =>
        {
            controlMenu.Hide();
            Close();
        }, () => UpdatePrivacy(SessionPrivacyCommand.Status), () => UpdatePrivacy(privacyState switch
        {
            SessionPrivacyState.Off => SessionPrivacyCommand.Enable,
            SessionPrivacyState.On => SessionPrivacyCommand.Disable,
            _ => SessionPrivacyCommand.Status
        }));
        controlMenu = new Flyout { Placement = PlacementMode.BottomEdgeAlignedRight, Content = controlCenter };
        controlMenu.FlyoutPresenterClasses.Add("streamControlCenter");
        controls.Flyout = floatingControls.Flyout = controlMenu;
        controlMenu.Opening += (_, _) =>
        {
            connectionFlyout.Hide();
            var floating = WindowState == WindowState.FullScreen;
            if (!floating && toolbar.Bounds.Width < 530 &&
                controls.TranslatePoint(new Point(controls.Bounds.Width, 0), toolbar) is { } right)
                controlMenu.HorizontalOffset = toolbar.Bounds.Width - right.X - 8;
            else
                controlMenu.HorizontalOffset = 0;
            (floating ? floatingControls : controls).Classes.Add("flyoutOpen");
            controlCenter.Home();
        };
        controlMenu.Closed += (_, _) =>
        {
            controls.Classes.Remove("flyoutOpen");
            floatingControls.Classes.Remove("flyoutOpen");
            controlCenter.SaveBitrate();
        };
        connectionFlyout.Opening += (_, _) => controlMenu.Hide();
        WireQtSessionPopup(qtShell, floatingControls);
        ShowPrivacyState();

        var minimize = CaptionButton("最小化", AuroraVisual.Minus, () => WindowState = WindowState.Minimized);
        maximize = CaptionButton("最大化", AuroraVisual.Square, ToggleWindowSize);
        var close = CaptionButton("关闭窗口", AuroraVisual.Close, Close);
        close.Classes.Add("close");
        disconnectSlot.Children.Add(disconnect);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal, Height = 44, VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 4, 0), Spacing = 3,
            Children = { controls, disconnectSlot, minimize, maximize, close }
        };

        toolbar = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto"), Height = 44,
            Background = Brush.Parse("#EDF1F8"), Children = { identity, buttons }
        };
        Grid.SetColumn(buttons, 1);
        toolbar.PointerPressed += (_, args) =>
        {
            for (var source = args.Source as Visual; source != null && source != toolbar;
                 source = source.GetVisualParent())
                if (source is Button) return;
            if (!args.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            if (args.ClickCount == 2 && WindowState != WindowState.FullScreen) ToggleWindowSize();
            else if (args.ClickCount == 1 && WindowState != WindowState.FullScreen) BeginMoveDrag(args);
        };

        stage = new Border
        {
            Background = Brush.Parse("#F9FAFC"), Padding = new Thickness(24),
            Child = new StackPanel
            {
                Spacing = 8, MaxWidth = 420, HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Children = { AuroraVisual.Icon(AuroraVisual.Monitor, size: 26), stageTitle, stageMessage, progress, retryButton }
            }
        };
        surface = new WindowsStreamSurface(SessionName) { IsVisible = false };
        var viewer = new Grid { Background = Brush.Parse("#151D2A"), Children = { surface, stage } };
        // A native popup stays above the Win32 video surface when the title bar is hidden.
        fullscreenControls = new Popup
        {
            PlacementTarget = viewer, Placement = PlacementMode.AnchorAndGravity,
            PlacementAnchor = PopupAnchor.TopRight, PlacementGravity = PopupGravity.BottomLeft,
            HorizontalOffset = -14, VerticalOffset = 14, Child = floatingControls
        };
        var content = new Grid
        {
            RowDefinitions = new RowDefinitions("Auto,*"), Children = { toolbar, viewer, fullscreenControls }
        };
        Grid.SetRow(viewer, 1);
        var frame = new Border
        {
            Background = Brushes.Transparent, BorderBrush = Brush.Parse("#C6D1E0"),
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(13),
            ClipToBounds = true, Child = content
        };
        frame[!MarginProperty] = this[!OffScreenMarginProperty];
        Content = frame;
        sizing = new WindowsStreamWindowSizing(this, viewer);

        UpdateWindowActions();
        ResizeToolbar(Width);
        SizeChanged += (_, args) =>
        {
            ResizeToolbar(args.NewSize.Width);
            controlCenter.Fit(args.NewSize.Height);
        };
        controlCenter.Fit(Height);
        PropertyChanged += (_, args) =>
        {
            if (args.Property != WindowStateProperty) return;
            CloseQtSessionPopup();
            frame.CornerRadius = new CornerRadius(WindowState == WindowState.Normal ? 13 : 0);
            frame.BorderThickness = new Thickness(WindowState == WindowState.Normal ? 1 : 0);
            UpdateWindowActions();
            sizing.FitSoon();
        };
        KeyDown += (_, args) =>
        {
            if (args.Key != Key.Escape || WindowState != WindowState.FullScreen) return;
            ToggleFullscreen();
            args.Handled = true;
        };
        Closed += (_, _) => fullscreenControls.IsOpen = false;
        Closing += (_, args) =>
        {
            if (allowClose) return;
            args.Cancel = true;
            if (stop.IsCancellationRequested) return;
            SetStatus(connected ? "正在断开" : "正在取消", "#526176");
            stageMessage.Text = status.Text + "…";
            CloseQtSessionPopup();
            stop.Cancel();
        };
    }

    private void ResizeToolbar(double width)
    {
        var compact = width < 760;
        var narrow = width < 490;
        controlsText.IsVisible = !compact;
        controls.Width = compact ? 32 : 94;
        displayTab.Width = (compact ? 36 : 152) + 2 * DisplayTab.Foot;
        displayTab.Margin = new Thickness(compact ? 0 : 14, 0, 0, 0);
        displayCaption.IsVisible = !compact;
        disconnectText.IsVisible = status.IsVisible = !narrow;
        disconnect.Width = disconnectSlot.Width = narrow ? 32 : 60;
        statusButton.Width = narrow ? 25 : double.NaN;
        statusButton.MinWidth = narrow ? 0 : 84;
    }

    private static void SetToolContent(Button button, TextBlock text, string label, string geometry)
    {
        text.Text = label;
        var icon = AuroraVisual.Icon(geometry, button, size: 16);
        if (button.Content is StackPanel current)
            current.Children[0] = icon;
        else
        {
            button.Padding = new Thickness(0);
            button.HorizontalContentAlignment = HorizontalAlignment.Center;
            button.VerticalContentAlignment = VerticalAlignment.Center;
            button.Content = new StackPanel
            {
                Orientation = Orientation.Horizontal, Spacing = 5, Children = { icon, text }
            };
        }
        AuroraVisual.Label(button, label);
    }

    private void SetStatus(string text, string color)
    {
        status.Text = text;
        statusDot.Fill = Brush.Parse(color);
        statusButton.IsHitTestVisible = statusButton.Focusable = statusArrow.IsVisible = connected;
        ToolTip.SetTip(statusButton, connected ? "连接详情" : text);
        AutomationProperties.SetName(statusButton, connected ? text + "，查看连接详情" : text);
    }

    private void ShowPrivacyState(bool changing = false)
    {
        var statusText = privacyBusy ? (changing ? "正在切换隐私屏保…" : "正在检查隐私屏保…") : privacyState switch
        {
            SessionPrivacyState.On => "隐私屏保已开启",
            SessionPrivacyState.Off => "隐私屏保已关闭",
            SessionPrivacyState.Unavailable => "当前会话不支持隐私屏保",
            _ => "隐私屏保状态未确认"
        };
        var actionText = privacyState switch
        {
            SessionPrivacyState.On => "关闭隐私屏保",
            SessionPrivacyState.Off => "开启隐私屏保",
            _ => "重新检查"
        };
        var available = privacyRequest != null && privacyState != SessionPrivacyState.Unavailable;
        controlCenter.SetPrivacy(statusText, actionText, available, available && !privacyBusy);
    }

    private async Task UpdatePrivacy(SessionPrivacyCommand command)
    {
        var request = privacyRequest;
        if (!connected || stop.IsCancellationRequested || privacyBusy || request == null) return;
        privacyBusy = true;
        ShowPrivacyState(changing: command != SessionPrivacyCommand.Status);
        SessionPrivacyState? result = null;
        try { result = await request(command); }
        catch (Exception error) when (error is InvalidOperationException or OperationCanceledException or Win32Exception) { }
        finally
        {
            if (ReferenceEquals(request, privacyRequest))
            {
                privacyBusy = false;
                privacyState = result;
                ShowPrivacyState();
            }
        }
    }

    private static Button CaptionButton(string label, string geometry, Action action, int width = 42)
    {
        var button = new Button { Width = width, Height = 32, VerticalAlignment = VerticalAlignment.Center, Classes = { "caption" } };
        SetCaption(button, label, geometry);
        if (action != null) button.Click += (_, _) => action();
        return button;
    }

    private static void SetCaption(Button button, string label, string geometry)
    {
        button.Content = AuroraVisual.Icon(geometry, button, size: 16);
        AuroraVisual.Label(button, label);
    }

    private void UpdateWindowActions()
    {
        var full = WindowState == WindowState.FullScreen;
        var maximized = WindowState == WindowState.Maximized;
        var fullLabel = full ? "退出全屏" : "全屏";
        SetCaption(maximize, full ? fullLabel : maximized ? "还原窗口" : "最大化",
            full || maximized ? AuroraVisual.RestoreWindow : AuroraVisual.Square);
        controlCenter.SetFullscreen(full);
        var showFloatingControls = full && connected;
        if (fullscreenControls.IsOpen && !showFloatingControls) controlMenu.Hide();
        fullscreenControls.IsOpen = showFloatingControls;
        toolbar.IsVisible = !showFloatingControls;
    }

    internal void ShowConnecting(CancellationTokenSource cancellation)
    {
        CloseQtSessionPopup();
        qtConnectionSample = null;
        stop = cancellation;
        connected = allowClose = false;
        sizing.SetAspect(0);
        controlMenu.Hide();
        connectionFlyout.Hide();
        connectionDetails.Update(null);
        privacyState = SessionPrivacyState.Unavailable;
        ShowPrivacyState();
        surface.IsVisible = false;
        stage.IsVisible = progress.IsVisible = true;
        retryButton.IsVisible = false;
        controls.IsEnabled = false;
        SetStatus("连接中", "#087CF5");
        disconnect.IsVisible = true;
        SetToolContent(disconnect, disconnectText, "取消", AuroraVisual.Close);
        UpdateWindowActions();
        stageTitle.Text = "正在连接…";
        stageMessage.Text = "正在检查设备连接…";
        if (!IsVisible)
        {
            if (StreamPreferences.Get("display-mode") is "fullscreen" or "borderless") ToggleFullscreen();
            Show();
        }
    }

    internal void ShowProgress(string message)
    {
        if (!stop.IsCancellationRequested) stageMessage.Text = message;
    }

    internal void ShowFailure(string message)
    {
        CloseQtSessionPopup();
        connected = false;
        controlMenu.Hide();
        sizing.SetAspect(0);
        connectionFlyout.Hide();
        allowClose = true;
        surface.IsVisible = false;
        stage.IsVisible = true;
        progress.IsVisible = false;
        retryButton.IsVisible = true;
        controls.IsEnabled = false;
        SetStatus("连接失败", "#AD384E");
        disconnect.IsVisible = false;
        UpdateWindowActions();
        stageTitle.Text = "连接失败";
        stageMessage.Text = message;
    }

    private void ShowVideo()
    {
        connected = true;
        surface.IsVisible = true;
        stage.IsVisible = false;
        controls.IsEnabled = true;
        SetStatus("已连接", "#087CF5");
        SetToolContent(disconnect, disconnectText, "断开", DisconnectIcon);
        UpdateWindowActions();
    }

    private void UpdateStreamAspect(StreamConnectionSample sample)
    {
        if (sample.Width > 0 && sample.Height > 0)
            sizing.SetAspect((double)sample.Width / sample.Height);
    }

    internal void Finish()
    {
        allowClose = true;
        Close();
    }

    internal static async Task<bool> IsSupported(string runtime, CancellationToken cancellation)
    {
        if (System.IO.Path.GetFileNameWithoutExtension(runtime) != "AuroraStream") return false;
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

    internal async Task<(bool AttachmentFailed, int? ExitCode)> Run(string runtime, StreamHost route, CancellationToken cancellation)
    {
        try
        {
            ShowProgress("正在建立串流…");
            cancellation.ThrowIfCancellationRequested();
            var hwnd = await surface.Handle.WaitAsync(TimeSpan.FromSeconds(3), cancellation);
            if (hwnd == 0) return (true, null);

            using var ready = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + SessionName + ".ready");
            using var disconnectSignal = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + SessionName + ".stop");
            using var fullscreenSignal = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\" + SessionName + ".fullscreen");
            var invitation = FormattableString.Invariant($"{hwnd.ToInt64()}:{Environment.ProcessId}:{id}");
            cancellation.ThrowIfCancellationRequested();
            using var process = Upstream.LaunchMoonlight(runtime, route, pair: false, invitation, readConnectionDetails: true);
            var qtStageReady = 0;
            var showingQtStage = false;
            StreamConnectionSample latest = null, displayed = null;
            process.OutputDataReceived += (_, data) =>
            {
                if (data.Data == "aurora-connection-stage/1,qt")
                    Volatile.Write(ref qtStageReady, 1);
                else if (StreamConnectionSample.Read(data.Data) is { } sample)
                    Volatile.Write(ref latest, sample);
            };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            using var privacyLifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            var exit = process.WaitForExitAsync();
            if (route.SupportsSessionPrivacy)
            {
                var token = privacyLifetime.Token;
                privacyRequest = command => Upstream.ChangeSessionPrivacy(runtime, route, command, token);
                privacyState = null;
                ShowPrivacyState();
            }
            try
            {
                while (!exit.IsCompleted)
                {
                    cancellation.ThrowIfCancellationRequested();
                    if (!connected && ready.WaitOne(0)) ShowVideo();
                    if (!connected && !showingQtStage && Volatile.Read(ref qtStageReady) != 0)
                    {
                        showingQtStage = true;
                        surface.IsVisible = true;
                        stage.IsVisible = false;
                    }
                    var sample = Volatile.Read(ref latest);
                    if (connected && sample != displayed && sample != null)
                    {
                        displayed = sample;
                        connectionDetails.Update(sample);
                        qtConnectionSample = sample;
                        publishQtSessionPopup?.Invoke();
                        UpdateStreamAspect(sample);
                        SetStatus(sample.Poor ? "网络较差" : "已连接", sample.Poor ? "#946317" : "#087CF5");
                    }
                    if (fullscreenSignal.WaitOne(0)) ToggleFullscreen();
                    await Task.WhenAny(exit, Task.Delay(80, cancellation));
                }
                await exit;
                return process.ExitCode == AttachmentFailed && !connected && !ready.WaitOne(0)
                    ? (true, null)
                    : (false, process.ExitCode);
            }
            finally
            {
                if (showingQtStage && !connected)
                {
                    surface.IsVisible = false;
                    stage.IsVisible = true;
                }
                CloseQtSessionPopup();
                qtConnectionSample = null;
                connectionFlyout.Hide();
                connectionDetails.Update(null);
                privacyRequest = null;
                privacyBusy = false;
                privacyState = SessionPrivacyState.Unavailable;
                ShowPrivacyState();
                privacyLifetime.Cancel();
                if (!exit.IsCompleted)
                {
                    SetStatus("正在断开", "#526176");
                    controls.IsEnabled = false;
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
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { return (false, connected ? 0 : null); }
    }

    private void ToggleWindowSize()
    {
        if (WindowState == WindowState.FullScreen)
        {
            ToggleFullscreen();
            return;
        }

        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
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
