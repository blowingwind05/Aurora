using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Automation;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace Aurora;

public sealed partial class AuroraWindow : Window
{
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(10) };
    private readonly CancellationTokenSource lifetime = new();
    private NetworkSnapshot network;
    private LanSnapshot lan;
    private Device[] discovered = Array.Empty<Device>();
    private HostStatus host;
    private SunshinePage sunshinePage;
    private TailscalePage tailscalePage;
    private bool refreshing;
    private bool installing;
    private bool connecting;
    private bool closed;
    private bool confirmInstall;

    public AuroraWindow()
    {
        InitializeComponent();
        var windows = OperatingSystem.IsWindows();
        CaptionButtons.IsVisible = DragArea.IsVisible = HostNavItem.IsVisible = windows;
        if (windows)
        {
            ExtendClientAreaChromeHints = Avalonia.Platform.ExtendClientAreaChromeHints.NoChrome;
            ExtendClientAreaTitleBarHeightHint = 54;
            ExtendClientAreaToDecorationsHint = true;
        }
        DragArea.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            if (e.ClickCount == 2) ToggleMaximize();
            else BeginMoveDrag(e);
        };
        MinimizeWindow.Click += (_, _) => WindowState = WindowState.Minimized;
        MaximizeWindow.Click += (_, _) => ToggleMaximize();
        CloseWindow.Click += (_, _) => Close();
        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty) UpdateMaximizeIcon();
        };
        Opened += async (_, _) => { timer.Start(); await RefreshDevices(); };
        Closing += (_, args) =>
        {
            if (sunshinePage?.CanLeaveApp() != false) return;
            args.Cancel = true;
            Navigation.SelectedIndex = 1;
        };
        Closed += (_, _) =>
        {
            closed = true;
            timer.Stop();
            lifetime.Cancel();
            lifetime.Dispose();
            sunshinePage?.Dispose();
            tailscalePage?.Dispose();
        };
        timer.Tick += async (_, _) => await RefreshDevices();
        Refresh.Click += async (_, _) => await RefreshDevices();
        Devices.SelectionChanged += (_, _) => UpdateActions();
        Devices.DoubleTapped += async (_, _) => await Launch(false);
        Connect.Click += async (_, _) => await Launch(false);
        HostAction.Click += async (_, _) => await ActivateHost();
        CopyAddress.Click += async (_, _) => await CopyLocalAddress(lan?.Address, "局域网");
        CopyTailnet.Click += async (_, _) => await CopyLocalAddress(network?.LocalAddress, "Tailscale");
        Navigation.SelectionChanged += (_, _) => ShowPage();
        NetworkHelp.Click += (_, _) => Navigation.SelectedIndex = 2;
    }

    private Device SelectedDevice => (Devices.SelectedItem as ListBoxItem)?.Tag as Device;
    private void ToggleMaximize() => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void UpdateMaximizeIcon()
    {
        var maximized = WindowState == WindowState.Maximized;
        MaximizeIcon.Data = Geometry.Parse(maximized
            ? "M2,0 L10,0 L10,8 M0,2 L8,2 L8,10 L0,10 Z" : "M0,0 L10,0 L10,10 L0,10 Z");
        AutomationProperties.SetName(MaximizeWindow, maximized ? "还原窗口" : "最大化");
        ToolTip.SetTip(MaximizeWindow, maximized ? "还原窗口" : "最大化");
    }

    private async Task CopyLocalAddress(System.Net.IPAddress address, string source)
    {
        if (address == null || Clipboard == null) return;
        try
        {
            await Clipboard.SetTextAsync(address.ToString());
            SetFeedback($"已复制 {source} 地址。可添加到其他设备上的 Moonlight。");
        }
        catch (Exception error) { SetFeedback("复制失败：" + error.Message); }
    }

    private void UpdateActions()
    {
        var selected = SelectedDevice;
        Connect.IsEnabled = !connecting && selected?.CanConnect == true;
        SelectionHint.Text = connecting ? "正在连接或配对…" :
            selected == null ? "选择要控制的设备" :
            !selected.Online ? "这台设备当前不在线" :
            !selected.CanConnect ? "设备在线，但未检测到串流服务" :
            selected.Lan != null && selected.Tailnet != null ? "优先局域网 · Tailscale 备用" :
            selected.Lan != null ? "通过局域网连接" : "通过 Tailscale 连接";
        CopyAddress.IsEnabled = lan?.Address != null;
        CopyTailnet.IsEnabled = network?.LocalAddress != null;
        HostAction.IsEnabled = !installing && (host != null || !refreshing);
        Refresh.IsEnabled = !refreshing;
    }

    private async Task RefreshDevices()
    {
        if (refreshing || closed) return;
        refreshing = true;
        UpdateActions();
        var discover = Task.Run(Upstream.ReadNetwork);
        var nearby = Task.Run(Discovery.ReadLan);
        var detect = OperatingSystem.IsWindows() ? Task.Run(Upstream.ReadHost) : Task.FromResult<HostStatus>(null);
        string networkError = null;
        string lanError = null;
        string hostError = null;
        IReadOnlyList<StreamHost> tailnetHosts = Array.Empty<StreamHost>();
        try { network = await discover; }
        catch (Exception error) { network = null; networkError = error.Message; }
        var tailnetProbe = network == null ? null : Discovery.ReadTailnetHosts(network);
        try { lan = await nearby; }
        catch (Exception error) { lan = null; lanError = error.Message; }
        if (tailnetProbe != null)
        {
            try { tailnetHosts = await tailnetProbe; }
            catch (Exception error) { networkError = error.Message; }
        }
        try { host = await detect; }
        catch (Exception error) { host = null; hostError = error.Message; }
        if (closed) return;
        refreshing = false;
        discovered = Discovery.Merge(network?.Devices.ToArray() ?? Array.Empty<Device>(),
            lan?.Hosts ?? Array.Empty<StreamHost>(), tailnetHosts).ToArray();
        RenderDevices(networkError, lanError);
        if (OperatingSystem.IsWindows())
        {
            RenderHost(hostError);
            if (host?.Installed == true && (HostPage.IsVisible || AutomaticPairing.IsConfigured)) OpenHostSettings();
            if (sunshinePage != null) _ = sunshinePage.SetNetwork(network);
        }
        UpdateActions();
    }

    private void RenderDevices(string networkError, string lanError)
    {
        var selected = SelectedDevice;
        var rows = discovered.Select(CreateRow).ToArray();
        Devices.ItemsSource = rows;
        Devices.SelectedItem = rows.FirstOrDefault(row => ((Device)row.Tag!).Id == selected?.Id) ??
            rows.FirstOrDefault(row => selected?.Lan != null && ((Device)row.Tag!).Lan == selected.Lan);
        EmptyState.IsVisible = rows.Length == 0;
        NetworkNotice.IsVisible = networkError != null || lanError != null;
        NetworkHelp.IsVisible = networkError != null;
        NetworkMessage.Text = string.Join(" ", new[]
        {
            lanError == null ? null : "局域网发现失败，可尝试刷新。",
            networkError == null ? null : network == null
                ? "Tailscale 未就绪。异地连接时请检查。"
                : "无法检查 Tailscale 串流服务，可尝试刷新。"
        }.Where(message => message != null));
        ToolTip.SetTip(NetworkMessage, string.Join("\n", new[] { lanError, networkError }.Where(error => error != null)));
        DeviceSummary.Text = $"{rows.Length} 台设备 · {discovered.Count(device => device.CanConnect)} 台可串流";
        EmptyTitle.Text = "还没有发现其他设备";
        EmptyHint.Text = OperatingSystem.IsMacOS() && lan?.Hosts.Count is null or 0
            ? "请在 Windows 上启动 Sunshine。同网未发现时，到系统设置 → 隐私与安全性 → 本地网络允许极光访问。异地连接请登录同一 Tailscale 账号。"
            : "在同一局域网的电脑上启动 Sunshine，或登录同一 Tailscale 账号发现异地设备。";
        var localAddresses = new[] { lan?.Address == null ? null : "局域网 " + lan.Address,
            network?.LocalAddress == null ? null : "Tailscale " + network.LocalAddress }.Where(address => address != null).ToArray();
        LocalAddress.Text = localAddresses.Length == 0 ? "连接地址暂不可用" : string.Join("   ·   ", localAddresses);
    }

    private ListBoxItem CreateRow(Device device)
    {
        var platform = device.Platform.ToLowerInvariant() switch
        {
            "windows" => "Windows",
            "android" => "Android",
            "macos" => "macOS",
            "ios" => "iOS",
            "linux" => "Linux",
            _ => "其他设备"
        };
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), ColumnSpacing = 12 };
        var tile = new Border
        {
            Width = 34, Height = 34, CornerRadius = new CornerRadius(8),
            Background = Brush.Parse("#E9EEFD"), VerticalAlignment = VerticalAlignment.Center,
            Child = new TextBlock
            {
                Text = platform == "其他设备" ? "·" : platform[..1].ToUpperInvariant(), FontSize = 15,
                Foreground = Brush.Parse("#244FC5"), FontWeight = FontWeight.Bold,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
            }
        };
        var info = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        info.Children.Add(new TextBlock { Text = device.Name, FontWeight = FontWeight.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        var paths = device.Lan != null && device.TailnetVisible ? "局域网 + Tailscale" :
            device.Lan != null ? "局域网" : "Tailscale";
        var detail = new TextBlock { Text = $"{paths}  ·  {device.Lan?.Endpoint.Address ?? device.Address}", FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis };
        if (device.Lan != null && device.Tailnet != null)
            ToolTip.SetTip(detail, $"局域网：{device.Lan.Endpoint}\nTailscale：{device.Tailnet.Endpoint}");
        detail.Classes.Add("muted");
        info.Children.Add(detail);
        Grid.SetColumn(info, 1);
        var state = new Border
        {
            CornerRadius = new CornerRadius(9), Padding = new Thickness(8, 3), VerticalAlignment = VerticalAlignment.Center,
            Background = Brush.Parse(device.Online ? "#E7EEFF" : "#EBEDF1"),
            Child = new TextBlock
            {
                Text = device.CanConnect ? "可连接" : device.Online ? "在线 · 未检测到串流" : "离线", FontSize = 12,
                Foreground = Brush.Parse(device.Online ? "#2246A7" : "#515D72")
            }
        };
        Grid.SetColumn(state, 2);
        row.Children.Add(tile);
        row.Children.Add(info);
        row.Children.Add(state);
        var item = new ListBoxItem { Content = row, Tag = device };
        if (device.CanConnect)
        {
            var manual = new MenuItem { Header = "兼容旧串流客户端 · 手动配对" };
            manual.Click += async (_, _) =>
            {
                Devices.SelectedItem = item;
                await Launch(true);
            };
            item.ContextMenu = new ContextMenu { ItemsSource = new[] { manual } };
        }
        return item;
    }

    private void RenderHost(string error = null)
    {
        if (installing)
        {
            HostBadge.Text = "这台电脑 · 安装中";
            HostTitle.Text = "正在启用本机被控";
            HostHint.Text = "请完成 Windows 安装窗口中的操作。";
            HostAction.Content = "安装中…";
            return;
        }
        if (host == null)
        {
            HostBadge.Text = "这台电脑 · 状态未知";
            HostTitle.Text = "暂时无法检查本机";
            HostHint.Text = "可以重新检查，不会修改已有安装。";
            HostAction.Content = "重新检查";
            ToolTip.SetTip(HostAction, error);
        }
        else if (host.SettingsAvailable)
        {
            HostBadge.Text = "这台电脑 · 被控组件已运行";
            HostTitle.Text = "在极光管理本机被控";
            HostHint.Text = "在此管理 Sunshine 账号、配置和配对。";
            HostAction.Content = "重新检测";
        }
        else if (host.Installed)
        {
            HostBadge.Text = "这台电脑 · 被控组件未就绪";
            HostTitle.Text = "已安装 Sunshine";
            HostHint.Text = "如果 Sunshine 正在运行，极光也会尝试读取自定义管理端口。";
            HostAction.Content = "重新检测";
        }
        else
        {
            HostBadge.Text = "这台电脑 · 尚未启用被控";
            HostTitle.Text = "让其他设备连接这台电脑";
            HostHint.Text = confirmInstall ? "将下载 Sunshine 官方组件，Windows 可能要求管理员确认。" :
                "从这里安装被控组件，无需自己找下载包。";
            HostAction.Content = confirmInstall ? "确认安装 Sunshine" : "启用本机被控";
        }
    }

    private async Task ActivateHost()
    {
        if (!OperatingSystem.IsWindows() || installing) return;
        if (host == null) { await RefreshDevices(); return; }
        if (host.Installed)
        {
            OpenHostSettings();
            await sunshinePage.ReloadIfUnavailable();
            await RefreshDevices();
            return;
        }
        if (!confirmInstall)
        {
            confirmInstall = true;
            RenderHost();
            return;
        }
        confirmInstall = false;
        installing = true;
        RenderHost();
        UpdateActions();
        try
        {
            await Task.Run(Upstream.InstallSunshine);
            SetFeedback("安装完成。组件启动后，可在当前页面管理 Sunshine。");
        }
        catch (Exception error) { SetFeedback("安装未完成：" + error.Message); }
        finally
        {
            installing = false;
            if (!closed)
            {
                RenderHost();
                UpdateActions();
                await RefreshDevices();
            }
        }
    }

    private async Task Launch(bool pairing)
    {
        if (!Connect.IsEnabled || connecting || SelectedDevice == null) return;
        connecting = true;
        UpdateActions();
        var token = lifetime.Token;
        try
        {
            var device = SelectedDevice;
            var route = await Discovery.ChooseRoute(device);
            if (closed) return;
            if (!pairing)
            {
                SetFeedback("正在检查设备连接…");
                if (!await IsPaired(route, token))
                {
                    if (device.Tailnet == null)
                        throw new InvalidOperationException("首次自动配对需要两台设备登录同一 Tailscale 账号。");
                    using var helpTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                    helpTimeout.CancelAfter(TimeSpan.FromSeconds(8));
                    var moonlight = await Upstream.FindBackgroundMoonlight(helpTimeout.Token);
                    SetFeedback("正在与对端极光建立信任…");
                    await AutomaticPairing.Pair(device, moonlight, token);
                    // Try the original LAN choice again now that Moonlight has a pinned host certificate.
                    if (route != device.Tailnet && await IsPaired(route, token))
                    {
                        SetFeedback("已自动配对，正在通过局域网连接…");
                    }
                    else if (await IsPaired(device.Tailnet, token))
                    {
                        route = device.Tailnet;
                    }
                    else
                    {
                        throw new InvalidOperationException("串流组件尚未确认设备信任，请重新连接。");
                    }
                }
            }
            token.ThrowIfCancellationRequested();
            using var process = Upstream.LaunchMoonlight(route, pairing);
            var source = route == device.Lan ? "局域网" : "Tailscale";
            SetFeedback(source + (pairing ? "配对：请在被控端设置中确认显示的 PIN。" : "连接：串流将在独立窗口显示。"));
            await process.WaitForExitAsync(token);
            if (process.ExitCode != 0) SetFeedback($"操作未完成（Moonlight：{process.ExitCode}）。请确认被控端已安装并启动 Sunshine。");
        }
        catch (MoonlightNotFoundException error)
        {
            SetFeedback(error.Message);
            OpenPage(Upstream.MoonlightReleasesUrl);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (OperationCanceledException) { SetFeedback("连接或自动配对超时，请检查两端状态后重试。"); }
        catch (Exception error) { SetFeedback(error.Message); }
        finally { connecting = false; if (!closed) UpdateActions(); }
    }

    private static async Task<bool> IsPaired(StreamHost route, CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(16));
        try { return (await Upstream.ListMoonlight(route, timeout.Token)).ExitCode == 0; }
        catch (OperationCanceledException) when (!token.IsCancellationRequested) { return false; }
    }

    private void OpenPage(string address)
    {
        try { using var process = Process.Start(new ProcessStartInfo(address) { UseShellExecute = true }); }
        catch (Exception error) { SetFeedback("无法打开：" + error.Message); }
    }

    private void ShowPage()
    {
        DevicesPage.IsVisible = Navigation.SelectedIndex is not (1 or 2);
        HostPage.IsVisible = Navigation.SelectedIndex == 1 && OperatingSystem.IsWindows();
        TailnetPage.IsVisible = Navigation.SelectedIndex == 2;
        if (HostPage.IsVisible && host?.Installed == true) OpenHostSettings();
        if (!HostPage.IsVisible) sunshinePage?.ClearUnsubmittedSecrets();
        if (TailnetPage.IsVisible && tailscalePage == null)
        {
            tailscalePage = new TailscalePage();
            TailnetPage.Content = tailscalePage;
        }
        tailscalePage?.SetActive(TailnetPage.IsVisible);
        if (DevicesPage.IsVisible) _ = RefreshDevices();
    }

    private void OpenHostSettings()
    {
        if (sunshinePage != null) return;
        sunshinePage = new SunshinePage();
        sunshinePage.DetailsChanged += showingDetails => HostSummary.IsVisible = !showingDetails;
        HostSettings.Content = sunshinePage;
        _ = sunshinePage.SetNetwork(network);
    }

    private void SetFeedback(string message)
    {
        if (closed) return;
        Feedback.Text = message;
        ToolTip.SetTip(Feedback, message);
    }
}
