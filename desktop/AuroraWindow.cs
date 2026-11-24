using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Automation;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace Aurora;

public sealed partial class AuroraWindow : Window
{
    private readonly ToolTip refreshTip;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(10) };
    private readonly CancellationTokenSource lifetime = new();
    private readonly DesktopWallpaper desktopWallpaper = new();
    private AuroraPresence presence;
    private readonly Dictionary<string, DateTime> wallpaperChecks = new();
    private CancellationTokenSource wallpaperRefresh;
    private (Device Device, string Path)? wallpaperAfterSession;
    private NetworkSnapshot network;
    private LanSnapshot lan;
    private Device[] displayed = Array.Empty<Device>();
    private StreamSettingsPage settings;
    private Page page;
    private int deviceColumns;
    private bool hasScanned;

    private enum Page { Devices, Host, Remote, Settings, Utility }
    private readonly List<(Button Button, Control Label, Page? Destination)> navigation = new();
    private readonly ContentControl accountIcon = new() { Width = 24, Height = 24 };
    private readonly TextBlock accountSubtitle = new() { Classes = { "muted" }, FontSize = 11, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly DispatcherTimer refreshMotion = new() { Interval = TimeSpan.FromMilliseconds(32) };
    private readonly RotateTransform refreshRotation = new();
    private TextBlock accountLabel;
    private TailscaleState accountState;
    private long? accountOwner;
    private Uri accountAvatarUri;
    private Bitmap accountAvatar;
    private CancellationTokenSource avatarRequest;
    private int accountVersion;
    private string utilityTitle;
    private TaskCompletionSource<bool> confirmationDecision;
    private IInputElement confirmationReturnFocus;
    private CancellationTokenSource streamAccess;
    private readonly Func<HostStatus> readHost;
    private readonly Func<Task> startManagedHost;
    private readonly Func<Task> stopManagedHost;
    private readonly ManagedHostPreference managedHostPreference;
    private bool allowManagedHostOnLaunch;
    private HostStatus host;
    private SunshinePage sunshinePage;
    private SunshineHostSession sunshine;
    private TailscalePage tailscalePage;
    private bool refreshing;
    private bool manualRefresh;
    private bool? changingManagedHost;
    private bool syncingHostSwitch;
    private int hostRevision;
    private string managedHostError;
    private bool connecting;
    private bool requestingDevicePower;
    private bool closed;
    private bool stoppingHost;
    private bool canClose;

    public AuroraWindow() : this(Upstream.ReadHost, Upstream.StartSunshine, BundledSunshine.StopAsync,
        ManagedHostPreference.ForCurrentUser()) { }

    internal AuroraWindow(Func<HostStatus> readHost, Func<Task> startManagedHost, Func<Task> stopManagedHost)
        : this(readHost, startManagedHost, stopManagedHost, null) { }

    internal AuroraWindow(Func<HostStatus> readHost, Func<Task> startManagedHost, Func<Task> stopManagedHost,
        ManagedHostPreference managedHostPreference)
    {
        this.readHost = readHost;
        this.startManagedHost = startManagedHost;
        this.stopManagedHost = stopManagedHost;
        this.managedHostPreference = managedHostPreference;
        allowManagedHostOnLaunch = managedHostPreference?.Enabled == true;
        InitializeComponent();
        InitializeGamepadDriver();
        var windows = OperatingSystem.IsWindows();
        var mac = OperatingSystem.IsMacOS();
        CaptionButtons.IsVisible = windows;
        HostNav.IsVisible = Upstream.SupportsHost;
        if (windows || mac)
        {
            ExtendClientAreaChromeHints = mac
                ? Avalonia.Platform.ExtendClientAreaChromeHints.PreferSystemChrome | Avalonia.Platform.ExtendClientAreaChromeHints.OSXThickTitleBar
                : Avalonia.Platform.ExtendClientAreaChromeHints.NoChrome;
            ExtendClientAreaTitleBarHeightHint = 52;
            ExtendClientAreaToDecorationsHint = true;
        }
        if (mac)
        {
            Brand.HorizontalAlignment = HorizontalAlignment.Right;
            BrandLatin.FontFamily = FontFamily.Default;
            BrandChinese.FontFamily = new FontFamily("PingFang SC");
            FontFamily = new FontFamily("PingFang SC, Helvetica Neue");
        }
        if (windows)
        {
            using var stream = Avalonia.Platform.AssetLoader.Open(new Uri("avares://Aurora/Assets/Aurora-Windows-32.png"));
            BrandIcon.Source = new Avalonia.Media.Imaging.Bitmap(stream);
            BrandIcon.Width = BrandIcon.Height = 24;
            BrandIcon.HorizontalAlignment = HorizontalAlignment.Right;
        }
        accountLabel = ConfigureNavigation(TailnetNav, "Tailscale 账户", Page.Remote, accountIcon, accountSubtitle);
        ConfigureNavigation(DevicesNav, "我的设备", Page.Devices, AuroraVisual.Icon(AuroraVisual.Monitor, DevicesNav, size: 16, scale: 1.4));
        ConfigureNavigation(HostNav, "本机被控", Page.Host, AuroraVisual.Icon(AuroraVisual.Panel, HostNav, size: 16, scale: 1.4));
        ConfigureNavigation(SettingsNav, "设置", Page.Settings, AuroraVisual.Icon(AuroraVisual.Gear, SettingsNav, size: 16, scale: 1.4));
        ConfigureNavigation(MoreNav, "更多", null, AuroraVisual.Icon(AuroraVisual.Menu, MoreNav, size: 16, scale: 1.4));
        AuroraMore.Attach(MoreNav, OpenUtilityPage, UpdateNotice.ShowRefreshSuccess);
        UpdateAccount(new(TailscaleState.Unknown));
        Refresh.Content = AuroraVisual.Icon(AuroraVisual.Refresh, Refresh, size: 20, strokeThickness: 2);
        refreshTip = AuroraVisual.LabelIcon(Refresh, "刷新设备列表", horizontalOffset: -20);
        refreshMotion.Tick += (_, _) => refreshRotation.Angle = (refreshRotation.Angle + 16) % 360;
        AuroraVisual.SetTextAction(Back, "本机被控", AuroraVisual.ArrowLeft, leading: true);
        DeviceSignInIcon.Content = AuroraVisual.TailnetMark(size: 28);
        EmptyDeviceIcon.Content = AuroraVisual.Icon(AuroraVisual.Monitor, size: 28, strokeThickness: 1.7);
        SetIcon(CopyAddress, AuroraVisual.Copy, "复制局域网地址");
        SetIcon(CopyTailnet, AuroraVisual.Copy, "复制 Tailscale 地址");
        HostIcon.Content = AuroraVisual.HostMark();
        SizeChanged += (_, args) => ResizeShell(args.NewSize.Width);
        DeviceScroll.ScrollChanged += (_, _) => PlaceDevices();
        DeviceScroll.SizeChanged += (_, _) => PlaceDevices();
        DragArea.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            if (e.ClickCount == 2 && !mac) ToggleMaximize();
            else if (e.ClickCount == 1) BeginMoveDrag(e);
        };
        MinimizeWindow.Content = AuroraVisual.Icon(AuroraVisual.Minus, MinimizeWindow, size: 16);
        CloseWindow.Content = AuroraVisual.Icon(AuroraVisual.Close, CloseWindow, size: 18);
        UpdateMaximizeIcon();
        MinimizeWindow.Click += (_, _) => WindowState = WindowState.Minimized;
        MaximizeWindow.Click += (_, _) => ToggleMaximize();
        CloseWindow.Click += (_, _) => Close();
        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty) UpdateMaximizeIcon();
            if ((e.Property == IsVisibleProperty || e.Property == WindowStateProperty) && !CanRefreshWallpapers)
                wallpaperRefresh?.Cancel();
            if ((e.Property == IsVisibleProperty || e.Property == WindowStateProperty) && !CanLoadAccountAvatar)
                avatarRequest?.Cancel();
        };
        Activated += (_, _) =>
        {
            settings?.Refresh();
            _ = RefreshWallpapers();
        };
        Deactivated += (_, _) => wallpaperRefresh?.Cancel();
        Opened += async (_, _) => await InitializeHostAsync();
        Closing += async (_, args) =>
        {
            if (canClose) return;
            if (confirmationDecision != null)
            {
                args.Cancel = true;
                CloseConfirmation(false);
                return;
            }
            if (!stoppingHost && TrayResidency.TryHideOnClose(this, args)) return;
            if (TailscaleRuntime.IsTransitioning || installingGamepadDriver)
            {
                args.Cancel = true;
                return;
            }
            if (sunshine?.Busy == true || sunshinePage?.IsBusy == true)
            {
                args.Cancel = true;
                if (!TryShowQtWindow("host")) Navigate(Page.Host);
                return;
            }
            if (sunshinePage?.HasPendingSettings == true)
            {
                args.Cancel = true;
                if (await sunshinePage.FinishSettingsAsync(allowDiscard: true)) Dispatcher.UIThread.Post(Close);
                else Navigate(Page.Host);
                return;
            }
            if (changingManagedHost.HasValue || stoppingHost)
            {
                args.Cancel = true;
                return;
            }
            if (!BundledSunshine.IsRunning) return;
            args.Cancel = true;
            stoppingHost = true;
            try { await BundledSunshine.StopAsync(); }
            catch (Exception error) { Debug.WriteLine(error); }
            finally
            {
                canClose = true;
                Dispatcher.UIThread.Post(Close);
            }
        };
        Closed += (_, _) =>
        {
            closed = true;
            TailscaleRuntime.ContextChanging -= OnTailscaleContextChanging;
            TailscaleRuntime.ContextChanged -= OnTailscaleContextChanged;
            timer.Stop();
            refreshMotion.Stop();
            lifetime.Cancel();
            lifetime.Dispose();
            accountAvatar?.Dispose();
            presence?.Dispose();
            CloseSunshineSettings();
            tailscalePage?.Dispose();
        };
        TailscaleRuntime.ContextChanging += OnTailscaleContextChanging;
        TailscaleRuntime.ContextChanged += OnTailscaleContextChanged;
        timer.Tick += async (_, _) => await RefreshDevices();
        Refresh.Click += async (_, _) => await RefreshPage();
        Back.Click += async (_, _) =>
        {
            if (sunshinePage != null) await sunshinePage.ReturnToOverview();
        };
        HostAction.Click += async (_, _) => await DetectHost();
        AllowManagedHost.IsCheckedChanged += async (_, _) =>
        {
            if (!syncingHostSwitch) await RequestManagedHostChange(AllowManagedHost.IsChecked == true);
        };
        CopyAddress.Click += async (_, _) => await CopyAddressValue(HostConnectionAddress(lan?.Address), "局域网", Page.Host);
        CopyTailnet.Click += async (_, _) => await CopyAddressValue(HostConnectionAddress(network?.LocalAddress), "Tailscale", Page.Host);
        NetworkHelp.Click += (_, _) => OpenAccount();
        EmptyNetworkHelp.Click += (_, _) => OpenAccount();
        DeviceSignInAction.Click += (_, _) => OpenAccount();
        CancelConfirmation.Click += (_, _) => CloseConfirmation(false);
        ApplyConfirmation.Click += (_, _) => CloseConfirmation(true);
        ConfirmationOverlay.KeyDown += (_, args) =>
        {
            if (args.Key != Key.Escape) return;
            args.Handled = true;
            CloseConfirmation(false);
        };
        ResizeShell(Width);
        RenderDevices(Array.Empty<Device>(), null, null);
        UpdatePageHeader();
    }

    private static void SetIcon(Button button, string geometry, string name)
    {
        button.Content = AuroraVisual.Icon(geometry, button, size: 20);
        AuroraVisual.LabelIcon(button, name, horizontalOffset: -20);
    }

    private TextBlock ConfigureNavigation(Button button, string name, Page? target, Control icon, TextBlock subtitle = null)
    {
        var label = new TextBlock { Classes = { "navLabel" }, Text = name,
            TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        Control labels = subtitle == null ? label : new StackPanel { Children = { label, subtitle } };
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), ColumnSpacing = 14 };
        Grid.SetColumn(labels, 1);
        row.Children.Add(icon);
        row.Children.Add(labels);
        button.Content = row;
        AuroraVisual.Label(button, name);
        if (target is { } destination)
            button.Click += (_, _) =>
            {
                if (destination != Page.Devices || !TryShowQtWindow("devices"))
                    Navigate(destination);
            };
        navigation.Add((button, labels, target));
        return label;
    }

    private void ResizeShell(double width)
    {
        if (width <= 0) return;
        var narrow = width <= 470;
        Shell.ColumnDefinitions[0].Width = new GridLength(narrow ? 43 : width <= 650 ? 144 : 184);
        NavigationTop.Spacing = narrow ? 4 : 2;
        NavigationBottom.Spacing = NavigationTop.Spacing;
        var inset = narrow ? 4 : 13;
        Sidebar.Margin = new Thickness(inset, 13, inset, inset);
        var horizontal = narrow ? 8 : width <= 650 ? 13 : 21;
        var vertical = width <= 650 ? 16 : 20;
        Workspace.Margin = new Thickness(horizontal, vertical, Math.Max(0, horizontal - 8), vertical);
        PageTitle.FontSize = narrow ? 21 : 26;
        HeaderActions.Spacing = narrow ? 6 : 10;
        PageHeader.ColumnSpacing = narrow ? 6 : 10;
        Workspace.Classes.Set("settingsCompact", narrow);
        foreach (var item in navigation)
        {
            item.Button.Classes.Set("compact", narrow);
            item.Label.IsVisible = !narrow;
            ((Grid)item.Button.Content).ColumnSpacing = narrow ? 0 : 14;
        }
    }

    internal async Task InitializeHostAsync()
    {
        if (closed || presence != null) return;
        presence = new AuroraPresence();
        timer.Start();
        await RefreshDevices();
        if (!closed) await RestoreManagedHost();
    }

    internal void OpenApplicationSettings() => Navigate(Page.Settings);

    private async Task RefreshPage()
    {
        if (manualRefresh || (refreshing && page != Page.Remote)) return;
        var activePage = page;
        var spinner = AuroraVisual.Icon(AuroraVisual.LoaderCircle, Refresh, size: 20);
        spinner.RenderTransformOrigin = RelativePoint.Center;
        spinner.RenderTransform = refreshRotation;
        refreshRotation.Angle = 0;
        manualRefresh = true;
        Refresh.Content = spinner;
        UpdateActions();
        if (!RefreshNotice.ReduceMotion()) refreshMotion.Start();
        try
        {
            if (activePage == Page.Remote) await tailscalePage.RefreshStatus();
            else await RefreshDevicesAndReport(activePage);
        }
        finally
        {
            refreshMotion.Stop();
            manualRefresh = false;
            Refresh.Content = AuroraVisual.Icon(AuroraVisual.Refresh, Refresh, size: 20, strokeThickness: 2);
            if (!closed) UpdateActions();
        }
    }

    private async Task RefreshDevicesAndReport(Page activePage)
    {
        if (activePage == Page.Devices && qtShell != null)
        {
            qtRefreshing = true;
            ReportQtShellFeedback("", failed: false);
        }
        try { await RefreshDevicesAndReportCore(activePage); }
        finally
        {
            qtRefreshing = false;
            PublishQtShellState();
        }
    }

    private async Task RefreshDevicesAndReportCore(Page activePage)
    {
        if (await RefreshDevices() is not { } result) return;
        if (activePage == Page.Devices)
        {
            var issues = new[]
            {
                result.LanError == null ? null : "局域网发现失败。",
                result.NetworkError == null ? null : network == null ? "Tailscale 未就绪。" : "未能完成 Tailscale 设备检测。"
            };
            var complete = issues.All(issue => issue == null);
            var message = complete ? $"已刷新，发现 {displayed.Length} 台设备。" :
                string.Concat(issues) + $" 共发现 {displayed.Length} 台设备。";
            ReportQtShellFeedback(message, failed: !complete);
            SetFeedback(message, activePage, transient: complete);
        }
        else if (activePage == Page.Host)
        {
            var running = result.HostError == null && host?.SettingsAvailable == true && !changingManagedHost.HasValue;
            var message = running ? "检测完成，Sunshine 正在运行。" :
                result.HostError != null || host == null ? "暂时无法检测 Sunshine。请检查服务后重试。" :
                host.Installed ? "Sunshine 未运行。启动后可重新检测。" : "尚未启用 Sunshine。";
            if (qtHostAction == "hostRefresh") ReportQtHostFeedback(message, failed: !running);
            SetFeedback(message, activePage, transient: running);
        }
    }

    private void ToggleMaximize() => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void UpdateMaximizeIcon()
    {
        var maximized = WindowState == WindowState.Maximized;
        var geometry = maximized ? AuroraVisual.RestoreWindow : AuroraVisual.Square;
        MaximizeWindow.Content = AuroraVisual.Icon(geometry, MaximizeWindow, size: 14);
        AutomationProperties.SetName(MaximizeWindow, maximized ? "还原窗口" : "最大化");
        ToolTip.SetTip(MaximizeWindow, maximized ? "还原窗口" : "最大化");
    }

    private static string HostConnectionAddress(System.Net.IPAddress address)
    {
        if (address == null) return null;
        return BundledSunshine.IsAvailable
            ? new System.Net.IPEndPoint(address, BundledSunshine.BasePort).ToString()
            : address.ToString();
    }

    private async Task CopyAddressValue(string address, string source, Page target, Action<string, bool> report = null)
    {
        if (address == null || Clipboard == null)
        {
            report?.Invoke("无法复制地址。", true);
            return;
        }
        try
        {
            await Clipboard.SetTextAsync(address);
            var message = $"已复制 {source} 地址。";
            SetFeedback(message, target);
            report?.Invoke(message, false);
        }
        catch (Exception error)
        {
            var message = "复制失败：" + error.Message;
            SetFeedback(message, target);
            report?.Invoke(message, true);
        }
    }

    private void UpdateActions()
    {
        foreach (var card in DeviceCards.Children.OfType<DeviceCard>()) card.SetBusy(connecting);
        CopyAddress.IsEnabled = lan?.Address != null;
        CopyTailnet.IsEnabled = network?.LocalAddress != null;
        HostAction.IsEnabled = !refreshing && !changingManagedHost.HasValue;
        AllowManagedHost.IsEnabled = host?.Managed == true && !changingManagedHost.HasValue;
        Refresh.IsEnabled = !manualRefresh && (!refreshing || page == Page.Remote);
        PublishQtShellState();
    }

    private async Task<(string NetworkError, string LanError, string HostError)?> RefreshDevices()
    {
        if (refreshing || closed || presence == null || TailscaleRuntime.IsTransitioning) return default;
        refreshing = true;
        UpdateActions();
        var accountVersionAtStart = accountVersion;
        var token = lifetime.Token;
        var discover = Task.Run(Upstream.ReadAccountNetwork);
        var hostRevisionAtStart = hostRevision;
        var detect = Upstream.SupportsHost ? Task.Run(readHost) : Task.FromResult<HostStatus>(null);
        string networkError = null;
        string lanError = null;
        string hostError = null;
        TailnetSnapshot tailnet = null;
        var previousNetwork = (network?.Owner, network?.LocalId);
        try
        {
            var account = await discover;
            if (!closed && accountVersionAtStart == accountVersion)
            {
                network = account.Network;
                UpdateAccount(account.Status);
            }
        }
        catch (Exception error)
        {
            networkError = error.Message;
            if (!closed && accountVersionAtStart == accountVersion)
            {
                network = null;
                UpdateAccount(new(TailscaleState.Unknown));
            }
        }
        if (previousNetwork != (network?.Owner, network?.LocalId))
        {
            wallpaperRefresh?.Cancel();
            wallpaperAfterSession = null;
            streamAccess?.Cancel();
        }
        var discoveredNetwork = accountVersionAtStart == accountVersion ? network : null;
        presence.UpdateTailnet(discoveredNetwork);
        try { await EmbeddedTailnet.ConfigureHost(discoveredNetwork, presence, token); }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { return default; }
        catch (Exception error) when (error is IOException or InvalidOperationException or TimeoutException)
        { if (discoveredNetwork?.Mode == TailscaleMode.Embedded) networkError ??= error.Message; }
        var nearby = Task.Run(() => Discovery.ReadLan(presence, token, scan: discoveredNetwork?.IsAuthenticated == true));
        var tailnetProbe = discoveredNetwork == null ? null : Discovery.ReadTailnet(discoveredNetwork, presence, token);
        try { lan = await nearby; }
        catch (Exception error) { lan = null; lanError = error.Message; }
        if (tailnetProbe != null)
        {
            try { tailnet = await tailnetProbe; }
            catch (Exception error) { networkError = error.Message; }
        }
        try
        {
            var detectedHost = await detect;
            if (hostRevisionAtStart == hostRevision && !changingManagedHost.HasValue) host = detectedHost;
        }
        catch (Exception error)
        {
            if (hostRevisionAtStart == hostRevision && !changingManagedHost.HasValue) hostError = error.Message;
        }
        if (closed) return default;
        refreshing = false;
        if (accountVersionAtStart != accountVersion)
        {
            _ = RefreshDevices();
            return default;
        }
        var devices = network?.IsAuthenticated == true
            ? Discovery.Merge(network.Devices, lan, tailnet).Where(device => device.TailnetVisible).ToArray()
            : Array.Empty<Device>();
        RenderDevices(devices, networkError, lanError);
        _ = desktopWallpaper.RefreshHost();
        _ = RefreshWallpapers();
        if (Upstream.SupportsHost)
        {
            if (hostRevisionAtStart == hostRevision && !changingManagedHost.HasValue)
                qtHostDetectionError = hostError;
            RenderHost(hostError);
            if (!changingManagedHost.HasValue && host?.Installed == true)
            {
                var prepareEmbeddedPairing = host.Managed && host.SettingsAvailable &&
                    network?.IsAuthenticated == true && network.Mode == TailscaleMode.Embedded;
                if (HostPage.IsVisible && qtShell == null) OpenHostSettings();
                else if (prepareEmbeddedPairing || AutomaticPairing.IsConfigured) EnsureSunshineSession();
                _ = SyncSunshineNetwork(network);
            }
        }
        UpdateActions();
        return (networkError, lanError, hostError);
    }

    private void RenderDevices(Device[] devices, string networkError, string lanError)
    {
        var signedIn = network?.IsAuthenticated == true;
        if (!signedIn) devices = Array.Empty<Device>();
        var firstScan = !hasScanned;
        hasScanned = true;
        var previewPaths = devices.Select(device => DesktopWallpaper.CachedPath(device, network)).ToArray();
        if (firstScan || !devices.Select(CardState).SequenceEqual(displayed.Select(CardState)) ||
            !previewPaths.SequenceEqual(DeviceCards.Children.OfType<DeviceCard>().Select(card => card.PreviewPath)))
        {
            displayed = devices;
            DeviceCards.Children.Clear();
            for (var i = 0; i < devices.Length; i++)
            {
                var device = devices[i];
                DeviceCards.Children.Add(new DeviceCard(device, i, Launch,
                    (address, source) => CopyAddressValue(device.CanConnect && address != null
                        ? new System.Net.IPEndPoint(address, BundledSunshine.BasePort).ToString()
                        : address?.ToString(), source, Page.Devices), RequestDevicePower, previewPaths[i]));
            }
            PlaceDevices(force: true);
        }
        EmptyState.IsVisible = signedIn && devices.Length == 0;
        DeviceSignIn.IsVisible = !signedIn;
        DeviceScroll.IsVisible = signedIn && devices.Length > 0;
        DevicePlaceholderScroll.IsVisible = !DeviceScroll.IsVisible;
        if (!signedIn) RenderDeviceSignIn();
        NetworkNotice.IsVisible = devices.Length > 0 && (networkError != null || lanError != null);
        NetworkHelp.IsVisible = networkError != null;
        NetworkMessage.Text = string.Join(" ", new[]
        {
            lanError == null ? null : "局域网发现失败，可尝试刷新。",
            networkError == null ? null : network == null
                ? "Tailscale 未就绪。请检查登录状态。"
                : "无法检查 Tailscale 串流服务，可尝试刷新。"
        }.Where(message => message != null));
        ToolTip.SetTip(NetworkMessage, string.Join("\n", new[] { lanError, networkError }.Where(error => error != null)));
        if (page == Page.Devices) UpdatePageHeader();
        SyncQtShellCatalog();
    }

    private void RenderDeviceSignIn()
    {
        (DeviceSignInTitle.Text, DeviceSignInHint.Text, DeviceSignInAction.Content) = accountState switch
        {
            TailscaleState.NeedsLogin => ("登录后连接设备", "登录 Tailscale 验证身份，即可连接局域网和异地设备。", "前往登录"),
            TailscaleState.NotInstalled when TailscaleRuntime.Mode == TailscaleMode.Embedded && TailscaleRuntime.SupportsEmbedded =>
                ("Tailscale 未启用", "启用内置网络服务后，可以连接自己的设备。", "前往启用"),
            TailscaleState.NotInstalled => ("Tailscale 暂不可用", "当前无法连接设备，可查看 Tailscale 状态。", "查看 Tailscale 状态"),
            TailscaleState.Stopped => ("Tailscale 已暂停", "恢复连接后，即可访问自己的设备。", "前往恢复连接"),
            TailscaleState.NeedsApproval => ("等待设备批准", "请联系 Tailscale 网络管理员批准这台设备。", "查看 Tailscale 状态"),
            TailscaleState.Starting => ("Tailscale 正在启动", "连接后设备会自动出现。", "查看 Tailscale 状态"),
            _ => ("连接 Tailscale 后使用设备", "连接设备前，请检查 Tailscale 的状态。", "查看 Tailscale 状态")
        };
    }

    private static object CardState(Device device) =>
        (device.Id, device.Name, device.Address, device.Online, device.LanAddress, device.Lan, device.Tailnet, device.TailnetVisible);

    private bool CanRefreshWallpapers => !closed && !connecting && !refreshing &&
        (qtShell != null && qtWindowVisible ? qtDevicesForeground :
            IsVisible && IsActive && WindowState != WindowState.Minimized && DevicesPage.IsVisible);

    private async Task RefreshWallpapers()
    {
        if (!CanRefreshWallpapers || wallpaperRefresh != null) return;
        using var source = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        wallpaperRefresh = source;
        var token = source.Token;
        var startingNetwork = (network?.Owner, network?.LocalId);
        try
        {
            var now = DateTime.UtcNow;
            foreach (var expired in wallpaperChecks.Where(check => check.Value <= now).Select(check => check.Key).ToArray())
                wallpaperChecks.Remove(expired);
            if (wallpaperAfterSession is { } completed)
            {
                wallpaperAfterSession = null;
                await RefreshWallpaper(completed.Device, completed.Path, token);
                if (token.IsCancellationRequested && startingNetwork == (network?.Owner, network?.LocalId))
                    wallpaperAfterSession ??= completed;
            }
            foreach (var card in DeviceCards.Children.OfType<DeviceCard>().ToArray())
            {
                if (token.IsCancellationRequested || !CanRefreshWallpapers) break;
                if (card.Device.CanConnect && !wallpaperChecks.ContainsKey(card.PreviewPath))
                    await RefreshWallpaper(card.Device, card.PreviewPath, token);
            }
        }
        finally
        {
            wallpaperRefresh = null;
            if (token.IsCancellationRequested && CanRefreshWallpapers)
                Dispatcher.UIThread.Post(() => _ = RefreshWallpapers());
        }
    }

    private async Task RefreshWallpaper(Device device, string path, CancellationToken token)
    {
        if (token.IsCancellationRequested || !CanRefreshWallpapers) return;
        var updated = await DesktopWallpaper.RefreshPreview(path, device, token);
        if (token.IsCancellationRequested || closed) return;
        wallpaperChecks[path] = DateTime.UtcNow.AddMinutes(2);
        if (!updated) return;
        foreach (var card in DeviceCards.Children.OfType<DeviceCard>().Where(card => card.PreviewPath == path))
            card.ReloadPreview();
        PublishQtShellState();
    }

    private void PlaceDevices(bool force = false)
    {
        var width = DeviceScroll.Viewport.Width - DeviceCards.Margin.Left - DeviceCards.Margin.Right;
        var columns = width switch
        {
            >= 1450 => 4,
            >= 1000 => 3,
            > 580 => 2,
            _ => 1
        };
        if (!force && deviceColumns == columns) return;
        deviceColumns = columns;
        DeviceCards.ColumnDefinitions = new ColumnDefinitions(string.Join(",", Enumerable.Repeat("*", columns)));
        DeviceCards.RowDefinitions.Clear();
        for (var i = 0; i < DeviceCards.Children.Count; i++)
        {
            if (i % columns == 0) DeviceCards.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            Grid.SetRow(DeviceCards.Children[i], i / columns);
            Grid.SetColumn(DeviceCards.Children[i], i % columns);
        }
    }

    private void RenderHost(string error = null)
    {
        var managed = host?.Managed == true;
        var running = host?.Installed == true;
        var ready = host?.SettingsAvailable == true;
        var changing = changingManagedHost.HasValue;
        syncingHostSwitch = true;
        AllowManagedHost.IsChecked = managed && (running || allowManagedHostOnLaunch);
        syncingHostSwitch = false;
        ManagedHostRow.IsVisible = managed;
        HostAction.IsVisible = !managed || (allowManagedHostOnLaunch && !running && !changing);
        HostAction.Content = managed ? "重试启动" : "重新检测";
        LocalAddress.Text = HostConnectionAddress(lan?.Address) ?? "—";
        LocalTailnetAddress.Text = HostConnectionAddress(network?.LocalAddress) ?? "—";
        HostSettings.IsVisible = running;
        HostSettings.IsEnabled = !changing;
        HostAddresses.IsVisible = managed ? ready : running;
        HostSetup.IsVisible = changing || managedHostError != null || error != null || (!ready && (!managed || running || allowManagedHostOnLaunch));
        HostDot.Fill = Brush.Parse(ready && !changing ? "#244FC5" : "#79879B");
        HostBadge.Foreground = Brush.Parse(ready && !changing ? "#244FC5" : "#526176");
        HostBadge.Text = changingManagedHost switch
        {
            true => "正在启动",
            false => "正在关闭",
            _ when ready => "运行中",
            _ when managed => running ? "正在就绪" : allowManagedHostOnLaunch ? "未就绪" : "未启用",
            _ => host == null ? "状态未知" : running ? "未就绪" : "未启用"
        };
        HostHint.Text = changingManagedHost switch
        {
            true => "正在启动内置被控组件…",
            false => "正在关闭本机被控…",
            _ => managedHostError ?? error ?? (host == null ? "暂时无法检查 Sunshine。" :
                running ? "Sunshine 就绪后，可在这里管理配置和配对。" :
                managed && allowManagedHostOnLaunch ? "已保存允许被控的设置，可重试启动。" : "此版本缺少内置被控组件。")
        };
    }

    private async Task DetectHost()
    {
        if (changingManagedHost.HasValue) return;
        if (allowManagedHostOnLaunch && host is { Managed: true, Installed: false })
        {
            await RestoreManagedHost();
            return;
        }
        if (host?.Installed == true && !LegacySunshineEditing)
            await EnsureSunshineSession().ReloadIfUnavailable();
        await RefreshDevices();
    }

    private Task RestoreManagedHost()
    {
        if (closed || managedHostPreference == null) return Task.CompletedTask;
        var enabled = managedHostPreference.Enabled;
        if (allowManagedHostOnLaunch != enabled)
        {
            allowManagedHostOnLaunch = enabled;
            RenderHost();
        }
        return allowManagedHostOnLaunch && host is { Managed: true, Installed: false }
            ? ChangeManagedHost(true) : Task.CompletedTask;
    }

    private bool CanChangeManagedHost(bool enable) =>
        host?.Managed == true && !changingManagedHost.HasValue &&
        (host.Installed != enable || managedHostPreference != null && allowManagedHostOnLaunch != enable);

    private async Task RequestManagedHostChange(bool enable)
    {
        RenderHost();
        if (!CanChangeManagedHost(enable)) return;
        if (enable)
        {
            await ChangeManagedHost(true);
            return;
        }
        managedHostError = null;
        RenderHost();
        if (await ConfirmAction("要关闭本机被控吗？", "关闭后会断开正在进行的远控会话。", "关闭被控", destructive: true))
            await ChangeManagedHost(false);
    }

    private async Task ChangeManagedHost(bool enable)
    {
        if (!CanChangeManagedHost(enable)) return;
        if (sunshine?.Busy == true || sunshinePage?.IsBusy == true)
        {
            managedHostError = "请等本机被控设置处理完成后再试。";
            RenderHost();
            AllowManagedHost.Focus();
            return;
        }
        changingManagedHost = enable;
        hostRevision++;
        managedHostError = null;
        RenderHost();
        UpdateActions();
        var applied = false;
        try
        {
            if (!enable && sunshinePage != null && !await sunshinePage.FinishSettingsAsync()) return;
            if (managedHostPreference != null)
            {
                try
                {
                    if (managedHostPreference.Enabled != enable) managedHostPreference.SetEnabled(enable);
                    allowManagedHostOnLaunch = enable;
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    managedHostError = "无法保存允许本机被控的设置。" + error.Message;
                    if (enable) return;
                }
            }
            if (enable)
            {
                if (!host.Installed) await startManagedHost();
            }
            else if (host.Installed)
            {
                sunshinePage?.DismissRestartConfirmation();
                sunshine?.SuspendManagedPairing();
                await stopManagedHost();
            }
            applied = true;
        }
        catch (Exception error)
        {
            managedHostError ??= (enable ? "启动本机被控失败。" : "关闭本机被控失败。") + error.Message;
        }
        finally
        {
            try
            {
                host = await Task.Run(readHost);
                qtHostDetectionError = null;
            }
            catch (Exception error)
            {
                qtHostDetectionError = error.Message;
                managedHostError ??= "无法确认本机被控状态。" + error.Message;
            }
            if (applied && host?.Installed != enable)
                managedHostError ??= "本机被控状态未更新，请重新检测后再试。";
            hostRevision++;
            changingManagedHost = null;
            if (!closed)
            {
                if (host is { Managed: true, Installed: false }) CloseSunshineSettings();
                else if (host?.Installed == true && page == Page.Host && qtShell == null) OpenHostSettings();
                RenderHost();
                UpdateActions();
                if (page == Page.Host) AllowManagedHost.Focus();
                if (applied && managedHostError == null && host?.Installed == enable)
                    SetFeedback(enable ? "已允许本机被控" : "已关闭本机被控", Page.Host, transient: true);
                _ = SyncSunshineNetwork(network);
                _ = RefreshDevices();
            }
        }
    }

    private static async Task VerifyDeviceIdentity(Device device, (long? Owner, string LocalId) account, CancellationToken token)
    {
        var verified = await Task.Run(Upstream.ReadNetwork, token).WaitAsync(token);
        if (!verified.IsAuthenticated || account != (verified.Owner, verified.LocalId) ||
            !verified.Devices.Any(peer => peer.Id == device.Id && Equals(peer.Address, device.Address)))
            throw new InvalidOperationException("Tailscale 账号或设备身份已变化，请重新登录或刷新设备。");
    }

    private async Task RequestDevicePower(Device device, DevicePowerAction action)
    {
        if (requestingDevicePower || closed || !device.SupportsPower || network?.IsAuthenticated != true) return;
        var account = (network?.Owner, network?.LocalId);
        var restart = action == DevicePowerAction.Restart;
        var label = restart ? "重启" : "关机";
        var title = restart ? $"要重启「{device.Name}」吗？" : $"要将「{device.Name}」关机吗？";
        var description = "此操作会断开远程连接。请先保存被控设备上未完成的工作。";
        if (!restart) description += "关机后需要重新开机才能连接。";
        requestingDevicePower = true;
        try
        {
            if (!await ConfirmAction(title, description, "确认" + label, destructive: true) || closed)
                return;
            var token = lifetime.Token;
            await VerifyDeviceIdentity(device, account, token);
            var route = await Discovery.ChoosePowerRoute(device, token);
            await Upstream.RequestDevicePower(route, action, token);
            if (!closed) SetFeedback($"已向「{device.Name}」发送{label}请求。", Page.Devices, transient: true);
        }
        catch (OperationCanceledException) when (closed || lifetime.IsCancellationRequested) { }
        catch (OperationCanceledException) { SetFeedback("未收到被控端确认，请先检查设备状态。", Page.Devices); }
        catch (Exception error) { if (!closed) SetFeedback(error.Message, Page.Devices); }
        finally { requestingDevicePower = false; }
    }

    private Task Launch(Device device) => Launch(device, null);

    private async Task Launch(Device device, StreamSessionWindow previousSession, Action<string> reportToShell = null)
    {
        if (connecting || !device.CanConnect || closed) return;
        if (network?.IsAuthenticated != true)
        {
            OpenAccount();
            return;
        }
        connecting = true;
        wallpaperRefresh?.Cancel();
        var previewPath = DeviceCards.Children.OfType<DeviceCard>().FirstOrDefault(card => card.Device == device)?.PreviewPath;
        var sessionNetwork = (network?.Owner, network?.LocalId);
        UpdateActions();
        using var currentAccess = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        streamAccess = currentAccess;
        var token = currentAccess.Token;
        var session = previousSession;
        EmbeddedTailnet.StreamLease stream = null;
        var keepFailureVisible = false;

        void ReportProgress(string message)
        {
            if (session != null) session.ShowProgress(message);
            else SetFeedback(message);
        }

        void ReportFailure(string message)
        {
            if (token.IsCancellationRequested) return;
            if (session == null)
            {
                SetFeedback(message);
                reportToShell?.Invoke(message);
                return;
            }
            session.ShowFailure(message);
            keepFailureVisible = true;
        }

        try
        {
            session?.ShowConnecting(currentAccess);
            var runtime = Upstream.FindMoonlight();
            if (session == null && OperatingSystem.IsWindows() && await StreamSessionWindow.IsSupported(runtime, token))
            {
                token.ThrowIfCancellationRequested();
                session = new StreamSessionWindow(device.Name, window => Launch(device, window), qtShell?.Launch);
                session.ShowConnecting(currentAccess);
            }
            ReportProgress("正在检查设备连接…");
            await VerifyDeviceIdentity(device, sessionNetwork, token);
            var route = await Discovery.ChooseRoute(device, () => ReportProgress("正在等待被控端检测编码器…"), token);
            if (closed) return;
            ReportProgress("正在确认设备信任…");
            stream = await EmbeddedTailnet.OpenStream(route, token);
            if (!await IsPaired(runtime, stream, token))
            {
                if (device.Tailnet == null)
                    throw new InvalidOperationException("首次自动配对需要两台设备登录同一 Tailscale 账号。");
                using var probeTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                probeTimeout.CancelAfter(TimeSpan.FromSeconds(8));
                var moonlight = await Upstream.FindBackgroundMoonlight(probeTimeout.Token);
                if (device.Tailnet.ViaEmbeddedTailnet) await Upstream.RequireRelayRuntime(moonlight, probeTimeout.Token);
                ReportProgress("正在与对端 Aurora 建立信任…");
                await AutomaticPairing.Pair(device, moonlight, token);
                // Retry LAN after Moonlight has pinned the host certificate.
                var confirmed = await IsPaired(runtime, stream, token);
                if (!confirmed && route != device.Tailnet)
                {
                    stream.Dispose();
                    stream = null;
                    route = device.Tailnet;
                    stream = await EmbeddedTailnet.OpenStream(route, token);
                    confirmed = await IsPaired(runtime, stream, token);
                }
                if (!confirmed) throw new InvalidOperationException("串流组件尚未确认设备信任，请重新连接。");
                if (route == device.Lan) ReportProgress("已自动配对，正在通过局域网连接…");
            }
            token.ThrowIfCancellationRequested();
            var source = route == device.Lan ? "局域网" : "Tailscale";
            int? exitCode;
            if (session != null)
            {
                var result = await session.Run(runtime, stream.Route, token);
                if (result.AttachmentFailed)
                {
                    ReportFailure("无法加载串流画面，请重试。");
                    return;
                }
                exitCode = result.ExitCode;
            }
            else
            {
                exitCode = await LaunchStandalone(runtime, stream.Route, source, device.Name, token);
            }
            if (exitCode == 0 && previewPath != null &&
                (!device.TailnetVisible || sessionNetwork == (network?.Owner, network?.LocalId)))
                wallpaperAfterSession = (device, previewPath);
            if (exitCode is int errorCode && errorCode != 0)
                ReportFailure($"串流组件退出（{errorCode}）。请检查被控端后重新连接。");
        }
        catch (MoonlightNotFoundException error)
        {
            ReportFailure(error.Message);
            OpenPage(Upstream.MoonlightReleasesUrl);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (OperationCanceledException) { ReportFailure("连接或自动配对超时，请检查两端状态后重试。"); }
        catch (Exception error) { ReportFailure(error.Message); }
        finally
        {
            stream?.Dispose();
            if (!keepFailureVisible) session?.Finish();
            streamAccess = null;
            connecting = false;
            if (!closed) UpdateActions();
            _ = RefreshWallpapers();
        }
    }

    private async Task<int> LaunchStandalone(string runtime, StreamHost route, string source, string deviceName, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        using var process = Upstream.LaunchMoonlight(runtime, route, pair: false, deviceName: deviceName);
        SetFeedback(source + "连接：正在打开串流窗口。");
        if (process.StartInfo.RedirectStandardOutput)
        {
            process.OutputDataReceived += (_, data) =>
            {
                if (!StreamPreferences.ReceiveSessionControlChange(data.Data))
                    Dispatcher.UIThread.Post(() =>
                    {
                        if (!closed) SetFeedback("串流设置暂时无法写入磁盘。");
                    });
            };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
        }
        try
        {
            await process.WaitForExitAsync(token);
            return process.ExitCode;
        }
        finally { if (token.IsCancellationRequested) Upstream.StopProcess(process); }
    }

    private static async Task<bool> IsPaired(string runtime, EmbeddedTailnet.StreamLease stream, CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(16));
        try { return (await Upstream.ListMoonlight(runtime, stream, timeout.Token)).ExitCode == 0; }
        catch (OperationCanceledException) when (!token.IsCancellationRequested) { return false; }
    }

    private void OpenPage(string address)
    {
        try { using var process = Process.Start(new ProcessStartInfo(address) { UseShellExecute = true }); }
        catch (Exception error) { SetFeedback("无法打开：" + error.Message); }
    }

    private void OpenAccount()
    {
        Navigate(Page.Remote);
        Dispatcher.UIThread.Post(() => tailscalePage?.FocusPrimaryAction());
    }

    private void OpenUtilityPage(string title, Control content)
    {
        utilityTitle = title;
        UtilityPage.Content = content;
        Navigate(Page.Utility);
    }

    private void UpdateAccount(TailscaleStatus account)
    {
        var connected = account.State == TailscaleState.Connected && network?.IsAuthenticated == true;
        var avatarUri = connected ? account.AvatarUri : null;
        if (account.OwnerId != accountOwner || avatarUri != accountAvatarUri)
        {
            avatarRequest?.Cancel();
            accountIcon.Content = null;
            accountAvatar?.Dispose();
            accountAvatar = null;
            qtAccountAvatar = null;
            accountAvatarUri = avatarUri;
        }
        accountState = account.State;
        accountOwner = account.OwnerId;
        var name = string.IsNullOrWhiteSpace(account.DisplayName) ? account.LoginName : account.DisplayName;
        var accountTitle = string.IsNullOrWhiteSpace(name) ? account.TailnetName : name;
        qtAccountName = connected && !string.IsNullOrWhiteSpace(accountTitle) ? accountTitle : "";
        accountLabel.Text = qtAccountName.Length > 0 ? qtAccountName : "Tailscale 账户";
        const string githubSuffix = "@github";
        var loginName = account.LoginName?.Trim();
        var isGitHub = loginName?.Length > githubSuffix.Length &&
            loginName.EndsWith(githubSuffix, StringComparison.OrdinalIgnoreCase);
        accountSubtitle.Text = connected ? (isGitHub ? "GitHub" : "Tailscale") : account.State switch
        {
            TailscaleState.NotInstalled => "暂不可用",
            TailscaleState.Starting => "正在启动",
            TailscaleState.Stopped => "已暂停",
            TailscaleState.NeedsLogin => "未登录",
            TailscaleState.NeedsApproval => "等待批准",
            _ => "待确认"
        };
        accountIcon.Content = accountAvatar != null ? PresentAccountAvatar(accountAvatar) : connected && !string.IsNullOrWhiteSpace(name)
            ? new Border
            {
                Width = 24, Height = 24, CornerRadius = new CornerRadius(12),
                Background = Brush.Parse("#E0EBF9"),
                Child = new TextBlock
                {
                    Text = System.Globalization.StringInfo.GetNextTextElement(name),
                    Foreground = Brush.Parse("#2350B4"),
                    FontSize = 13, FontWeight = FontWeight.SemiBold,
                    HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
                }
            }
            : AuroraVisual.TailnetMark(TailnetNav, size: 18);
        AuroraVisual.Label(TailnetNav, accountLabel.Text + " · " + accountSubtitle.Text);
        if (accountAvatarUri != null && accountAvatar == null && avatarRequest == null && CanLoadAccountAvatar)
            _ = LoadAccountAvatar(account);
        UpdateQtTailscaleAccount(account);
    }

    private bool CanLoadAccountAvatar => !closed &&
        ((IsVisible && WindowState != WindowState.Minimized) ||
            (qtShell != null && qtWindowVisible && qtAvatarVisible));

    private async Task LoadAccountAvatar(TailscaleStatus account)
    {
        using var request = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        avatarRequest = request;
        try
        {
            var bitmap = await TailscaleAvatar.LoadAsync(account, request.Token);
            if (bitmap == null) return;
            if (request.IsCancellationRequested || !CanLoadAccountAvatar ||
                accountOwner != account.OwnerId || accountAvatarUri != account.AvatarUri)
            {
                bitmap.Dispose();
                return;
            }
            accountAvatar = bitmap;
            accountIcon.Content = PresentAccountAvatar(bitmap);
            if (qtShell != null)
            {
                qtAccountAvatar = TailscaleAvatar.EncodeRound(bitmap);
                PublishQtShellState();
            }
        }
        catch (OperationCanceledException) when (request.IsCancellationRequested) { }
        finally { avatarRequest = null; }
    }

    private static Border PresentAccountAvatar(Bitmap bitmap) => new()
    {
        Width = 24, Height = 24, CornerRadius = new CornerRadius(12),
        Background = new ImageBrush(bitmap) { Stretch = Stretch.UniformToFill }
    };

    private void OnAccountStatusChanged(TailscaleStatus account)
    {
        if (closed) return;
        var changed = account.State != accountState || account.OwnerId != accountOwner;
        if (changed) accountVersion++;
        if (network != null && (account.State != TailscaleState.Connected || account.OwnerId != network.Owner))
            ClearActiveTailnet();
        UpdateAccount(account);
        if (network == null) RenderDevices(Array.Empty<Device>(), null, null);
        if (changed) _ = RefreshDevices();
    }

    private void OnTailscaleContextChanging()
    {
        if (closed) return;
        accountVersion++;
        ClearActiveTailnet();
        UpdateAccount(new(TailscaleState.Starting));
        RenderDevices(Array.Empty<Device>(), null, null);
        RenderHost();
        UpdateActions();
    }

    private void OnTailscaleContextChanged()
    {
        if (!closed) _ = RefreshDevices();
    }

    private void ClearActiveTailnet()
    {
        network = null;
        streamAccess?.Cancel();
        wallpaperRefresh?.Cancel();
        wallpaperAfterSession = null;
        presence?.UpdateTailnet(null);
        _ = ClearEmbeddedHost();
        _ = SyncSunshineNetwork(null);
    }

    private static async Task ClearEmbeddedHost()
    {
        try { await EmbeddedTailnet.ConfigureHost(null); }
        catch (Exception error) when (error is IOException or InvalidOperationException or TimeoutException) { Debug.WriteLine(error.Message); }
    }

    private Task<bool> ConfirmLogout() => ConfirmAction("要退出登录吗？",
        "退出后本机会断开 Tailscale 网络，重新登录后才能连接设备。", "退出登录", destructive: true);

    private Task<bool> ConfirmTailscaleSwitch(string title, string description) =>
        ConfirmAction(title, description, "切换", destructive: false);

    private Task<bool> ConfirmAction(string title, string description, string action, bool destructive)
    {
        if (confirmationDecision != null || closed) return Task.FromResult(false);
        confirmationDecision = new(TaskCreationOptions.RunContinuationsAsynchronously);
        confirmationReturnFocus = FocusManager?.GetFocusedElement();
        ConfirmationTitle.Text = title;
        ConfirmationBody.Text = description;
        ApplyConfirmation.Content = action;
        ApplyConfirmation.Classes.Set("danger", destructive);
        ApplyConfirmation.Classes.Set("accent", !destructive);
        ConfirmationOverlay.IsVisible = true;
        TitleBar.IsEnabled = Shell.IsEnabled = false;
        CancelConfirmation.Focus();
        return confirmationDecision.Task;
    }

    private void CloseConfirmation(bool approved)
    {
        var decision = confirmationDecision;
        if (decision == null) return;
        confirmationDecision = null;
        ConfirmationOverlay.IsVisible = false;
        TitleBar.IsEnabled = Shell.IsEnabled = true;
        if (!approved) confirmationReturnFocus?.Focus();
        confirmationReturnFocus = null;
        decision.TrySetResult(approved);
    }

    private void Navigate(Page target)
    {
        if (target == Page.Host && !Upstream.SupportsHost) return;
        page = target;
        DevicesPage.IsVisible = target == Page.Devices;
        if (target != Page.Devices) wallpaperRefresh?.Cancel();
        HostPage.IsVisible = target == Page.Host;
        TailnetPage.IsVisible = target == Page.Remote;
        SettingsPage.IsVisible = target == Page.Settings;
        UtilityPage.IsVisible = target == Page.Utility;
        for (var i = 0; i < navigation.Count; i++)
            navigation[i].Button.Classes.Set("selected", navigation[i].Destination == target);
        Feedback.Clear();
        if (HostPage.IsVisible && host?.Installed == true) OpenHostSettings();
        if (!HostPage.IsVisible) sunshinePage?.ClearUnsubmittedSecrets();
        if (TailnetPage.IsVisible && tailscalePage == null)
        {
            tailscalePage = new TailscalePage(ConfirmLogout, ConfirmTailscaleSwitch, () => connecting,
                UpdateNotice.ShowRefreshSuccess);
            tailscalePage.StatusChanged += OnAccountStatusChanged;
            TailnetPage.Content = tailscalePage;
        }
        if (SettingsPage.IsVisible)
        {
            if (settings == null) SettingsPage.Content = settings = new StreamSettingsPage();
            else settings.Refresh();
        }
        tailscalePage?.SetActive(TailnetPage.IsVisible);
        UpdatePageHeader();
        UpdateActions();
        if (DevicesPage.IsVisible || HostPage.IsVisible) _ = RefreshDevices();
    }

    private void UpdatePageHeader()
    {
        var detail = page == Page.Host ? sunshinePage?.DetailTitle : null;
        var advanced = detail == "全部配置项";
        PageTitle.Text = detail ?? page switch
        {
            Page.Host => "本机被控", Page.Remote => "Tailscale 账户", Page.Settings => "设置",
            Page.Utility => utilityTitle, _ => "我的设备"
        };
        Refresh.IsVisible = detail == null && (page == Page.Host || page == Page.Remote ||
            page == Page.Devices && network?.IsAuthenticated == true);
        Back.IsVisible = detail != null;
        PageHeader.Margin = new Thickness(0, 0, 8, page == Page.Settings || advanced ? 6 : 15);
        var refreshLabel = page switch
        {
            Page.Host => "重新检测本机被控",
            Page.Remote => "刷新 Tailscale 状态",
            _ => "刷新设备列表"
        };
        AutomationProperties.SetName(Refresh, refreshLabel);
        refreshTip.Content = refreshLabel;
    }

    private bool LegacySunshineEditing =>
        sunshinePage?.HasPendingSettings == true || sunshinePage?.DetailTitle != null || sunshinePage?.IsBusy == true;

    private SunshineHostSession EnsureSunshineSession()
    {
        if (sunshine != null) return sunshine;
        sunshine = new SunshineHostSession();
        sunshine.Changed += PublishQtShellState;
        return sunshine;
    }

    private Task SyncSunshineNetwork(NetworkSnapshot snapshot) =>
        sunshine?.SetNetwork(snapshot, refresh: !LegacySunshineEditing) ?? Task.CompletedTask;

    private void OpenHostSettings()
    {
        if (sunshinePage != null) return;
        sunshinePage = new SunshinePage(EnsureSunshineSession());
        sunshinePage.DetailsChanged += showingDetails =>
        {
            HostSummary.IsVisible = !showingDetails;
            UpdatePageHeader();
        };
        HostSettings.Content = sunshinePage;
        _ = SyncSunshineNetwork(network);
    }

    private void CloseSunshineSettings()
    {
        sunshinePage?.Dispose();
        sunshinePage = null;
        HostSettings.Content = null;
        if (sunshine == null) return;
        sunshine.Changed -= PublishQtShellState;
        sunshine.Dispose();
        sunshine = null;
    }

    private void SetFeedback(string message, Page source = Page.Devices, bool transient = false)
    {
        if (closed || source != page) return;
        if (transient) Feedback.ShowRefreshSuccess(message);
        else Feedback.ShowPersistent(message);
        ToolTip.SetTip(Feedback, message);
    }
}
