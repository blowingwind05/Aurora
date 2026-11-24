using System;
using System.Linq;
using System.Threading.Tasks;

namespace Aurora;

internal sealed partial class SunshineHostSession
{
    private bool qtAttached;
    private QtSunshineConfig qtConfig;
    private QtSunshineSaveResult qtSaveResult;

    internal string StateId { get; private set; } = Guid.NewGuid().ToString("N");
    internal bool QtAttached => qtAttached;

    internal static QtSunshineState QtIdleState(bool managed) => new("idle", "", false, !managed,
        false, false, false, "", "", "", [], null, null);

    internal QtSunshineState QtState
    {
        get
        {
            if (!qtAttached) return null;
            var status = qtConfig != null ? "ready"
                : NeedsCertificate ? "certificate"
                : NeedsAccount ? Access == SunshineAccess.Setup ? "setup" : "login"
                : Busy ? "loading" : "unavailable";
            return new(status, StateId, Busy, !Bundled, RememberAvailable, SavedLogin, ReauthorizeAvailable,
                ConnectionNote ?? "", Feedback ?? "", PairingFeedback ?? "",
                PairingRequests.Select(item => new QtSunshinePairing(item.Id, item.ToString())).ToArray(),
                qtConfig, qtSaveResult);
        }
    }

    internal async Task AttachQt(NetworkSnapshot current, bool legacyEditing)
    {
        if (closed) return;
        if (!qtAttached)
        {
            qtAttached = true;
            Changed?.Invoke();
            await SetNetwork(current, refresh: !legacyEditing);
            if (Overview == null || qtConfig != null) return;
        }
        if (!legacyEditing) await RefreshSettings();
    }

    internal Task HandleQtRequest(string action, QtSunshineRequest request, bool legacyEditing)
    {
        if (request == null || request.Session != StateId || closed) return Task.CompletedTask;
        if (action == "sunshineSave") return SaveQtSettings(request, legacyEditing);
        if (Busy) return Task.CompletedTask;
        switch (action)
        {
            case "sunshineRestart" when qtConfig != null && !legacyEditing:
                return Restart();
            case "sunshineCertificate" when NeedsCertificate:
                return ImportCertificate();
            case "sunshineLogin" when NeedsAccount:
                return SubmitCredentials(request.Username?.Trim() ?? "", request.Password ?? "",
                    request.Confirmation ?? "", request.Remember);
            case "sunshineForget" when SavedLogin:
                return ForgetCredentials();
            case "sunshineOriginal" when !Bundled:
                OpenOriginalSettings();
                break;
            case "sunshineReauthorize" when ReauthorizeAvailable:
                return ReauthorizePairing();
            case "sunshinePair":
                var pairing = PairingRequests.FirstOrDefault(item => item.Id == request.PairingId);
                if (pairing != null) return ApprovePairing(pairing, request.Pin);
                break;
        }
        return Task.CompletedTask;
    }

    private Task SaveQtSettings(QtSunshineRequest request, bool legacyEditing)
    {
        if (request.Id <= 0 || request.Changes == null || request.Changes.Count is 0 or > 110)
            return Task.CompletedTask;
        if (Busy || qtConfig == null || legacyEditing)
        {
            qtSaveResult = new(request.Id, [], request.Changes.Keys.ToDictionary(key => key,
                _ => "Sunshine 正在处理另一项操作。请稍后重试保存。"));
            Changed?.Invoke();
            return Task.CompletedTask;
        }

        return Run(async () =>
        {
            var result = await SunshineQtConfig.SaveAsync(Admin, request.Id, request.Changes);
            try { await ReadOverview(); }
            catch (Exception error)
            {
                foreach (var key in result.Saved)
                    result.Errors[key] = "无法重新读取已提交的设置。" + error.Message;
                result = result with { Saved = [] };
            }
            if (closed) return;
            qtSaveResult = result;
            Feedback = result.Errors.Count == 0 ? "已保存。重启 Sunshine 后生效。" : "";
        }, showProgress: false);
    }

    private void ResetQtSettings()
    {
        qtConfig = null;
        qtSaveResult = null;
        StateId = Guid.NewGuid().ToString("N");
    }
}
