using System;
using System.Linq;
using System.Threading.Tasks;

namespace Aurora;

public sealed partial class SunshinePage
{
    private bool qtAttached;
    private string qtSession = Guid.NewGuid().ToString("N");
    private QtSunshineConfig qtConfig;
    private QtSunshineSaveResult qtSaveResult;

    internal event Action QtChanged;
    internal bool QtAttached => qtAttached;

    internal static QtSunshineState QtIdleState(bool managed) => new("idle", "", false, !managed,
        false, false, false, "", "", "", [], null, null);

    internal QtSunshineState QtState
    {
        get
        {
            if (!qtAttached) return null;
            var status = qtConfig != null ? "ready"
                : ImportCert.IsVisible ? "certificate"
                : AccountPanel.IsVisible ? access == SunshineAccess.Setup ? "setup" : "login"
                : busy ? "loading" : "unavailable";
            var pending = MoonlightRequest.ItemsSource as SunshinePairing[] ?? [];
            return new(status, qtSession, busy, !bundled, credentials != null, ForgetLogin.IsVisible,
                PairingReauthorize.IsVisible, ConnectionNote.IsVisible ? ConnectionNote.Text ?? "" : "",
                Feedback.Text ?? "", PairingFeedback.Text ?? "",
                pending.Select(item => new QtSunshinePairing(item.Id, item.ToString())).ToArray(), qtConfig, qtSaveResult);
        }
    }

    internal async Task AttachQt(NetworkSnapshot current)
    {
        var newlyAttached = !qtAttached;
        qtAttached = true;
        if (newlyAttached)
        {
            QtChanged?.Invoke();
            await SetNetwork(current);
            if (!SettingsPanel.IsVisible || qtConfig != null) return;
        }
        await RefreshQtSettings();
    }

    internal Task RefreshQtSettings()
    {
        if (closed || busy || HasPendingSettings || DetailTitle != null) return Task.CompletedTask;
        if (SettingsPanel.IsVisible && admin != null && access == SunshineAccess.SignedIn &&
            bundled == BundledSunshine.IsRunning)
            return Run(ReadOverviewFromSunshine);
        return ReloadIfUnavailable();
    }

    internal async Task HandleQtRequest(string action, QtSunshineRequest request)
    {
        if (request == null || request.Session != qtSession || closed) return;
        if (action == "sunshineSave")
        {
            await SaveQtSettings(request);
            return;
        }
        if (busy) return;
        switch (action)
        {
            case "sunshineRestart" when qtConfig != null && !HasPendingSettings && DetailTitle == null:
                await Run(RestartHost);
                break;
            case "sunshineCertificate" when ImportCert.IsVisible:
                await ImportCertificate();
                break;
            case "sunshineLogin" when AccountPanel.IsVisible && admin != null:
                await SubmitCredentials(request.Username?.Trim() ?? "", request.Password ?? "",
                    request.Confirmation ?? "", request.Remember);
                break;
            case "sunshineForget" when ForgetLogin.IsVisible && credentials != null:
                await ForgetCredentials();
                break;
            case "sunshineOriginal" when !bundled:
                OpenOriginalSettings();
                QtChanged?.Invoke();
                break;
            case "sunshineReauthorize" when PairingReauthorize.IsVisible:
                await Run(() => UpdateAutomaticPairing(reauthorize: true), showProgress: false);
                break;
            case "sunshinePair" when MoonlightRequest.ItemsSource is SunshinePairing[] pending:
                var pairing = pending.FirstOrDefault(item => item.Id == request.PairingId);
                if (pairing != null) await ApprovePairing(pairing, request.Pin);
                break;
        }
    }

    private async Task SaveQtSettings(QtSunshineRequest request)
    {
        if (request.Id <= 0 || request.Changes == null || request.Changes.Count is 0 or > 110) return;
        if (busy || qtConfig == null || HasPendingSettings || DetailTitle != null)
        {
            qtSaveResult = new(request.Id, [], request.Changes.Keys.ToDictionary(key => key,
                _ => "Sunshine 正在处理另一项操作。请稍后重试保存。"));
            QtChanged?.Invoke();
            return;
        }

        await Run(async () =>
        {
            var result = await SunshineQtConfig.SaveAsync(admin, request.Id, request.Changes);
            try { await ReadOverviewFromSunshine(); }
            catch (Exception error)
            {
                foreach (var key in result.Saved)
                    result.Errors[key] = "无法重新读取已提交的设置。" + error.Message;
                result = result with { Saved = [] };
            }
            qtSaveResult = result;
            Feedback.Text = result.Errors.Count == 0 ? "已保存。重启 Sunshine 后生效。" : "";
        }, showProgress: false);
    }
}
