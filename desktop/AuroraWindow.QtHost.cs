using System;
using System.Threading.Tasks;

namespace Aurora;

public sealed partial class AuroraWindow
{
    private string qtHostAction;
    private string qtHostDetectionError;
    private string qtHostFeedback = "";
    private bool qtHostFeedbackFailed;

    private QtShellHostState ReadQtHostState()
    {
        var available = Upstream.SupportsHost;
        var managed = host?.Managed == true;
        var installed = host?.Installed == true;
        var allowed = managed && (installed || allowManagedHostOnLaunch);
        var ready = host?.SettingsAvailable == true && qtHostDetectionError == null;
        var busy = changingManagedHost.HasValue || qtHostAction != null;
        var tailnetReady = network?.IsAuthenticated == true && network.Generation == TailscaleRuntime.Generation &&
            !TailscaleRuntime.IsTransitioning;
        var status = changingManagedHost switch
        {
            true => "starting",
            false => "stopping",
            _ when qtHostDetectionError != null => "unknown",
            _ when ready => "running",
            _ when host == null => hasScanned ? "unknown" : "checking",
            _ when managed && installed => "preparing",
            _ when installed || allowed => "notReady",
            _ => "disabled"
        };
        var hint = changingManagedHost switch
        {
            true => "正在启动内置被控组件…",
            false => "正在关闭本机被控…",
            _ => managedHostError ?? qtHostDetectionError ??
                (host != null && !managed && !installed ? "此版本缺少内置被控组件。" : "")
        };

        return new QtShellHostState(
            Available: available,
            Status: status,
            Managed: managed,
            Allowed: allowed,
            ToggleAvailable: managed && qtHostDetectionError == null,
            Busy: busy,
            RefreshAvailable: available && !busy && !refreshing && !manualRefresh && presence != null &&
                !TailscaleRuntime.IsTransitioning,
            Refreshing: qtHostAction is "hostRefresh" or "hostContinuePermissions" or "hostRetry",
            RetryAvailable: allowed && !installed && qtHostDetectionError == null,
            LanAddress: ready ? HostConnectionAddress(lan?.Address) ?? "" : "",
            TailscaleAddress: ready && tailnetReady ? HostConnectionAddress(network.LocalAddress) ?? "" : "",
            PermissionsAvailable: OperatingSystem.IsMacOS() && (allowed || changingManagedHost == true),
            SettingsAvailable: installed,
            Hint: hint,
            Feedback: qtHostFeedback,
            FeedbackFailed: qtHostFeedbackFailed)
        {
            Sunshine = installed ? sunshinePage?.QtState ?? SunshinePage.QtIdleState(managed) : null
        };
    }

    private void ReportQtHostFeedback(string message, bool failed)
    {
        qtHostFeedback = message;
        qtHostFeedbackFailed = failed;
        PublishQtShellState();
    }

    private async Task HandleQtHostRequest(string action)
    {
        var state = ReadQtHostState();
        if (!state.Available || state.Busy || confirmationDecision != null || stoppingHost) return;

        var available = action switch
        {
            "hostRefresh" => state.RefreshAvailable,
            "hostEnable" => state.ToggleAvailable && !state.Allowed && CanChangeManagedHost(true),
            "hostDisable" => state.ToggleAvailable && state.Allowed && CanChangeManagedHost(false),
            "hostRetry" => state.RetryAvailable && state.RefreshAvailable,
            "hostCopyLan" => state.LanAddress.Length > 0,
            "hostCopyTailnet" => state.TailscaleAddress.Length > 0,
            "hostOpenPermissions" => state.PermissionsAvailable,
            "hostContinuePermissions" => state.PermissionsAvailable && state.RefreshAvailable,
            _ => false
        };
        if (!available) return;

        qtHostAction = action;
        ReportQtHostFeedback("", failed: false);
        try
        {
            switch (action)
            {
                case "hostRefresh":
                    await RefreshDevicesAndReportCore(Page.Host);
                    if (sunshinePage?.QtAttached == true)
                        await sunshinePage.RefreshQtSettings();
                    break;
                case "hostEnable":
                case "hostDisable":
                    var enable = action == "hostEnable";
                    // The Qt window confirms shutdown before sending hostDisable.
                    await ChangeManagedHost(enable);
                    if (managedHostError == null && host?.Installed == enable)
                        ReportQtHostFeedback(enable ? "已允许本机被控" : "已关闭本机被控", failed: false);
                    break;
                case "hostCopyLan":
                    await CopyAddressValue(state.LanAddress, "局域网", Page.Host, ReportQtHostFeedback);
                    break;
                case "hostCopyTailnet":
                    await CopyAddressValue(state.TailscaleAddress, "Tailscale", Page.Host, ReportQtHostFeedback);
                    break;
                case "hostOpenPermissions":
                    SunshinePage.OpenMacPrivacySettings();
                    ReportQtHostFeedback("已打开 macOS 隐私设置。", failed: false);
                    break;
                case "hostContinuePermissions":
                case "hostRetry":
                    await DetectHost();
                    if (managedHostError == null && qtHostDetectionError == null)
                    {
                        var ready = host?.SettingsAvailable == true;
                        ReportQtHostFeedback(ready ? "检测完成，Sunshine 正在运行。" :
                            "Sunshine 尚未就绪。请检查后重新检测。", failed: !ready);
                    }
                    break;
            }
        }
        catch (Exception error)
        {
            ReportQtHostFeedback("本机被控操作失败。" + error.Message, failed: true);
        }
        finally
        {
            qtHostAction = null;
            PublishQtShellState();
        }
    }
}
