using System;
using System.Globalization;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace Aurora;

internal sealed class WindowsStreamControlCenter : UserControl
{
    internal const string Sliders = "M3 7h2.5m5 0H21 M3 17h10.5m5 0H21 M5.5 7a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0Z M13.5 17a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0Z";
    private const string Volume = "M11 4 6 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h3l5 4Z M15 9a4 4 0 0 1 0 6 M18 6a8 8 0 0 1 0 12";
    private const string Mouse = "M12 6v4 M12 2a7 7 0 0 0-7 7v6a7 7 0 0 0 14 0V9a7 7 0 0 0-7-7Z";
    private const string Shield = "M12 22s8-3 8-10V5l-8-3-8 3v7c0 7 8 10 8 10Z";
    private const string Fullscreen = "M8 3H5a2 2 0 0 0-2 2v3 M16 3h3a2 2 0 0 1 2 2v3 M21 16v3a2 2 0 0 1-2 2h-3 M8 21H5a2 2 0 0 1-2-2v-3";

    private readonly StackPanel rows = new() { Width = 274 };
    private readonly ScrollViewer scroll;
    private TextBlock saveError;
    private bool persistenceFailed;
    private readonly TextBlock privacyStatus = new() { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap };
    private readonly Button privacyAction = new() { Classes = { "action" }, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly Action toggleFullscreen;
    private readonly Action connectionDetails;
    private readonly Action disconnect;
    private readonly Func<Task> queryPrivacy;
    private bool subpage;
    private bool fullscreen;
    private int? pendingBitrate;
    private readonly DispatcherTimer bitrateSave = new() { Interval = TimeSpan.FromMilliseconds(250) };

    internal WindowsStreamControlCenter(Action toggleFullscreen, Action connectionDetails, Action disconnect,
        Func<Task> queryPrivacy, Func<Task> togglePrivacy)
    {
        this.toggleFullscreen = toggleFullscreen;
        this.connectionDetails = connectionDetails;
        this.disconnect = disconnect;
        this.queryPrivacy = queryPrivacy;
        privacyAction.Click += async (_, _) => await togglePrivacy();
        bitrateSave.Tick += (_, _) => SaveBitrate();
        scroll = new ScrollViewer { Content = rows, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Content = scroll;
        AddHandler(KeyDownEvent, (_, args) =>
        {
            if (args.Key == Key.Escape && subpage)
            {
                Back();
                args.Handled = true;
            }
        }, RoutingStrategies.Tunnel);
    }

    internal void Fit(double windowHeight) => scroll.MaxHeight = Math.Clamp(windowHeight - 65, 140, 390);
    internal void SetFullscreen(bool value) => fullscreen = value;

    internal void SetPrivacy(string status, string action, bool visible, bool enabled)
    {
        privacyStatus.Text = status;
        privacyAction.Content = action;
        privacyAction.IsVisible = visible;
        privacyAction.IsEnabled = enabled;
    }

    internal void SaveBitrate()
    {
        bitrateSave.Stop();
        if (pendingBitrate is not { } value) return;
        pendingBitrate = null;
        Save(StreamPreferences.SetBitrate(value));
    }

    internal void Home()
    {
        Reset();
        subpage = false;
        rows.Children.Add(MenuRow("画质", AuroraVisual.Monitor, Quality, enter: true));
        rows.Children.Add(MenuRow("声音", Volume, Sound, enter: true));
        rows.Children.Add(MenuRow("输入", Mouse, Input, enter: true));
        rows.Children.Add(MenuRow("安全", Shield, async () => await Security(), enter: true));
        if (fullscreen)
        {
            rows.Children.Add(MenuRow("连接详情", AuroraVisual.Monitor, connectionDetails));
            rows.Children.Add(MenuRow("断开连接", AuroraVisual.Close, disconnect));
        }
        rows.Children.Add(new Border { Height = 1, Background = Brush.Parse("#E2E7EF"), Margin = new Thickness(0, 5) });
        rows.Children.Add(MenuRow(fullscreen ? "退出全屏" : "全屏",
            fullscreen ? AuroraVisual.RestoreWindow : Fullscreen, toggleFullscreen));
    }

    private void Back()
    {
        Home();
        ((Button)rows.Children[0]).Focus();
    }

    private void Reset()
    {
        SaveBitrate();
        rows.Children.Clear();
        scroll.Offset = default;
    }

    private StackPanel BeginPage(string title, bool nextConnection = true)
    {
        Reset();
        subpage = true;
        var back = MenuRow(title, AuroraVisual.ArrowLeft, Back);
        AutomationProperties.SetName(back, "返回控制中心");
        var header = new Grid { Children = { back } };
        rows.Children.Add(header);
        back.Focus();
        var settings = new StackPanel { Spacing = 7, Margin = new Thickness(8, 2, 8, 5) };
        rows.Children.Add(settings);
        if (nextConnection)
        {
            header.Children.Add(new TextBlock
            {
                Text = "下次连接生效", FontSize = 11, Classes = { "muted" },
                VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right,
                Margin = new Thickness(0, 0, 8, 0), IsHitTestVisible = false
            });
            saveError = new TextBlock { Text = "设置暂时无法写入磁盘。", IsVisible = persistenceFailed, TextWrapping = TextWrapping.Wrap };
            settings.Children.Add(saveError);
        }
        return settings;
    }

    private void Quality()
    {
        var settings = BeginPage("画质");
        var quality = new Slider
        {
            Minimum = 0.5, Maximum = 150, Value = StreamPreferences.GetBitrate() / 1000.0,
            SmallChange = 0.5, LargeChange = 5, TickFrequency = 0.5, IsSnapToTickEnabled = true
        };
        AutomationProperties.SetName(quality, "视频码率（Mbps）");
        var bitrate = new TextBlock { FontSize = 12, TextAlignment = TextAlignment.Right };
        void RefreshBitrate() => bitrate.Text = quality.Value.ToString("0.#", CultureInfo.CurrentCulture) + " Mbps";
        // Changing resolution or frame rate refreshes Aurora's automatic bitrate estimate.
        void RefreshEstimate()
        {
            quality.Value = StreamPreferences.GetBitrate() / 1000.0;
            RefreshBitrate();
        }

        RefreshBitrate();
        quality.ValueChanged += (_, _) =>
        {
            RefreshBitrate();
            var kbps = (int)Math.Round(quality.Value * 1000);
            if (kbps == StreamPreferences.GetBitrate() && !pendingBitrate.HasValue) return;
            pendingBitrate = kbps;
            bitrateSave.Stop();
            bitrateSave.Start();
        };
        var label = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Children = { Label("视频码率"), bitrate } };
        Grid.SetColumn(bitrate, 1);
        settings.Children.Add(Label("分辨率"));
        settings.Children.Add(Choice("resolution", RefreshEstimate, ("跟随被控端", StreamPreferences.FollowHostResolution),
            ("720p", "1280x720"), ("1080p", "1920x1080"), ("1440p", "2560x1440"), ("4K", "3840x2160")));
        settings.Children.Add(Label("帧率"));
        settings.Children.Add(Choice("fps", RefreshEstimate, ("30 FPS", "30"), ("60 FPS", "60")));
        settings.Children.Add(label);
        settings.Children.Add(quality);
        settings.Children.Add(Label("视频编码"));
        settings.Children.Add(Choice("video-codec", null, ("自动", "auto"), ("H.264", "H.264"), ("HEVC", "HEVC"), ("AV1", "AV1")));
        settings.Children.Add(Toggle("HDR", "hdr"));
        settings.Children.Add(Toggle("性能统计", "performance-overlay"));
    }

    private void Sound()
    {
        var settings = BeginPage("声音");
        settings.Children.Add(Label("音频配置"));
        settings.Children.Add(Choice("audio-config", null, ("立体声", "stereo"),
            ("5.1 环绕声", "5.1-surround"), ("7.1 环绕声", "7.1-surround")));
        settings.Children.Add(Toggle("远端扬声器静音", "audio-on-host", invert: true));
        settings.Children.Add(Toggle("失焦静音", "mute-on-focus-loss"));
    }

    private void Input()
    {
        var settings = BeginPage("输入");
        settings.Children.Add(Label("鼠标模式"));
        settings.Children.Add(Choice("absolute-mouse", null, ("桌面指针", "on"), ("锁定鼠标", "off")));
        settings.Children.Add(Toggle("交换鼠标左右键", "mouse-buttons-swap"));
        settings.Children.Add(Toggle("反转滚轮", "reverse-scroll-direction"));
        settings.Children.Add(new TextBlock
        {
            Text = "Ctrl+Alt+Shift+M  切换当前鼠标模式", FontSize = 12,
            Classes = { "muted" }, TextWrapping = TextWrapping.Wrap
        });
    }

    private async Task Security()
    {
        var settings = BeginPage("安全", nextConnection: false);
        settings.Children.Add(Label("隐私屏保"));
        if (privacyStatus.Parent is Panel oldPanel)
        {
            oldPanel.Children.Remove(privacyStatus);
            oldPanel.Children.Remove(privacyAction);
        }
        settings.Children.Add(privacyStatus);
        settings.Children.Add(privacyAction);
        await queryPrivacy();
    }

    private Control Choice(string key, Action changed, params (string Text, string Value)[] options)
    {
        var grid = new Grid { ColumnSpacing = 4 };
        foreach (var option in options)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition(new GridLength(option.Text == "跟随被控端" ? 2 : 1, GridUnitType.Star)));
            var choice = new ToggleButton
            {
                Content = option.Text, Classes = { "sessionOption" },
                IsChecked = StreamPreferences.Get(key) == option.Value, HorizontalAlignment = HorizontalAlignment.Stretch
            };
            choice.Click += (_, _) =>
            {
                SaveBitrate();
                if (StreamPreferences.Get(key) != option.Value)
                {
                    Save(StreamPreferences.Set(key, option.Value));
                    changed?.Invoke();
                }
                foreach (ToggleButton button in grid.Children) button.IsChecked = button == choice;
            };
            Grid.SetColumn(choice, grid.Children.Count);
            grid.Children.Add(choice);
        }
        return grid;
    }

    private Control Toggle(string label, string key, bool invert = false)
    {
        var toggle = new ToggleSwitch
        {
            Classes = { "settingSwitch" }, IsChecked = StreamPreferences.GetToggle(key) != invert,
            HorizontalAlignment = HorizontalAlignment.Right
        };
        AutomationProperties.SetName(toggle, label);
        toggle.IsCheckedChanged += (_, _) => Save(StreamPreferences.SetToggle(key, (toggle.IsChecked == true) != invert));
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Children = { Label(label), toggle } };
        Grid.SetColumn(toggle, 1);
        return row;
    }

    private void Save(bool saved)
    {
        persistenceFailed = !saved;
        if (saveError != null) saveError.IsVisible = persistenceFailed;
    }

    private static TextBlock Label(string text) => new() { Text = text, FontSize = 13, VerticalAlignment = VerticalAlignment.Center };

    private static Button MenuRow(string label, string icon, Action clicked, bool enter = false)
    {
        var button = new Button { Classes = { "sessionMenuRow" } };
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("18,*,14"), ColumnSpacing = 7 };
        var title = Label(label);
        Grid.SetColumn(title, 1);
        row.Children.Add(AuroraVisual.Icon(icon, button, size: 16));
        row.Children.Add(title);
        if (enter)
        {
            var arrow = AuroraVisual.Icon(AuroraVisual.ChevronRight, button, size: 12);
            Grid.SetColumn(arrow, 2);
            row.Children.Add(arrow);
        }
        button.Content = row;
        AutomationProperties.SetName(button, label);
        button.Click += (_, _) => clicked();
        return button;
    }
}
