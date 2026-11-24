using System;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;

namespace Aurora;

internal sealed partial class TailscaleAccountSession
{
    private (TailscaleMode Mode, int Generation, int Interaction, bool HasStatus, TailscaleStatus Status, TailscaleProfile[] Profiles) qtSelection;
    private int qtRevision;

    internal QtShellTailscaleAccount ReadQtPresentation()
    {
        var selection = (mode, runtime.Generation, interactionEpoch, hasStatus, current, profiles);
        if (selection != qtSelection)
        {
            qtSelection = selection;
            qtRevision++;
        }

        var view = ReadView();
        var hasSystem = runtime.HasSystemRuntime;
        var runtimeNotice = OperatingSystem.IsWindows() && !hasSystem
            ? mode == TailscaleMode.Embedded ? "尚未安装系统 Tailscale。Aurora 使用内置 Tailscale。"
                : "尚未安装系统 Tailscale。"
            : null;
        var choices = Profiles.Select((profile, index) => new QtShellTailscaleProfile(index.ToString(CultureInfo.InvariantCulture),
            string.IsNullOrWhiteSpace(profile.Name) ? profile.LoginName : profile.Name,
            profile.LoginName, profile.Tailnet, profile.IsCurrent)).ToArray();

        return new(mode == TailscaleMode.Embedded ? "embedded" : "system", DescribeMode(mode, hasSystem),
            runtime.SupportsEmbedded, hasSystem, runtimeNotice, view.State?.ToString() ?? "Checking",
            view.Status, view.Detail, view.DisplayName, view.LoginName, view.Initial, view.TailnetName, view.LocalAddress,
            view.PrimaryAction, CanChoose && hasStatus && view.PrimaryAction.Length > 0, CanChoose, refreshing)
        {
            Revision = qtRevision,
            Busy = Busy,
            Profiles = choices,
            Progress = Progress,
            RuntimeProgress = RuntimeProgress,
            Feedback = Feedback?.Message,
            RuntimeFeedback = Feedback?.Runtime == true,
            RetryAvailable = CanChoose && RetryAvailable,
            FocusVersion = primaryFocusVersion
        };
    }

    internal Task HandleQtAction(string command, string target, int revision)
    {
        if (!CanChoose || revision != ReadQtPresentation().Revision) return Task.CompletedTask;
        return command switch
        {
            "tailscale.refresh" => RefreshStatus(),
            "tailscale.primary" => RequestPrimaryAction(),
            "tailscale.mode" when target == "embedded" => RequestModeSwitch(TailscaleMode.Embedded),
            "tailscale.mode" when target == "system" => RequestModeSwitch(TailscaleMode.System),
            "tailscale.profile" when int.TryParse(target, NumberStyles.None, CultureInfo.InvariantCulture, out var index) &&
                index >= 0 && index < profiles.Length => RequestProfileSwitch(profiles[index]),
            "tailscale.logout" => RequestLogout(),
            "tailscale.retry" => RequestRetry(),
            _ => Task.CompletedTask
        };
    }
}
