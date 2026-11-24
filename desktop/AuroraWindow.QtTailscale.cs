using System;
using System.Globalization;
using System.Threading.Tasks;

namespace Aurora;

public sealed partial class AuroraWindow
{
    private TailscalePage qtTailscalePage;
    private TailscaleStatus qtTailscaleStatus;
    private int qtTailscaleStatusGeneration;
    private bool qtHasSystemTailscale;
    private bool qtSupportsEmbeddedTailscale;
    private QtShellTailscaleConfirmation qtTailscaleConfirmation;
    private TaskCompletionSource<bool> qtTailscaleDecision;
    private int qtTailscaleConfirmationId;
    private string qtTailscaleNotice;
    private int qtTailscaleNoticeVersion;

    private bool QtTailscaleBlocksDevices => qtTailscalePage?.QtInteractionInProgress == true;

    private void UpdateQtTailscaleAccount(TailscaleStatus status)
    {
        if (qtShell == null) return;
        qtTailscaleStatus = status;
        qtTailscaleStatusGeneration = TailscaleRuntime.Generation;
        if (qtTailscalePage == null) ReadQtTailscaleCapabilities();
        PublishQtShellState();
    }

    private void ReadQtTailscaleCapabilities()
    {
        qtHasSystemTailscale = TailscaleRuntime.HasSystemRuntime;
        qtSupportsEmbeddedTailscale = TailscaleRuntime.SupportsEmbedded;
    }

    private QtShellTailscaleAccount GetQtTailscaleAccount()
    {
        if (qtTailscalePage != null)
            return qtTailscalePage.ReadQtPresentation() with
            {
                Confirmation = qtTailscaleConfirmation,
                Notice = qtTailscaleNotice,
                NoticeVersion = qtTailscaleNoticeVersion
            };

        var mode = TailscaleRuntime.Mode;
        var state = TailscaleRuntime.IsTransitioning ? TailscaleState.Starting :
            qtTailscaleStatusGeneration == TailscaleRuntime.Generation ? qtTailscaleStatus?.State : null;
        var (status, detail, _) = TailscalePage.DescribeStatus(state, mode, qtSupportsEmbeddedTailscale);
        return new QtShellTailscaleAccount(mode == TailscaleMode.Embedded ? "embedded" : "system",
            TailscalePage.DescribeMode(mode, qtHasSystemTailscale), qtSupportsEmbeddedTailscale, qtHasSystemTailscale,
            null, state?.ToString() ?? "Checking", status, detail, "Tailscale", null, null, null, null, "", false, false, false);
    }

    private void OpenQtTailscaleController()
    {
        if (qtTailscalePage != null) return;
        qtTailscalePage = new TailscalePage(
            () => ConfirmQtTailscale("要退出登录吗？", "退出后本机会断开 Tailscale 网络，重新登录后才能连接设备。",
                "退出登录", destructive: true),
            (title, description) => ConfirmQtTailscale(title, description, "切换", destructive: false),
            () => connecting,
            message =>
            {
                qtTailscaleNotice = message;
                qtTailscaleNoticeVersion++;
                PublishQtShellState();
            }, externalPresentation: true);
        qtTailscalePage.StatusChanged += OnAccountStatusChanged;
        qtTailscalePage.QtPresentationChanged += PublishQtShellState;
        qtTailscalePage.SetActive(true);
    }

    private void CloseQtTailscaleController()
    {
        FinishQtTailscaleConfirmation(approved: false);
        if (qtTailscalePage == null) return;
        qtTailscalePage.StatusChanged -= OnAccountStatusChanged;
        qtTailscalePage.QtPresentationChanged -= PublishQtShellState;
        qtTailscalePage.Dispose();
        qtTailscalePage = null;
        qtTailscaleNotice = null;
    }

    private Task<bool> ConfirmQtTailscale(string title, string body, string label, bool destructive)
    {
        if (qtShell == null || closed || qtTailscaleDecision != null) return Task.FromResult(false);
        var decision = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        qtTailscaleDecision = decision;
        qtTailscaleConfirmation = new(++qtTailscaleConfirmationId, title, body, label, destructive);
        PublishQtShellState();
        return decision.Task;
    }

    private void FinishQtTailscaleConfirmation(bool approved)
    {
        var decision = qtTailscaleDecision;
        if (decision == null) return;
        qtTailscaleDecision = null;
        qtTailscaleConfirmation = null;
        decision.TrySetResult(approved);
        PublishQtShellState();
    }

    private async Task HandleQtTailscaleRequest(QtShellRequest request)
    {
        if (request.Action is "account" or "tailscale.enter")
        {
            OpenQtTailscaleController();
            return;
        }
        if (request.Action is "tailscale.confirm" or "tailscale.cancel")
        {
            if (int.TryParse(request.AccountTarget, NumberStyles.None, CultureInfo.InvariantCulture, out var id) &&
                qtTailscaleConfirmation?.Id == id)
                FinishQtTailscaleConfirmation(request.Action == "tailscale.confirm");
            return;
        }
        if (qtTailscalePage != null)
            await qtTailscalePage.HandleQtAction(request.Action, request.AccountTarget, request.AccountRevision);
    }
}