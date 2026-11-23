using System;
using System.Globalization;
using System.IO;
using System.Linq;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace Aurora;

internal sealed class StreamSettingsPage : UserControl
{
    private const double ScrollGutter = 8;
    private static readonly IBrush Ink = Brush.Parse("#19283F");

    private readonly RadioButton[] tabs = new RadioButton[4];
    private readonly TextBlock saveWarning = new()
    {
        Text = "无法保存串流设置。改动会在本次运行中生效。",
        Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap, IsVisible = false
    };
    private readonly ScrollViewer scroll = new()
    {
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
    };
    private Control[] panels;
    private Action refreshStartup;

    public StreamSettingsPage()
    {
        BuildPanels();
        var tabBar = new WrapPanel { ItemSpacing = 6, LineSpacing = 6 };
        AutomationProperties.SetName(tabBar, "设置板块");
        var names = new[] { "画质", "音频", "通用", "高级设置" };
        for (var index = 0; index < names.Length; index++)
        {
            var target = index;
            var tab = new RadioButton
            {
                Content = names[index],
                GroupName = "StreamSettings",
                IsTabStop = false,
            };
            tab.Classes.Add("configTab");
            AutomationProperties.SetName(tab, names[index]);
            tab.IsCheckedChanged += (_, _) =>
            {
                if (tab.IsChecked == true) Select(target);
            };
            tab.KeyDown += (_, args) => MoveTab(args, target);
            tabs[index] = tab;
            tabBar.Children.Add(tab);
        }

        var reset = new Button
        {
            Content = "恢复默认设置",
            Classes = { "quietAction" },
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center,
        };
        reset.Click += (_, _) =>
        {
            ShowSaveResult(StreamPreferences.Reset());
            var trayChangeFailed = !TrayResidency.TrySetCloseToTray(true);
            var sleepChangeFailed = !TrySetSleepMode(HostSleepMode.System);
            BuildPanels(trayChangeFailed, sleepChangeFailed);
            Select(Array.FindIndex(tabs, tab => tab.IsChecked == true));
        };
        var header = new Grid
        {
            RowDefinitions = new RowDefinitions("Auto,Auto"),
            ColumnDefinitions = new ColumnDefinitions("*,Auto"),
            ColumnSpacing = 10,
            Margin = new Thickness(0, 0, ScrollGutter, 0),
        };
        header.Children.Add(tabBar);
        Grid.SetColumn(reset, 1);
        header.Children.Add(reset);
        header.SizeChanged += (_, args) =>
        {
            var width = args.NewSize.Width;
            var windowWidth = TopLevel.GetTopLevel(this)?.Bounds.Width ?? width;
            tabBar.ItemSpacing = Math.Clamp(windowWidth * 0.025, 6, 24);
            var tabsWidth = tabs.Sum(tab => Math.Max(tab.MinWidth, tab.DesiredSize.Width))
                + (tabs.Length - 1) * tabBar.ItemSpacing;
            var stacked = width < tabsWidth + header.ColumnSpacing + reset.DesiredSize.Width;
            Grid.SetColumnSpan(tabBar, stacked ? 2 : 1);
            Grid.SetColumn(reset, stacked ? 0 : 1);
            Grid.SetColumnSpan(reset, stacked ? 2 : 1);
            Grid.SetRow(reset, stacked ? 1 : 0);
            reset.Margin = stacked ? new Thickness(0, 6, 0, 0) : default;
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), RowSpacing = 11 };
        AutomationProperties.SetLiveSetting(saveWarning, AutomationLiveSetting.Polite);
        root.Children.Add(new StackPanel { Spacing = 6, Children = { header, saveWarning } });
        Grid.SetRow(scroll, 1);
        root.Children.Add(scroll);
        Content = root;
        scroll.TemplateApplied += (_, args) =>
        {
            if (args.NameScope.Find<ScrollBar>("PART_VerticalScrollBar") is { } bar)
                bar.Resources["ScrollBarSize"] = ScrollGutter;
        };
        tabs[0].IsChecked = true;
    }

    private void ShowSaveResult(bool saved) => saveWarning.IsVisible = !saved;

    private void BuildPanels(bool trayChangeFailed = false, bool sleepChangeFailed = false)
    {
        var bitrate = Bitrate(out var refreshBitrate);
        panels = new[] { Quality(refreshBitrate), Audio(), General(trayChangeFailed, sleepChangeFailed), Advanced(bitrate) };
    }

    private void MoveTab(KeyEventArgs args, int index)
    {
        var target = args.Key switch
        {
            Key.Left => (index + tabs.Length - 1) % tabs.Length,
            Key.Right => (index + 1) % tabs.Length,
            Key.Home => 0,
            Key.End => tabs.Length - 1,
            _ => -1,
        };
        if (target < 0) return;
        args.Handled = true;
        tabs[target].IsChecked = true;
        tabs[target].Focus(NavigationMethod.Directional);
    }

    private void Select(int index)
    {
        for (var target = 0; target < tabs.Length; target++)
            tabs[target].IsTabStop = target == index;

        scroll.Content = panels[index];
        AutomationProperties.SetName(scroll, tabs[index].Content as string);
        scroll.Offset = default;
    }

    private Control Quality(Action refreshBitrate)
    {
        var resolution = Choice("分辨率", "resolution", ("720p · 1280 × 720", "1280x720"),
            ("1080p · 1920 × 1080", "1920x1080"), ("1440p · 2560 × 1440", "2560x1440"),
            ("4K · 3840 × 2160", "3840x2160"));
        var fps = Choice("帧率", "fps", ("30 FPS", "30"), ("60 FPS", "60"));
        resolution.SelectionChanged += (_, _) => refreshBitrate();
        fps.SelectionChanged += (_, _) => refreshBitrate();
        var displayMode = Choice("串流窗口模式", "display-mode",
            ("全屏", "fullscreen"), ("无边框窗口", "borderless"), ("窗口", "windowed"));

        return Panel(Card("串流画面",
            Row("分辨率", resolution),
            Row("帧率", fps),
            Row("串流窗口模式", displayMode)));
    }

    private Control PlaybackSettings(Control bitrate)
    {
        var pacing = MakeToggle("帧速调节", StreamPreferences.GetToggle("frame-pacing"),
            value => ShowSaveResult(StreamPreferences.SetToggle("frame-pacing", value)));
        void UpdatePacing(bool vsyncEnabled)
        {
            pacing.IsEnabled = false;
            pacing.IsChecked = vsyncEnabled && StreamPreferences.GetToggle("frame-pacing");
            pacing.IsEnabled = vsyncEnabled;
        }

        var vsync = MakeToggle("垂直同步", StreamPreferences.GetToggle("vsync"), value =>
        {
            ShowSaveResult(StreamPreferences.SetToggle("vsync", value));
            UpdatePacing(value);
        });
        UpdatePacing(vsync.IsChecked == true);

        return Card("播放与显示",
            Row("视频码率", bitrate, minEditorWidth: 120),
            Row("垂直同步", vsync, stretchEditor: false),
            Row("帧速调节", pacing, stretchEditor: false),
            ToggleRow("串流时保持屏幕开启", "keep-awake"));
    }

    private Control Audio() => Panel(Card("音频播放",
        Row("串流时将目标电脑的扬声器静音", MakeToggle("串流时将目标电脑的扬声器静音",
            !StreamPreferences.GetToggle("audio-on-host"),
            value => ShowSaveResult(StreamPreferences.SetToggle("audio-on-host", !value))), stretchEditor: false)));

    internal void RefreshStartup() => refreshStartup?.Invoke();

    private Control General(bool trayChangeFailed, bool sleepChangeFailed)
    {
        var power = PowerSettings(sleepChangeFailed);
        const string startupLabel = "开机自启动";
        var startup = MakeToggle(startupLabel, false);
        var startupNote = Text("", 12);
        startupNote.Classes.Add("muted");
        startupNote.Margin = new Thickness(0, 0, 0, 10);
        AutomationProperties.SetLiveSetting(startupNote, AutomationLiveSetting.Polite);
        var updatingStartup = false;

        void ReadStartup(string error = null)
        {
            updatingStartup = true;
            try
            {
                var state = StartupRegistration.Read();
                startup.IsEnabled = state != StartupState.Unavailable;
                startup.IsChecked = state is StartupState.On or StartupState.OtherLocation or StartupState.NeedsApproval;
                startupNote.Text = error ?? (state switch
                {
                    StartupState.NeedsApproval => "请在系统设置的登录项中允许 Aurora。",
                    StartupState.OtherLocation => "启动项指向另一个位置的 Aurora。关闭后重新开启可更新路径。",
                    StartupState.Unavailable => "此系统或应用包无法登记开机自启动。",
                    _ => StartupRegistration.InitializationError,
                });
            }
            catch (Exception)
            {
                startup.IsEnabled = false;
                startupNote.Text = "无法读取开机自启动状态。";
            }
            finally { updatingStartup = false; }
            startupNote.IsVisible = !string.IsNullOrEmpty(startupNote.Text);
        }

        startup.IsCheckedChanged += (_, _) =>
        {
            if (updatingStartup || !startup.IsEnabled) return;
            string error = null;
            try { StartupRegistration.Change(startup.IsChecked == true); }
            catch (Exception) { error = "无法更改开机自启动。"; }
            ReadStartup(error);
        };
        refreshStartup = () => ReadStartup();
        ReadStartup();
        var startupRow = Row(startupLabel, startup, stretchEditor: false);
        if (!TrayResidency.IsAvailable) return Panel(Card("Aurora 窗口", startupRow, startupNote), power);

        const string label = "关闭窗口时";
        const int keepRunning = 0;
        const int exitAurora = 1;
        var choice = Combo(label, OperatingSystem.IsMacOS() ? "隐藏到菜单栏" : "最小化到托盘", "退出 Aurora");
        choice.SelectedIndex = TrayResidency.CloseToTray ? keepRunning : exitAurora;
        var error = Text("无法更改关闭窗口的设置。", 12);
        error.IsVisible = trayChangeFailed;
        error.Margin = new Thickness(0, 0, 0, 10);

        var restoringSelection = false;
        choice.SelectionChanged += (_, _) =>
        {
            if (restoringSelection || choice.SelectedIndex < 0) return;
            var changed = TrayResidency.TrySetCloseToTray(choice.SelectedIndex == keepRunning);
            error.IsVisible = !changed;
            if (!changed)
            {
                restoringSelection = true;
                choice.SelectedIndex = TrayResidency.CloseToTray ? keepRunning : exitAurora;
                restoringSelection = false;
            }
        };
        return Panel(Card("Aurora 窗口", startupRow, startupNote, Row(label, choice), error), power);
    }

    private static Control PowerSettings(bool changeFailed)
    {
        const string label = "防止电脑自动休眠";
        const string explanation = "关闭时按系统电源设置休眠。防休眠仅在被控组件运行时生效。";
        var modes = new[] { HostSleepMode.System, HostSleepMode.OnAc, HostSleepMode.Always };
        var selector = Combo(label, "关闭", "仅接通电源时", "始终");
        selector.SelectedIndex = Array.IndexOf(modes, HostSleepPrevention.Mode);
        var message = Text(changeFailed ? "无法恢复自动休眠设置。" : explanation, 12);
        message.Classes.Add("muted");
        message.Margin = new Thickness(0, 0, 0, 10);
        AutomationProperties.SetLiveSetting(message, AutomationLiveSetting.Polite);
        var restoring = false;
        selector.SelectionChanged += (_, _) =>
        {
            if (restoring || selector.SelectedIndex < 0) return;
            if (TrySetSleepMode(modes[selector.SelectedIndex]))
            {
                message.Text = explanation;
                return;
            }
            message.Text = "无法保存自动休眠设置。";
            restoring = true;
            selector.SelectedIndex = Array.IndexOf(modes, HostSleepPrevention.Mode);
            restoring = false;
        };
        return Card("电源", Row(label, selector), message);
    }

    private static bool TrySetSleepMode(HostSleepMode mode)
    {
        try
        {
            HostSleepPrevention.SetMode(mode);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    private Control[] InputSettings()
    {
        const string option = "capture-system-keys";
        var current = StreamPreferences.Get(option);
        var scope = Combo("系统快捷键捕获范围", "仅全屏时", "始终");
        scope.SelectedIndex = current == "always" ? 1 : 0;
        var capture = current != "never";
        string Scope() => scope.SelectedIndex == 1 ? "always" : "fullscreen";
        var shortcut = MakeToggle("捕获系统快捷键", capture, value =>
        {
            scope.IsEnabled = value;
            ShowSaveResult(StreamPreferences.Set(option, value ? Scope() : "never"));
        });
        scope.IsEnabled = capture;
        scope.SelectionChanged += (_, _) =>
        {
            if (scope.IsEnabled) ShowSaveResult(StreamPreferences.Set(option, Scope()));
        };
        var shortcuts = InlineEditors(scope, shortcut, minInlineWidth: 145);

        return [
            Card("键盘、鼠标与触屏",
                ToggleRow("桌面鼠标模式", "absolute-mouse"),
                Row("捕获系统快捷键", shortcuts, minEditorWidth: 120),
                ToggleRow("将触摸屏用作虚拟触控板", "touchscreen-trackpad"),
                ToggleRow("交换鼠标左右键", "mouse-buttons-swap"),
                ToggleRow("反转鼠标滚动方向", "reverse-scroll-direction")),
            Card("手柄",
                ToggleRow("交换手柄的 A/B 和 X/Y 键", "swap-gamepad-buttons"),
                ToggleRow("后台接收手柄输入", "background-gamepad"))];
    }

    private Control Advanced(Control bitrate)
    {
        const string option = "video-decoder";
        var values = new[] { "auto", "hardware", "software" };
        var decoder = Combo("视频解码方式", "自动（推荐）", "强制硬件解码", "强制软件解码");
        void UpdateDecoder(bool hdrEnabled)
        {
            var stored = Array.IndexOf(values, StreamPreferences.Get(option));
            decoder.IsEnabled = false;
            decoder.SelectedIndex = hdrEnabled ? 0 : Math.Max(0, stored);
            decoder.IsEnabled = !hdrEnabled;
        }
        decoder.SelectionChanged += (_, _) =>
        {
            if (decoder.IsEnabled && decoder.SelectedIndex >= 0)
                ShowSaveResult(StreamPreferences.Set(option, values[decoder.SelectedIndex]));
        };

        var initialHdr = StreamPreferences.GetToggle("hdr");
        var hdr = MakeToggle("启用 HDR（实验）", initialHdr, value =>
        {
            ShowSaveResult(StreamPreferences.SetToggle("hdr", value));
            UpdateDecoder(value);
        });
        UpdateDecoder(initialHdr);

        return Panel([
            PlaybackSettings(bitrate),
            Card("视频与性能", Row("视频解码方式", decoder),
                Row("视频编码格式", Choice("视频编码格式", "video-codec", ("自动（推荐）", "auto"),
                    ("H.264", "H.264"), ("HEVC（H.265）", "HEVC"), ("AV1（实验）", "AV1"))),
                Row("启用 HDR（实验）", hdr, stretchEditor: false),
                ToggleRow("串流时显示性能统计", "performance-overlay")),
            Card("音频",
                Row("音频配置", Choice("音频配置", "audio-config", ("立体声", "stereo"),
                    ("5.1 环绕声", "5.1-surround"), ("7.1 环绕声", "7.1-surround"))),
                ToggleRow("失去焦点时静音", "mute-on-focus-loss")),
            .. InputSettings(),
            Card("目标电脑",
                ToggleRow("为串流优化目标电脑上的游戏设置", "game-optimization"),
                ToggleRow("串流结束后退出目标电脑上的应用", "quit-after"))]);
    }

    private Control Bitrate(out Action refresh)
    {
        var slider = new Slider
        {
            Minimum = 0.5, Maximum = 150, TickFrequency = 0.5, IsSnapToTickEnabled = true,
            SmallChange = 0.5, LargeChange = 5, Value = StreamPreferences.GetBitrate() / 1000.0,
        };
        AutomationProperties.SetName(slider, "视频码率（Mbps）");
        var readout = Text("", 13);
        readout.MinWidth = 65;
        readout.TextAlignment = TextAlignment.Right;
        readout.TextWrapping = TextWrapping.NoWrap;
        void UpdateReadout()
        {
            readout.Text = slider.Value.ToString("0.#", CultureInfo.CurrentCulture) + " Mbps";
        }

        slider.ValueChanged += (_, _) =>
        {
            var kbps = (int)Math.Round(slider.Value * 1000);
            if (kbps != StreamPreferences.GetBitrate()) ShowSaveResult(StreamPreferences.SetBitrate(kbps));
            UpdateReadout();
        };
        refresh = () => slider.Value = StreamPreferences.GetBitrate() / 1000.0;
        UpdateReadout();
        return InlineEditors(slider, readout, minInlineWidth: 145);
    }

    private static Control InlineEditors(Control first, Control last, double minInlineWidth)
    {
        var layout = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto"),
            RowDefinitions = new RowDefinitions("Auto,Auto"),
            ColumnSpacing = 8,
        };
        first.VerticalAlignment = VerticalAlignment.Center;
        last.VerticalAlignment = VerticalAlignment.Center;
        last.HorizontalAlignment = HorizontalAlignment.Right;
        layout.Children.Add(first);
        Grid.SetColumn(last, 1);
        layout.Children.Add(last);
        layout.SizeChanged += (_, args) =>
        {
            var compact = args.NewSize.Width < minInlineWidth;
            Grid.SetColumnSpan(first, compact ? 2 : 1);
            Grid.SetColumn(last, compact ? 0 : 1);
            Grid.SetColumnSpan(last, compact ? 2 : 1);
            Grid.SetRow(last, compact ? 1 : 0);
        };
        return layout;
    }

    private ComboBox Choice(string label, string option, params (string Label, string Value)[] choices)
    {
        var combo = Combo(label, choices.Select(choice => choice.Label).ToArray());
        var selected = StreamPreferences.Get(option);
        combo.SelectedIndex = Array.FindIndex(choices, choice => choice.Value == selected);
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex >= 0) ShowSaveResult(StreamPreferences.Set(option, choices[combo.SelectedIndex].Value));
        };
        return combo;
    }

    private static ComboBox Combo(string label, params string[] choices)
    {
        var combo = new ComboBox
        {
            ItemsSource = choices, HorizontalAlignment = HorizontalAlignment.Stretch, MinWidth = 0,
            VerticalContentAlignment = VerticalAlignment.Center, FontSize = 13,
        };
        combo.Classes.Add("settingField");
        AutomationProperties.SetName(combo, label);
        return combo;
    }

    private Control ToggleRow(string label, string option) => Row(label,
        MakeToggle(label, StreamPreferences.GetToggle(option),
            value => ShowSaveResult(StreamPreferences.SetToggle(option, value))), stretchEditor: false);

    private static ToggleSwitch MakeToggle(string label, bool initial, Action<bool> change = null)
    {
        var toggle = new ToggleSwitch
        {
            IsThreeState = false, OnContent = "", OffContent = "",
            IsChecked = initial,
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center,
        };
        toggle.Classes.Add("settingSwitch");
        AutomationProperties.SetName(toggle, label);
        toggle.IsCheckedChanged += (_, _) =>
        {
            if (toggle.IsEnabled) change?.Invoke(toggle.IsChecked == true);
        };
        return toggle;
    }

    private static Control Panel(params Control[] cards)
    {
        var stack = new StackPanel { Spacing = 12, Margin = new Thickness(0, 0, ScrollGutter, 8) };
        foreach (var card in cards) stack.Children.Add(card);
        return stack;
    }

    private static Control Card(string title, params Control[] rows)
    {
        var stack = new StackPanel();
        var heading = new TextBlock { Classes = { "sectionTitle" }, Text = title, TextWrapping = TextWrapping.Wrap };
        AutomationProperties.SetHeadingLevel(heading, 2);
        stack.Children.Add(new Border { MinHeight = 45, Child = heading });
        foreach (var row in rows) stack.Children.Add(row);
        var group = new Border { Child = stack };
        group.Classes.Add("panel");
        group.Classes.Add("settingsGroup");
        return group;
    }

    private static Control Row(string label, Control editor, bool stretchEditor = true, double minEditorWidth = 0)
    {
        var row = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions(stretchEditor ? "52*,48*" : "*,Auto"),
            ColumnSpacing = 10,
            MinHeight = 45,
        };
        row.ColumnDefinitions[1].MaxWidth = 235;
        row.ColumnDefinitions[1].MinWidth = minEditorWidth;
        row.Children.Add(new TextBlock { Classes = { "settingLabel" }, Text = label });
        editor.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(editor, 1);
        row.Children.Add(editor);
        return row;
    }

    private static TextBlock Text(string value, double size) => new()
    {
        Text = value, FontSize = size, Foreground = Ink, TextWrapping = TextWrapping.Wrap,
        VerticalAlignment = VerticalAlignment.Center,
    };
}
