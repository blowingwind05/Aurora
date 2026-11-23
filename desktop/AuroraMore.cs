using System;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using Avalonia.VisualTree;

namespace Aurora;

internal static class AuroraMore
{
    private const string Repository = "https://github.com/blowingwind05/Aurora";
    private const string Help = Repository + "#从极光窗口开始";
    private const string FeedbackAddress = "blowingwind05@mail.ustc.edu.cn";
    private const string LatestRelease = "https://api.github.com/repos/blowingwind05/Aurora/releases/latest";
    private const string CheckLabel = "检查更新";
    private const double CompactWidth = 480;

    internal const string MenuIcon = "M4 5h16 M4 12h16 M4 19h16";
    private const string Circle = "M22 12a10 10 0 1 1-20 0 10 10 0 1 1 20 0Z";
    private const string HelpIcon = Circle + " M9 9.5a3 2.5 0 0 1 6 0c0 2-2 2-3 3.5 M12 17h.01";
    private const string AboutIcon = Circle + " M12 16v-4 M12 8h.01";
    private const string FeedbackIcon = "M22 17a2 2 0 0 1-2 2H7L3 22a.7.7 0 0 1-1-.7V5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2Z";
    private const string MailIcon = "M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2Z M22 7l-9 5.7a2 2 0 0 1-2 0L2 7";
    private const string ExternalIcon = "M7 17 17 7 M7 7h10v10";
    private const string SpinnerArc = "M21 12a9 9 0 1 1-9-9";
    // Primer mark-github-16 from the design's bundled icon assets.
    private const string GitHubIcon = "M6.766 11.328c-2.063-.25-3.516-1.734-3.516-3.656 0-.781.281-1.625.75-2.188-.203-.515-.172-1.609.063-2.062.625-.078 1.468.25 1.968.703.594-.187 1.219-.281 1.985-.281.765 0 1.39.094 1.953.265.484-.437 1.344-.765 1.969-.687.218.422.25 1.515.046 2.047.5.593.766 1.39.766 2.203 0 1.922-1.453 3.375-3.547 3.64.531.344.89 1.094.89 1.954v1.625c0 .468.391.734.86.547C13.781 14.359 16 11.53 16 8.03 16 3.61 12.406 0 7.984 0 3.563 0 0 3.61 0 8.031a7.88 7.88 0 0 0 5.172 7.422c.422.156.828-.125.828-.547v-1.25c-.219.094-.5.156-.75.156-1.031 0-1.64-.562-2.078-1.609-.172-.422-.36-.672-.719-.719-.187-.015-.25-.093-.25-.187 0-.188.313-.328.625-.328.453 0 .844.281 1.25.86.313.452.64.655 1.031.655s.641-.14 1-.5c.266-.265.47-.5.657-.656";
    private static readonly IBrush Accent = Brush.Parse("#087CF5");
    private static readonly IBrush Muted = Brush.Parse("#63738A");
    private static readonly IBrush Rule = Brush.Parse("#DBE2ED");
    private static readonly Lazy<Bitmap> AppIcon = new(() =>
    {
        using var stream = AssetLoader.Open(new Uri("avares://Aurora/Assets/Aurora.png"));
        return new Bitmap(stream);
    });
    private static readonly Version CurrentVersion = typeof(AuroraMore).Assembly.GetName().Version;
    private static readonly HttpClient Releases = CreateReleaseClient();

    internal static void Attach(Button button, Action<string, Control> showPage, Action<string> showNotice)
    {
        ArgumentNullException.ThrowIfNull(button);
        ArgumentNullException.ThrowIfNull(showPage);
        ArgumentNullException.ThrowIfNull(showNotice);

        if (button.Content == null)
            AuroraVisual.SetTextAction(button, "更多", MenuIcon, leading: true);
        AutomationProperties.SetName(button, "更多");

        var entries = new StackPanel { Width = 146 };
        var flyout = new MoreFlyout
        {
            Content = entries,
            Placement = PlacementMode.RightEdgeAlignedBottom,
            HorizontalOffset = 6
        };
        button.Flyout = flyout;

        var check = RowButton(33, new Thickness(7, 4));
        var refresh = AuroraVisual.Icon(AuroraVisual.Refresh, check, size: 16);
        // Both visuals occupy the same 16 DIP slot and keep the label fixed.
        var rotation = new RotateTransform();
        var spinner = new Path
        {
            Data = Geometry.Parse(SpinnerArc), Stroke = Accent, StrokeThickness = 1.6,
            StrokeLineCap = PenLineCap.Round, Stretch = Stretch.Uniform, Width = 14, Height = 14,
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            IsHitTestVisible = false, IsVisible = false,
            RenderTransformOrigin = RelativePoint.Center, RenderTransform = rotation
        };
        var iconSlot = new Grid { Width = 16, Height = 16, Children = { refresh, spinner } };
        var checkLabel = new TextBlock { Text = CheckLabel, VerticalAlignment = VerticalAlignment.Center };
        check.Content = new StackPanel
        {
            Orientation = Orientation.Horizontal, Spacing = 8, Children = { iconSlot, checkLabel }
        };
        AutomationProperties.SetName(check, CheckLabel);
        entries.Children.Add(check);

        var spin = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(32) };
        spin.Tick += (_, _) => rotation.Angle = (rotation.Angle + 14) % 360;
        var checking = false;
        flyout.Opening += (_, _) =>
        {
            foreach (var ancestor in button.GetVisualAncestors())
            {
                if (ancestor is not Control { Name: "Sidebar" } sidebar) continue;
                var buttonRight = button.TranslatePoint(new Point(button.Bounds.Width, 0), sidebar);
                if (buttonRight.HasValue)
                    flyout.HorizontalOffset = 6 + Math.Max(0, sidebar.Bounds.Width + sidebar.Margin.Right - buttonRight.Value.X);
                break;
            }
        };
        flyout.Opened += (_, _) =>
        {
            if (checking && !ReduceMotion()) spin.Start();
        };
        flyout.Closed += (_, _) => spin.Stop();
        check.Click += async (_, _) =>
        {
            if (checking) return;
            checking = true;
            refresh.IsVisible = false;
            spinner.IsVisible = true;
            checkLabel.Text = "正在检查…";
            AutomationProperties.SetName(check, checkLabel.Text);
            if (!ReduceMotion()) spin.Start();

            string result;
            try { result = await CheckLatestRelease(); }
            finally
            {
                spin.Stop();
                rotation.Angle = 0;
                refresh.IsVisible = true;
                spinner.IsVisible = false;
                checkLabel.Text = CheckLabel;
                AutomationProperties.SetName(check, CheckLabel);
                checking = false;
            }

            if (button.Flyout != flyout || TopLevel.GetTopLevel(button) == null) return;
            flyout.Hide();
            showNotice(result);
        };

        var repository = MenuRow("开源地址", GitHubIcon);
        repository.Click += async (_, _) =>
        {
            flyout.Hide();
            await OpenLink(button, Repository, showNotice);
        };
        var help = MenuRow("帮助", HelpIcon);
        help.Click += async (_, _) =>
        {
            flyout.Hide();
            await OpenLink(button, Help, showNotice);
        };
        var feedback = MenuRow("反馈", FeedbackIcon);
        feedback.Click += (_, _) =>
        {
            flyout.Hide();
            showPage("反馈", FeedbackPage(showNotice));
        };
        var about = MenuRow("关于", AboutIcon);
        about.Click += (_, _) =>
        {
            flyout.Hide();
            showPage("关于", AboutPage(showNotice));
        };
        entries.Children.Add(repository);
        entries.Children.Add(help);
        entries.Children.Add(feedback);
        entries.Children.Add(about);
    }

    private sealed class MoreFlyout : Flyout
    {
        protected override Control CreatePresenter() => new FlyoutPresenter
        {
            Content = Content,
            Background = Brushes.White,
            BorderBrush = Rule,
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(4)
        };
    }

    private static Button RowButton(double height, Thickness padding) => new()
    {
        Classes = { "nav" },
        MinHeight = height,
        Padding = padding,
        HorizontalContentAlignment = HorizontalAlignment.Stretch,
        CornerRadius = new CornerRadius(6)
    };

    private static Control Mark(string geometry, Button owner, double size)
    {
        if (geometry != GitHubIcon) return AuroraVisual.Icon(geometry, owner, size);
        var mark = new PathIcon { Data = Geometry.Parse(GitHubIcon), Width = size, Height = size, IsHitTestVisible = false };
        mark[!PathIcon.ForegroundProperty] = owner[!Button.ForegroundProperty];
        return mark;
    }

    private static Button MenuRow(string title, string icon)
    {
        var row = RowButton(33, new Thickness(7, 4));
        row.Content = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Children = { Mark(icon, row, 16), new TextBlock { Text = title, VerticalAlignment = VerticalAlignment.Center } }
        };
        AutomationProperties.SetName(row, title);
        return row;
    }

    private static Control FeedbackPage(Action<string> showNotice)
    {
        var email = FeedbackCard("邮箱", FeedbackAddress, MailIcon, "mailto:" + FeedbackAddress, showNotice);
        var issues = FeedbackCard("GitHub Issues", "公开报告问题或提出功能建议。", GitHubIcon,
            Repository + "/issues/new/choose", showNotice);
        var channels = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,*"),
            RowDefinitions = new RowDefinitions("Auto,Auto"),
            ColumnSpacing = 12,
            VerticalAlignment = VerticalAlignment.Top,
            Children = { email, issues }
        };
        Grid.SetColumn(issues, 1);
        channels.SizeChanged += (_, args) =>
        {
            var narrow = args.NewSize.Width < CompactWidth;
            Grid.SetColumnSpan(email, narrow ? 2 : 1);
            Grid.SetColumnSpan(issues, narrow ? 2 : 1);
            Grid.SetColumn(issues, narrow ? 0 : 1);
            Grid.SetRow(issues, narrow ? 1 : 0);
            issues.Margin = narrow ? new Thickness(0, 12, 0, 0) : default;
        };
        return ScrollPage(channels);
    }

    private static Button FeedbackCard(string title, string detail, string icon, string target, Action<string> showNotice)
    {
        var card = LinkButton(title, detail, target, showNotice, 82, new Thickness(14, 13));
        card.Background = Brushes.White;
        card.BorderBrush = Rule;
        card.BorderThickness = new Thickness(1);
        card.CornerRadius = new CornerRadius(11);
        var label = new StackPanel { Spacing = 4, Children = { Heading(title), Secondary(detail) } };
        var mark = Mark(icon, card, 22);
        var arrow = AuroraVisual.Icon(ExternalIcon, card, size: 16);
        var content = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("24,*,16"), ColumnSpacing = 11,
            VerticalAlignment = VerticalAlignment.Center, Children = { mark, label, arrow }
        };
        Grid.SetColumn(label, 1);
        Grid.SetColumn(arrow, 2);
        card.Content = content;
        return card;
    }

    private static Control AboutPage(Action<string> showNotice)
    {
        var logo = new Image { Source = AppIcon.Value, Width = 56, Height = 56 };
        var title = Heading("Aurora 远程桌面");
        title.FontSize = 19;
        var version = Secondary(CurrentVersion == null ? "版本未知" : $"版本 {DisplayVersion(CurrentVersion)}");
        var identity = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 12,
            Children = { logo, new StackPanel { VerticalAlignment = VerticalAlignment.Center, Spacing = 3, Children = { title, version } } }
        };
        var overview = Panel(new StackPanel
        {
            Spacing = 9,
            Children = { identity, Secondary("Aurora 是开源远程桌面应用，可通过局域网或 Tailscale 连接自己的电脑并串流桌面。") }
        }, new Thickness(16, 14));
        var project = AboutSection("项目信息",
            AboutLink("作者", "blowingwind05", "https://github.com/blowingwind05", showNotice),
            AboutLink("开源仓库", "GitHub", Repository, showNotice),
            AboutLink("开源许可", "GPL-3.0-only", Repository + "/blob/main/LICENSE", showNotice));
        var components = AboutSection("主要组件",
            AboutLink("Moonlight", "在主控端播放串流并传送键鼠输入。", "https://github.com/moonlight-stream/moonlight-qt", showNotice),
            AboutLink("Sunshine", "在被控端采集、编码并发送桌面画面。", "https://github.com/SunshineStream/Sunshine", showNotice),
            AboutLink("Tailscale", "发现同账号设备并建立跨网连接。", "https://github.com/tailscale/tailscale", showNotice));
        return ScrollPage(new StackPanel { Spacing = 12, Children = { overview, project, components } });
    }

    private static Control AboutSection(string title, params Button[] rows)
    {
        var label = Heading(title);
        label.Classes.Add("sectionTitle");
        var links = new StackPanel();
        foreach (var row in rows) links.Children.Add(row);
        return new StackPanel { Spacing = 6, Children = { label, Panel(links, new Thickness(8)) } };
    }

    private static Button AboutLink(string title, string detail, string target, Action<string> showNotice)
    {
        var row = LinkButton(title, detail, target, showNotice, 33, new Thickness(8, 5));
        var heading = Heading(title);
        var copy = Secondary(detail);
        var label = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("120,*"),
            RowDefinitions = new RowDefinitions("Auto,Auto"), ColumnSpacing = 8, RowSpacing = 2,
            Children = { heading, copy }
        };
        Grid.SetColumn(copy, 1);
        label.SizeChanged += (_, args) =>
        {
            var narrow = args.NewSize.Width < 330;
            Grid.SetColumnSpan(heading, narrow ? 2 : 1);
            Grid.SetColumnSpan(copy, narrow ? 2 : 1);
            Grid.SetColumn(copy, narrow ? 0 : 1);
            Grid.SetRow(copy, narrow ? 1 : 0);
        };
        var arrow = AuroraVisual.Icon(ExternalIcon, row, size: 16);
        Grid.SetColumn(arrow, 1);
        row.Content = new Grid { ColumnDefinitions = new ColumnDefinitions("*,16"), ColumnSpacing = 8, Children = { label, arrow } };
        return row;
    }

    private static Button LinkButton(string title, string detail, string target, Action<string> showNotice, double height, Thickness padding)
    {
        var button = RowButton(height, padding);
        AutomationProperties.SetName(button, $"{title}，{detail}");
        button.Click += async (_, _) => await OpenLink(button, target, showNotice);
        return button;
    }

    private static Border Panel(Control child, Thickness padding) =>
        new() { Classes = { "panel" }, Child = child, Padding = padding };

    private static ScrollViewer ScrollPage(Control content) => new()
    {
        Content = content,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto
    };

    private static TextBlock Heading(string text) =>
        new() { Text = text, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap };

    private static TextBlock Secondary(string text) =>
        new() { Text = text, FontSize = 12, Foreground = Muted, TextWrapping = TextWrapping.Wrap };

    private static async Task OpenLink(Control anchor, string target, Action<string> showNotice)
    {
        try
        {
            var topLevel = TopLevel.GetTopLevel(anchor);
            if (topLevel != null && await topLevel.Launcher.LaunchUriAsync(new Uri(target))) return;
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or
            System.IO.IOException or UnauthorizedAccessException or NotSupportedException)
        {
            System.Diagnostics.Debug.WriteLine(error);
        }
        showNotice(target.StartsWith("mailto:", StringComparison.Ordinal) ? "无法打开系统邮件应用。" : "无法打开链接。");
    }

    private static HttpClient CreateReleaseClient()
    {
        var client = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false, UseCookies = false })
        {
            Timeout = TimeSpan.FromSeconds(8),
            MaxResponseContentBufferSize = 1024 * 1024
        };
        client.DefaultRequestHeaders.UserAgent.ParseAdd($"Aurora/{CurrentVersion ?? new Version(0, 0)}");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        client.DefaultRequestHeaders.Add("X-GitHub-Api-Version", "2022-11-28");
        return client;
    }

    private static async Task<string> CheckLatestRelease()
    {
        if (CurrentVersion == null) return "无法读取当前版本。";
        try
        {
            using var response = await Releases.GetAsync(LatestRelease).ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.NotFound)
                return "GitHub 未返回可用的正式版本（HTTP 404）。";
            if (!response.IsSuccessStatusCode)
                return $"无法检查更新。GitHub 返回 HTTP {(int)response.StatusCode}。";

            using var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var release = await JsonDocument.ParseAsync(stream).ConfigureAwait(false);
            var releaseInfo = release.RootElement;
            if (releaseInfo.ValueKind != JsonValueKind.Object ||
                !releaseInfo.TryGetProperty("draft", out var draft) || draft.ValueKind != JsonValueKind.False ||
                !releaseInfo.TryGetProperty("prerelease", out var prerelease) || prerelease.ValueKind != JsonValueKind.False ||
                !releaseInfo.TryGetProperty("tag_name", out var tag) || tag.ValueKind != JsonValueKind.String)
                return "GitHub 未返回可用的正式版本。";
            var tagName = tag.GetString().Trim();
            if (tagName.StartsWith('v') || tagName.StartsWith('V')) tagName = tagName[1..];
            if (!Version.TryParse(tagName, out var latest)) return "无法识别 GitHub 的正式版本号。";

            int comparison = Normalize(latest).CompareTo(Normalize(CurrentVersion));
            if (comparison > 0) return $"发现新版本 {DisplayVersion(latest)}。";
            return comparison == 0 ? "当前已是最新版本" : $"当前版本高于已发布的正式版本 {DisplayVersion(latest)}。";
        }
        catch (OperationCanceledException) { return "检查更新超时，请重试。"; }
        catch (HttpRequestException) { return "无法连接 GitHub，请检查网络后重试。"; }
        catch (JsonException) { return "无法读取 GitHub 的版本信息。"; }
    }

    private static Version Normalize(Version version) =>
        new(version.Major, version.Minor, Math.Max(0, version.Build), Math.Max(0, version.Revision));

    private static string DisplayVersion(Version version) =>
        Normalize(version).ToString(version.Revision > 0 ? 4 : 3);

    private static bool ReduceMotion()
    {
        if (OperatingSystem.IsWindows())
            return SystemParametersInfo(0x1042, 0, out var enabled, 0) != 0 && enabled == 0;
        if (!OperatingSystem.IsMacOS()) return false;
        var workspace = SendObject(GetClass("NSWorkspace"), RegisterSelector("sharedWorkspace"));
        return SendBool(workspace, RegisterSelector("accessibilityDisplayShouldReduceMotion")) != 0;
    }

    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", ExactSpelling = true)]
    private static extern int SystemParametersInfo(uint action, uint parameter, out int value, uint flags);

    private const string ObjC = "/usr/lib/libobjc.A.dylib";
    [DllImport(ObjC, EntryPoint = "objc_getClass")] private static extern IntPtr GetClass(string name);
    [DllImport(ObjC, EntryPoint = "sel_registerName")] private static extern IntPtr RegisterSelector(string name);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")] private static extern IntPtr SendObject(IntPtr target, IntPtr selector);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")] private static extern byte SendBool(IntPtr target, IntPtr selector);
}
