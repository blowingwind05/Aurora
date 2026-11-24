using System;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;

namespace Aurora;

internal sealed partial class TailscalePage
{
    private readonly bool externalPresentation;
    private (TailscaleMode Mode, int Generation, bool HasStatus, TailscaleStatus Status, TailscaleProfile[] Profiles) qtSelection;
    private int qtRevision;
    private int qtFocusVersion;

    internal event Action QtPresentationChanged;
    internal bool QtInteractionInProgress => foregroundOperations > 0 || confirmingLogout || TailscaleRuntime.IsTransitioning;

    internal QtShellTailscaleAccount ReadQtPresentation()
    {
        var selection = (mode, TailscaleRuntime.Generation, hasStatus, current, profiles);
        if (selection != qtSelection)
        {
            qtSelection = selection;
            qtRevision++;
        }

        var supportsEmbedded = TailscaleRuntime.SupportsEmbedded;
        var hasSystem = TailscaleRuntime.HasSystemRuntime;
        var currentContext = readVersion == TailscaleRuntime.Generation && mode == TailscaleRuntime.Mode &&
            !TailscaleRuntime.IsTransitioning;
        if (!currentContext)
        {
            var (title, explanation, _) = DescribeStatus(TailscaleState.Starting, mode, supportsEmbedded);
            return Build("Starting", title, explanation, "Tailscale", null, null, null, null, "", []);
        }

        var accountProfiles = mode == TailscaleMode.System
            ? profiles.Select((profile, index) => new QtShellTailscaleProfile(index.ToString(CultureInfo.InvariantCulture),
                string.IsNullOrWhiteSpace(profile.Name) ? profile.LoginName : profile.Name,
                profile.LoginName, profile.Tailnet, profile.IsCurrent)).ToArray() : [];
        return Build(hasStatus ? current.State.ToString() : "Checking", status.Text, details.Text,
            displayName.Text, loginName.Text, initial.Text, networkName.Text, localAddress.Text,
            action.Content as string ?? "", accountProfiles);

        QtShellTailscaleAccount Build(string state, string statusText, string detail, string name, string login,
            string firstLetter, string tailnet, string local, string primary, QtShellTailscaleProfile[] choices)
        {
            var runtimeNotice = OperatingSystem.IsWindows() && !hasSystem
                ? mode == TailscaleMode.Embedded ? "尚未安装系统 Tailscale。Aurora 使用内置 Tailscale。"
                    : "尚未安装系统 Tailscale。"
                : null;
            return new(mode == TailscaleMode.Embedded ? "embedded" : "system", DescribeMode(mode, hasSystem),
                supportsEmbedded, hasSystem, runtimeNotice, state, statusText, detail, name, login, firstLetter,
                tailnet, local, primary, CanChoose && hasStatus && primary.Length > 0, CanChoose, qtRefreshingStatus)
            {
                Revision = qtRevision,
                Busy = QtInteractionInProgress,
                Profiles = choices,
                Progress = switchMessage,
                RuntimeProgress = switchMessage != null && !switchingProfile,
                Feedback = feedback.Message,
                RuntimeFeedback = runtimeFeedback.Content != null,
                RetryAvailable = CanChoose && retry != null,
                FocusVersion = qtFocusVersion
            };
        }
    }

    private bool qtRefreshingStatus;

    internal async Task HandleQtAction(string command, string target, int revision)
    {
        if (!externalPresentation || !CanChoose || revision != ReadQtPresentation().Revision) return;
        switch (command)
        {
            case "tailscale.refresh":
                qtRefreshingStatus = true;
                try { await RefreshStatus(); }
                finally
                {
                    qtRefreshingStatus = false;
                    QtPresentationChanged?.Invoke();
                }
                break;
            case "tailscale.primary":
                await RequestPrimaryAction();
                break;
            case "tailscale.mode" when target == "embedded":
                await RequestModeSwitch(Embedded);
                break;
            case "tailscale.mode" when target == "system":
                await RequestModeSwitch(SystemClient);
                break;
            case "tailscale.profile" when int.TryParse(target, NumberStyles.None, CultureInfo.InvariantCulture, out var index) &&
                index >= 0 && index < profiles.Length:
                await RequestProfileSwitch(profiles[index]);
                break;
            case "tailscale.logout":
                await RequestLogout();
                break;
            case "tailscale.retry" when retry != null:
                await retry();
                break;
        }
    }
}