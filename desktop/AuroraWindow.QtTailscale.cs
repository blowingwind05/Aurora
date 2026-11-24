using System;
using System.Globalization;
using System.Threading.Tasks;
using Avalonia.Threading;

namespace Aurora;

public sealed partial class AuroraWindow
{
    private TailscaleAccountSession tailscaleAccount;
    private readonly DispatcherTimer tailscaleAccountTimer = new() { Interval = TimeSpan.FromSeconds(3) };
    private bool qtTailscaleOpened;
    private TailscaleStatus qtTailscaleStatus;
    private int qtTailscaleStatusGeneration;
    private bool qtHasSystemTailscale;
    private bool qtSupportsEmbeddedTailscale;
    private QtShellTailscaleConfirmation qtTailscaleConfirmation;
    private TaskCompletionSource<bool> qtTailscaleDecision;
    private int qtTailscaleConfirmationId;
    private string qtTailscaleNotice;
    private int qtTailscaleNoticeVersion;

    private bool QtAccountOwnsPresentation => qtTailscaleOpened && qtShell != null && qtWindowVisible;
    private bool QtTailscaleBlocksDevices => qtTailscaleOpened && tailscaleAccount?.Busy == true;

    private void UpdateQtTailscaleAccount(TailscaleStatus status)
    {
        if (qtShell == null) return;
        qtTailscaleStatus = status;
        qtTailscaleStatusGeneration = TailscaleRuntime.Generation;
        if (!qtTailscaleOpened) ReadQtTailscaleCapabilities();
        PublishQtShellState();
    }

    private void ReadQtTailscaleCapabilities()
    {
        qtHasSystemTailscale = TailscaleRuntime.HasSystemRuntime;
        qtSupportsEmbeddedTailscale = TailscaleRuntime.SupportsEmbedded;
    }

    private QtShellTailscaleAccount GetQtTailscaleAccount()
    {
        if (qtTailscaleOpened && tailscaleAccount != null)
            return tailscaleAccount.ReadQtPresentation() with
            {
                Confirmation = qtTailscaleConfirmation,
                Notice = qtTailscaleNotice,
                NoticeVersion = qtTailscaleNoticeVersion
            };

        var mode = TailscaleRuntime.Mode;
        var state = TailscaleRuntime.IsTransitioning ? TailscaleState.Starting :
            qtTailscaleStatusGeneration == TailscaleRuntime.Generation ? qtTailscaleStatus?.State : null;
        var (status, detail, _) = TailscaleAccountSession.DescribeStatus(state, mode, qtSupportsEmbeddedTailscale);
        return new QtShellTailscaleAccount(mode == TailscaleMode.Embedded ? "embedded" : "system",
            TailscaleAccountSession.DescribeMode(mode, qtHasSystemTailscale), qtSupportsEmbeddedTailscale, qtHasSystemTailscale,
            null, state?.ToString() ?? "Checking", status, detail, "Tailscale", null, null, null, null, "", false, false, false);
    }

    private TailscaleAccountSession EnsureTailscaleAccount()
    {
        if (tailscaleAccount != null) return tailscaleAccount;
        tailscaleAccount = new TailscaleAccountSession(new TailscaleAccountRuntime(),
            action => Dispatcher.UIThread.Post(action), ConfirmLogout, ConfirmTailscaleSwitch,
            () => connecting, ShowTailscaleSwitchSuccess);
        tailscaleAccount.StatusChanged += OnAccountStatusChanged;
        tailscaleAccount.Changed += PublishQtShellState;
        tailscaleAccountTimer.Tick += async (_, _) =>
        {
            if (tailscaleAccount != null) await tailscaleAccount.Poll();
        };
        return tailscaleAccount;
    }

    private void UpdateTailscaleAccountActivity()
    {
        if (tailscaleAccount == null || closed) return;
        var active = qtTailscaleOpened || tailscalePage != null && TailnetPage.IsVisible;
        if (active) tailscaleAccountTimer.Start();
        else tailscaleAccountTimer.Stop();
        _ = tailscaleAccount.SetActive(active);
    }

    private void CloseTailscaleAccount()
    {
        tailscaleAccountTimer.Stop();
        if (tailscaleAccount == null) return;
        tailscaleAccount.StatusChanged -= OnAccountStatusChanged;
        tailscaleAccount.Changed -= PublishQtShellState;
        tailscaleAccount.Dispose();
        tailscaleAccount = null;
    }

    private void OpenQtTailscaleController()
    {
        if (qtTailscaleOpened) return;
        EnsureTailscaleAccount();
        qtTailscaleOpened = true;
        UpdateTailscaleAccountActivity();
        PublishQtShellState();
    }

    private void CloseQtTailscaleController()
    {
        if (!qtTailscaleOpened) return;
        qtTailscaleOpened = false;
        tailscaleAccount?.RevokePendingActions();
        FinishQtTailscaleConfirmation(approved: false);
        qtTailscaleNotice = null;
        UpdateTailscaleAccountActivity();
    }

    private void ShowTailscaleSwitchSuccess(string message)
    {
        if (!QtAccountOwnsPresentation)
        {
            UpdateNotice.ShowRefreshSuccess(message);
            return;
        }
        qtTailscaleNotice = message;
        qtTailscaleNoticeVersion++;
        PublishQtShellState();
    }

    private Task<bool> ConfirmQtTailscale(string title, string body, string label, bool destructive)
    {
        if (!QtAccountOwnsPresentation || closed || qtTailscaleDecision != null) return Task.FromResult(false);
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
        if (QtAccountOwnsPresentation && tailscaleAccount != null)
            await tailscaleAccount.HandleQtAction(request.Action, request.AccountTarget, request.AccountRevision);
    }
}