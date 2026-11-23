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

internal sealed class StreamSessionWindow : Window
{
    private const int AttachmentFailed = 74;
    private const string MaximizeIcon = "M8 3H5a2 2 0 0 0-2 2v3 M16 3h3a2 2 0 0 1 2 2v3 M21 16v3a2 2 0 0 1-2 2h-3 M8 21H5a2 2 0 0 1-2-2v-3";
    private const string DisconnectIcon = "M10 17l5-5-5-5 M15 12H3 M12 3h6a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-6";
    private readonly string id = Guid.NewGuid().ToString("N");
    private CancellationTokenSource stop;
    private readonly WindowsStreamSurface surface;
    private readonly TextBlock status = new()
    {
        Text = "正在连接", Classes = { "muted" }, FontSize = 12, VerticalAlignment = VerticalAlignment.Center
    };
    private readonly Ellipse statusDot = new() { Width = 7, Height = 7, VerticalAlignment = VerticalAlignment.Center };
    private readonly StackPanel statusGroup;
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
    private readonly Button fullscreen = new() { Height = 32, Classes = { "icon", "streamTool" } };
    private readonly Button controls = new() { Height = 32, Classes = { "icon", "streamTool" } };
    private readonly Button disconnect = new() { Height = 32, Classes = { "icon", "streamTool", "streamDisconnect" } };
    private readonly TextBlock fullscreenText = new() { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock controlsText = new() { Text = "控制中心", FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock disconnectText = new() { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button maximize;
    private readonly Grid toolbar;
    private readonly Grid disconnectSlot = new();
    private readonly Popup fullscreenControls;
    private readonly MenuFlyout controlMenu;
    private readonly MenuItem fullscreenItem = new();
    private readonly MenuItem statsItem = new() { Header = "切换性能统计", IsVisible = false };
    private EventWaitHandle statsSignal;
    private readonly MenuItem privacyStatus = new() { IsEnabled = false };
    private readonly MenuItem privacyAction = new() { StaysOpenOnClick = true };
    private Func<SessionPrivacyCommand, Task<SessionPrivacyState>> privacyRequest;
    private SessionPrivacyState? privacyState;
    private bool privacyBusy;
    private WindowState previousState;
    private bool connected;
    private bool allowClose;

    private string SessionName => "Aurora.Embedded." + id;

    internal StreamSessionWindow(string deviceName, Func<StreamSessionWindow, Task> retry)
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
        statusGroup = new StackPanel
        {
            Width = 76, Orientation = Orientation.Horizontal, Spacing = 6,
            VerticalAlignment = VerticalAlignment.Center, Children = { statusDot, status }
        };
        var identity = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), ColumnSpacing = 9,
            Margin = new Thickness(13, 0, 10, 0), VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Left,
            Children = { mark, name, statusGroup }
        };
        Grid.SetColumn(name, 1);
        Grid.SetColumn(statusGroup, 2);

        fullscreen.Click += (_, _) => ToggleFullscreen();
        disconnect.Click += (_, _) => Close();
        retryButton.Click += async (_, _) => await retry(this);
        SetToolContent(fullscreen, fullscreenText, "全屏", MaximizeIcon);
        SetToolContent(controls, controlsText, "控制中心", AuroraVisual.Gear);
        var mouseHelp = new MenuItem
        {
            Header = "Ctrl+Alt+Shift+M 切换鼠标模式", IsEnabled = false
        };
        fullscreenItem.Click += (_, _) => ToggleFullscreen();
        statsItem.Click += (_, _) => statsSignal?.Set();
        var security = new MenuItem { Header = "安全", Items = { privacyStatus, privacyAction } };
        security.SubmenuOpened += async (_, _) => await UpdatePrivacy(SessionPrivacyCommand.Status);
        privacyAction.Click += async (_, _) => await UpdatePrivacy(privacyState switch
        {
            SessionPrivacyState.Off => SessionPrivacyCommand.Enable,
            SessionPrivacyState.On => SessionPrivacyCommand.Disable,
            _ => SessionPrivacyCommand.Status
        });
        var disconnectItem = new MenuItem { Header = "断开连接" };
        disconnectItem.Click += (_, _) => Close();
        controlMenu = new MenuFlyout
        {
            Placement = PlacementMode.BottomEdgeAlignedRight,
            Items = { fullscreenItem, statsItem, security, mouseHelp, new Separator(), disconnectItem }
        };
        controls.Flyout = controlMenu;
        var floatingControls = new Button
        {
            Width = 40, Height = 40, Padding = new Thickness(0), CornerRadius = new CornerRadius(13),
            HorizontalContentAlignment = HorizontalAlignment.Center, VerticalContentAlignment = VerticalAlignment.Center,
            Background = Brush.Parse("#F7F9FD"), BorderBrush = Brush.Parse("#C6D1E0"), BorderThickness = new Thickness(1),
            Foreground = Brush.Parse("#19283F"), Flyout = controlMenu
        };
        floatingControls.Content = AuroraVisual.Icon(AuroraVisual.Gear, floatingControls, size: 18);
        AuroraVisual.Label(floatingControls, "控制中心");
        ShowPrivacyState();

        var minimize = CaptionButton("最小化", AuroraVisual.Minus, () => WindowState = WindowState.Minimized);
        maximize = CaptionButton("最大化", AuroraVisual.Square, ToggleWindowSize);
        var close = CaptionButton("关闭窗口", AuroraVisual.Close, Close);
        close.Classes.Add("close");
        disconnectSlot.Children.Add(disconnect);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 4, 0), Spacing = 3,
            Children = { fullscreen, controls, disconnectSlot, minimize, maximize, close }
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

        UpdateWindowActions();
        ResizeToolbar(Width);
        SizeChanged += (_, args) => ResizeToolbar(args.NewSize.Width);
        PropertyChanged += (_, args) =>
        {
            if (args.Property != WindowStateProperty) return;
            frame.CornerRadius = new CornerRadius(WindowState == WindowState.Normal ? 13 : 0);
            frame.BorderThickness = new Thickness(WindowState == WindowState.Normal ? 1 : 0);
            UpdateWindowActions();
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
            stop.Cancel();
        };
    }

    private void ResizeToolbar(double width)
    {
        var compact = width < 760;
        var narrow = width < 490;
        fullscreenText.IsVisible = controlsText.IsVisible = !compact;
        fullscreen.Width = compact ? 32 : 86;
        controls.Width = compact ? 32 : 92;
        disconnectText.IsVisible = status.IsVisible = !narrow;
        disconnect.Width = disconnectSlot.Width = narrow ? 32 : 60;
        statusGroup.Width = narrow ? 14 : 76;
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
        ToolTip.SetTip(statusGroup, text);
        AutomationProperties.SetName(statusGroup, text);
    }

    private void ShowPrivacyState()
    {
        privacyStatus.Header = privacyBusy ? "正在检查隐私屏保…" : privacyState switch
        {
            SessionPrivacyState.On => "隐私屏保已开启",
            SessionPrivacyState.Off => "隐私屏保已关闭",
            SessionPrivacyState.Unavailable => "当前会话不支持隐私屏保",
            _ => "隐私屏保状态未确认"
        };
        privacyAction.Header = privacyState switch
        {
            SessionPrivacyState.On => "关闭隐私屏保",
            SessionPrivacyState.Off => "开启隐私屏保",
            _ => "重新检查"
        };
        privacyAction.IsVisible = privacyRequest != null && privacyState != SessionPrivacyState.Unavailable;
        privacyAction.IsEnabled = privacyAction.IsVisible && !privacyBusy;
    }

    private async Task UpdatePrivacy(SessionPrivacyCommand command)
    {
        var request = privacyRequest;
        if (!connected || stop.IsCancellationRequested || privacyBusy || request == null) return;
        privacyBusy = true;
        ShowPrivacyState();
        if (command != SessionPrivacyCommand.Status) privacyStatus.Header = "正在切换隐私屏保…";
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
        var button = new Button { Width = width, Classes = { "caption" } };
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
        fullscreen.IsVisible = !full;
        SetCaption(maximize, full ? fullLabel : maximized ? "还原窗口" : "最大化",
            full || maximized ? AuroraVisual.RestoreWindow : AuroraVisual.Square);
        fullscreenItem.Header = fullLabel;
        fullscreenItem.Icon = AuroraVisual.Icon(full ? AuroraVisual.RestoreWindow : MaximizeIcon, size: 16);
        var showFloatingControls = full && connected;
        if (fullscreenControls.IsOpen && !showFloatingControls) controlMenu.Hide();
        fullscreenControls.IsOpen = showFloatingControls;
        toolbar.IsVisible = !showFloatingControls;
    }

    internal void ShowConnecting(CancellationTokenSource cancellation)
    {
        stop = cancellation;
        connected = allowClose = false;
        privacyState = SessionPrivacyState.Unavailable;
        ShowPrivacyState();
        surface.IsVisible = false;
        stage.IsVisible = progress.IsVisible = true;
        retryButton.IsVisible = false;
        controls.IsEnabled = fullscreen.IsEnabled = fullscreenItem.IsEnabled = false;
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
        connected = false;
        allowClose = true;
        surface.IsVisible = false;
        stage.IsVisible = true;
        progress.IsVisible = false;
        retryButton.IsVisible = true;
        controls.IsEnabled = fullscreen.IsEnabled = fullscreenItem.IsEnabled = false;
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
        controls.IsEnabled = fullscreen.IsEnabled = fullscreenItem.IsEnabled = true;
        SetStatus("已连接", "#087CF5");
        SetToolContent(disconnect, disconnectText, "断开", DisconnectIcon);
        UpdateWindowActions();
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
            using var toggleStats = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\" + SessionName + ".stats");
            using var statsAvailable = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + SessionName + ".stats-available");
            var invitation = FormattableString.Invariant($"{hwnd.ToInt64()}:{Environment.ProcessId}:{id}");
            cancellation.ThrowIfCancellationRequested();
            using var process = Upstream.LaunchMoonlight(runtime, route, pair: false, invitation);
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
                    if (!connected && ready.WaitOne(0))
                    {
                        if (statsAvailable.WaitOne(0))
                        {
                            statsSignal = toggleStats;
                            statsItem.IsVisible = true;
                        }
                        ShowVideo();
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
                statsSignal = null;
                statsItem.IsVisible = false;
                privacyRequest = null;
                privacyBusy = false;
                privacyState = SessionPrivacyState.Unavailable;
                ShowPrivacyState();
                privacyLifetime.Cancel();
                if (!exit.IsCompleted)
                {
                    SetStatus("正在断开", "#526176");
                    controls.IsEnabled = fullscreen.IsEnabled = fullscreenItem.IsEnabled = false;
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
