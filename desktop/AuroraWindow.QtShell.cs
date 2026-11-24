using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Threading;

namespace Aurora;

public sealed partial class AuroraWindow
{
    private static readonly string QtApplicationVersion = typeof(AuroraWindow).Assembly.GetName().Version?.ToString();

    private QtShellBridge qtShell;
    private bool qtWindowVisible = true;
    private bool qtAvatarVisible;
    private string qtAccountAvatar;
    private bool qtDevicesForeground;
    private int qtWindowActivation;
    private string qtWindowPage;
    private readonly QtShellGeneralSettings qtGeneralSettings = new();
    private (int Version, long? Owner, string LocalId, int? Generation) qtCatalogContext;
    private int qtDeviceEpoch;
    private string qtAccountName = "";
    private bool qtRefreshing;
    private string qtFeedback = "";
    private bool qtFeedbackFailed;

    private (int, long?, string, int?) QtAccountContext =>
        (accountVersion, network?.Owner, network?.LocalId, network?.Generation);
    private bool QtDevicesReady => hasScanned && qtCatalogContext == QtAccountContext &&
        network?.IsAuthenticated == true && network.Generation == TailscaleRuntime.Generation &&
        !TailscaleRuntime.IsTransitioning;

    internal bool StartQtShell(string[] args)
    {
        var index = Array.IndexOf(args ?? Array.Empty<string>(), "--qt-shell");
        QtShellLaunch launch;
        if (index < 0)
        {
            launch = QtShellBridge.FindBundledLaunch(AppContext.BaseDirectory);
            if (launch == null) return false;
        }
        else
        {
            if (index + 2 >= args.Length)
            {
                Console.Error.WriteLine("Usage: --qt-shell <companion executable> <Live.qml>");
                return false;
            }
            launch = new QtShellLaunch(args[index + 1], args[index + 2]);
        }

        try
        {
            qtShell = QtShellBridge.Start(launch.Executable, launch.EntryPoint, request =>
                Dispatcher.UIThread.Post(() => _ = HandleQtShellRequest(request)));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or
            Win32Exception or ArgumentException)
        {
            Console.Error.WriteLine($"Qt Shell could not start: {error.Message}");
            return false;
        }

        if (index < 0 && Environment.GetEnvironmentVariable("AURORA_QT_SESSION") == null)
            Environment.SetEnvironmentVariable("AURORA_QT_SESSION", "1");

        var bridge = qtShell;
        Closed += (_, _) => DetachQtShell(bridge);
        _ = ObserveQtShell(bridge);
        ReadQtTailscaleCapabilities();
        PublishQtShellState();
        return true;
    }

    internal bool TryShowQtWindow(string destination = null)
    {
        if (qtShell == null || closed) return false;
        if (confirmationDecision != null)
        {
            TrayResidency.ShowAvaloniaWindow(this);
            return true;
        }
        if (IsVisible) Hide();
        qtWindowVisible = true;
        qtWindowPage = destination;
        qtWindowActivation++;
        PublishQtShellState();
        return true;
    }

    private void HideQtWindow()
    {
        qtWindowVisible = false;
        qtDevicesForeground = false;
        SetQtAvatarVisibility(false);
        wallpaperRefresh?.Cancel();
        PublishQtShellState();
    }

    private void ShowAvaloniaPage(Page destination)
    {
        HideQtWindow();
        TrayResidency.ShowAvaloniaWindow(this);
        Navigate(destination);
    }

    private async Task ObserveQtShell(QtShellBridge bridge)
    {
        try { await bridge.Completion; }
        finally
        {
            if (qtShell == bridge)
            {
                var shouldShow = qtWindowVisible && !closed;
                DetachQtShell(bridge);
                if (shouldShow) TrayResidency.ShowAvaloniaWindow(this);
            }
        }
    }

    private void DetachQtShell(QtShellBridge bridge)
    {
        if (qtShell != bridge) return;
        qtShell = null;
        SetQtAvatarVisibility(false);
        qtAccountAvatar = null;
        CloseQtTailscaleController();
        bridge.Dispose();
    }

    private void SyncQtShellCatalog()
    {
        if (qtShell == null) return;
        if (qtCatalogContext != QtAccountContext)
        {
            qtCatalogContext = QtAccountContext;
            qtDeviceEpoch++;
            qtFeedback = "";
            qtFeedbackFailed = false;
        }
        PublishQtShellState();
    }

    private void PublishQtShellState()
    {
        if (qtShell == null || closed) return;
        var ready = QtDevicesReady;
        var access = ready ? "ready" : !hasScanned || accountState == TailscaleState.Starting ||
            network?.IsAuthenticated == true ? "loading" : "signedOut";
        var devices = ready ? displayed.Select(device =>
        {
            var path = DesktopWallpaper.CachedPath(device, network);
            var tailAddress = device.Tailnet?.Endpoint.Address ?? (device.TailnetVisible ? device.Address : null);
            return new QtShellDevice(device.Id, device.Name, device.Online, device.CanConnect,
                (device.LanAddress ?? device.Lan?.Endpoint.Address)?.ToString() ?? "", tailAddress?.ToString() ?? "",
                File.Exists(path) ? new Uri(path).AbsoluteUri : "");
        }).ToArray() : Array.Empty<QtShellDevice>();
        qtShell.Publish(new QtShellState(access, qtAccountName, accountSubtitle.Text, devices, qtDeviceEpoch,
            ready && !refreshing && !manualRefresh && !qtRefreshing && !QtTailscaleBlocksDevices, qtRefreshing, qtFeedback, qtFeedbackFailed,
            connecting || QtTailscaleBlocksDevices, RefreshNotice.ReduceMotion())
        {
            Host = ReadQtHostState(),
            ApplicationVersion = QtApplicationVersion,
            AccountAvatar = network?.IsAuthenticated == true && network.Owner == accountOwner &&
                accountState == TailscaleState.Connected && !TailscaleRuntime.IsTransitioning ? qtAccountAvatar : null,
            // Carry the latest preference acknowledgement across device snapshots.
            GeneralSettings = qtGeneralSettings.Snapshot,
            TailscaleAccount = GetQtTailscaleAccount(),
            ManagedWindow = true,
            WindowVisible = qtWindowVisible,
            WindowActivation = qtWindowActivation,
            WindowPage = qtWindowPage
        });
    }

    private void SetQtAvatarVisibility(bool visible)
    {
        qtAvatarVisible = visible && qtWindowVisible;
        if (!CanLoadAccountAvatar)
        {
            avatarRequest?.Cancel();
            return;
        }
        if (accountAvatar == null && avatarRequest == null && accountAvatarUri != null &&
            qtTailscaleStatus is { } status && status.OwnerId == accountOwner && status.AvatarUri == accountAvatarUri)
            _ = LoadAccountAvatar(status);
    }

    private void ReportQtShellFeedback(string message, bool failed)
    {
        if (qtShell == null || closed) return;
        qtFeedback = message;
        qtFeedbackFailed = failed;
        PublishQtShellState();
    }

    private async Task HandleQtShellRequest(QtShellRequest request)
    {
        if (qtShell == null || closed) return;
        if (qtGeneralSettings.TryHandle(request, out var preferencesChanged))
        {
            if (preferencesChanged)
                settings?.RefreshGeneralPreferences();
            PublishQtShellState();
            return;
        }
        switch (request.Action)
        {
            case "refresh" when QtDevicesReady && !refreshing && !manualRefresh && !qtRefreshing && !QtTailscaleBlocksDevices:
                await RefreshDevicesAndReport(Page.Devices);
                break;
            case "connect" when QtDevicesReady && !connecting && !QtTailscaleBlocksDevices:
                var device = request.DeviceEpoch == qtDeviceEpoch && !string.IsNullOrEmpty(request.DeviceId)
                    ? displayed.FirstOrDefault(item => item.Id == request.DeviceId && item.Online && item.CanConnect)
                    : null;
                if (device != null)
                    await Launch(device, null, message => ReportQtShellFeedback(message, failed: true));
                else
                    ReportQtShellFeedback("设备状态已更新，请刷新后重试。", failed: true);
                break;
            case "window-close":
                FinishQtTailscaleConfirmation(approved: false);
                if (TrayResidency.CloseToTray) HideQtWindow();
                else Close();
                break;
            case "avatar-visible":
            case "avatar-hidden":
                SetQtAvatarVisibility(request.Action == "avatar-visible");
                break;
            case "devices-foreground":
                qtDevicesForeground = qtWindowVisible;
                _ = RefreshWallpapers();
                break;
            case "devices-background":
                qtDevicesForeground = false;
                wallpaperRefresh?.Cancel();
                break;
            case "account":
            case string action when action.StartsWith("tailscale.", StringComparison.Ordinal):
                await HandleQtTailscaleRequest(request);
                break;
            case "settings":
                ShowAvaloniaPage(Page.Settings);
                break;
            case "sunshineLoad" when host?.Installed == true && !changingManagedHost.HasValue && !stoppingHost:
                OpenHostSettings(syncNetwork: false);
                await sunshinePage.AttachQt(network);
                break;
            case "sunshineSave" or "sunshineRestart" or "sunshineCertificate" or "sunshineLogin" or
                "sunshineForget" or "sunshineOriginal" or "sunshineReauthorize" or "sunshinePair"
                when host?.Installed == true && !changingManagedHost.HasValue && !stoppingHost &&
                    sunshinePage?.QtAttached == true:
                await sunshinePage.HandleQtRequest(request.Action, request.Sunshine);
                if (request.Action == "sunshineRestart") _ = RefreshDevices();
                break;
            default:
                await HandleQtHostRequest(request.Action);
                break;
        }
    }
}
