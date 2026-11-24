using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;

namespace Aurora;

internal sealed record SunshineOverview(SunshinePreferences Preferences, SunshinePrivacySettings Displays);

// Owns the local administration session for both desktop frontends. Callers serialize
// requests on the desktop dispatcher, and Busy guards operations across awaits.
internal sealed partial class SunshineHostSession : IDisposable
{
    private readonly Func<bool> isManagedRunning;
    private readonly Func<bool, byte[], SunshineAdmin> openAdmin;
    private SunshineCredentialStore credentials;
    private AutomaticPairing.Host automaticPairing;
    private NetworkSnapshot network;
    private bool closed;

    internal SunshineHostSession() : this(() => BundledSunshine.IsRunning,
        (managed, pin) => managed ? SunshineAdmin.OpenBundled() : SunshineAdmin.OpenLocal(pin)) { }

    internal SunshineHostSession(Func<bool> isManagedRunning, Func<bool, byte[], SunshineAdmin> openAdmin)
    {
        this.isManagedRunning = isManagedRunning;
        this.openAdmin = openAdmin;
    }

    internal event Action Changed;
    internal SunshineAdmin Admin { get; private set; }
    internal SunshineAccess Access { get; private set; }
    internal SunshineOverview Overview { get; private set; }
    internal SunshinePairing[] PairingRequests { get; private set; } = [];
    internal bool Busy { get; private set; }
    internal bool Bundled { get; private set; }
    internal bool NeedsCertificate { get; private set; }
    internal bool NeedsAccount { get; private set; }
    internal bool RememberAvailable => credentials != null;
    internal bool SavedLogin { get; private set; }
    internal bool ReauthorizeAvailable { get; private set; }
    internal string SavedUsername { get; private set; }
    internal string ConnectionNote { get; private set; }
    internal string Feedback { get; private set; }
    internal string PairingFeedback { get; private set; }

    internal Task ReloadIfUnavailable() =>
        Bundled != isManagedRunning() || !NeedsAccount && Overview == null
            ? Run(() => Open()) : Task.CompletedTask;

    internal Task RefreshSettings() => Overview != null && Admin != null && Access == SunshineAccess.SignedIn &&
        Bundled == isManagedRunning() ? Run(ReadOverview) : ReloadIfUnavailable();

    internal async Task SetNetwork(NetworkSnapshot value, bool refresh = true)
    {
        if (closed) return;
        network = value;
        if ((value?.IsAuthenticated != true || automaticPairing?.Matches(value) == false) &&
            (automaticPairing != null || ReauthorizeAvailable || PairingFeedback != null))
        {
            StopAutomaticPairing();
            Changed?.Invoke();
        }
        if (!refresh || Busy) return;
        var runtimeChanged = Bundled != isManagedRunning();
        if (runtimeChanged) StopAutomaticPairing();
        if (runtimeChanged || Admin == null || !NeedsAccount && Overview == null)
            await ReloadIfUnavailable();
        else if (Access == SunshineAccess.SignedIn)
        {
            await UpdateAutomaticPairing();
            await RefreshMoonlightRequests();
            if (!closed) Changed?.Invoke();
        }
    }

    internal Task ImportCertificate() => Run(async () =>
    {
        if (!OperatingSystem.IsWindows() || Bundled || !NeedsCertificate) return;
        try { await Open(await SunshineTrust.ReadElevated()); }
        catch (Win32Exception error) when (error.NativeErrorCode == 1223)
        {
            Feedback = "已取消管理员授权。没有导入证书，也不会发送账号密码。";
        }
    });

    internal Task ForgetCredentials() => Run(() =>
    {
        if (credentials == null || !SavedLogin) return Task.CompletedTask;
        credentials.Forget();
        SavedLogin = false;
        if (Overview != null) ConnectionNote = "已连接本机 Sunshine · 登录只在本次运行期间有效";
        Feedback = "已从 Windows 凭据管理器删除。退出 Aurora 后需要重新登录。";
        return Task.CompletedTask;
    });

    internal void OpenOriginalSettings()
    {
        if (closed || Bundled) return;
        try
        {
            var url = Admin?.SettingsUrl ?? Upstream.SettingsUrl;
            using var process = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
        }
        catch (Exception error) { Feedback = error.Message; }
        Changed?.Invoke();
    }

    internal Task SubmitCredentials(string username, string password, string confirmation, bool remember) => Run(async () =>
    {
        if (!NeedsAccount || Admin == null) return;
        remember &= credentials != null;
        if (Access == SunshineAccess.Setup) await Admin.CreateAccount(username, password, confirmation);
        else await Admin.Login(username, password);
        if (closed) return;
        Access = SunshineAccess.SignedIn;
        if (remember)
        {
            try { credentials.Save(username, password); SavedLogin = true; }
            catch (Exception error) { Feedback = "已登录，但无法记住登录：" + error.Message; }
        }
        else if (SavedLogin)
        {
            try { credentials.Forget(); SavedLogin = false; }
            catch (Exception error) { Feedback = "已登录，但无法清除先前保存的凭据：" + error.Message; }
        }
        await ShowSettings();
    });

    internal Task ApprovePairing(SunshinePairing request, string pin) => Run(async () =>
    {
        if (Admin == null) return;
        await Admin.Pair(request, pin, "Moonlight");
        await RefreshMoonlightRequests();
        Feedback = "Moonlight 已配对。";
    });

    internal Task Restart() => Run(async () =>
    {
        if (Admin == null || Overview == null) return;
        if (!Bundled)
        {
            await Admin.Restart();
            Feedback = "已发送重启请求。请等待服务恢复后再连接。";
            return;
        }
        StopAutomaticPairing();
        try
        {
            await BundledSunshine.RestartAsync();
            await Open();
        }
        catch
        {
            ResetAdmin();
            throw;
        }
        Feedback = "内置 Sunshine 已重启。";
    });

    internal void SuspendManagedPairing()
    {
        StopAutomaticPairing();
        Changed?.Invoke();
    }

    internal Task ReauthorizePairing() => Run(() => UpdateAutomaticPairing(reauthorize: true), showProgress: false);

    private void ResetAdmin()
    {
        StopAutomaticPairing();
        ResetQtSettings();
        Admin?.Dispose();
        Admin = null;
        credentials = null;
        NeedsCertificate = NeedsAccount = SavedLogin = false;
        SavedUsername = null;
        Overview = null;
        PairingRequests = [];
    }

    internal static void OpenMacPrivacySettings()
    {
        var open = new ProcessStartInfo("/usr/bin/open") { UseShellExecute = false };
        open.ArgumentList.Add("x-apple.systempreferences:com.apple.preference.security?Privacy");
        using var process = Process.Start(open);
    }

    private async Task Open(byte[] importedPin = null)
    {
        if (closed) return;
        ResetAdmin();
        Bundled = isManagedRunning();
        ConnectionNote = "正在检查本机 Sunshine…";
        var managed = Bundled;
        var local = await Task.Run(() => openAdmin(managed, importedPin));
        if (closed) { local.Dispose(); return; }
        Admin = local;
        Access = await Admin.CheckAccess();
        if (closed) return;
        if (Bundled)
        {
            if (Access != SunshineAccess.SignedIn)
                throw new InvalidOperationException("内置被控组件未完成 Aurora 托管登录，请重新启用本机被控。");
            await ShowSettings();
            return;
        }

        if (importedPin != null) Admin.RememberVerifiedCertificate();
        if (OperatingSystem.IsWindows()) credentials = new SunshineCredentialStore(Admin.CredentialScope);
        if (Access == SunshineAccess.Login && credentials != null)
        {
            SunshineLogin saved = null;
            try { saved = credentials.Read(); }
            catch (Exception error) { Feedback = "无法读取已保存的登录：" + error.Message; }
            SavedLogin = saved != null;
            if (saved != null)
            {
                try
                {
                    await Admin.Login(saved.Username, saved.Password);
                    Access = SunshineAccess.SignedIn;
                }
                catch (SunshineLoginRejectedException)
                {
                    SavedUsername = saved.Username;
                    Feedback = "已保存的凭据不再有效，请重新登录。勾选「记住登录」可替换旧凭据。";
                }
            }
        }
        if (closed) return;
        if (Access == SunshineAccess.SignedIn) await ShowSettings();
        else
        {
            NeedsAccount = true;
            ConnectionNote = null;
        }
    }

    private async Task ShowSettings()
    {
        await ReadOverview();
        if (closed) return;
        NeedsAccount = false;
        ConnectionNote = Bundled ? null : SavedLogin
            ? "已登录 · 凭据保存在 Windows 凭据管理器"
            : "已登录 · 本次运行期间有效";
        await UpdateAutomaticPairing();
        await RefreshMoonlightRequests();
    }

    private async Task ReadOverview()
    {
        var admin = Admin;
        var preferences = qtAttached ? null : await admin.ReadPreferences();
        var displays = qtAttached ? null : await admin.ReadPrivacySettings();
        if (closed) return;
        if (qtAttached)
        {
            var loaded = await SunshineQtConfig.ReadAsync(admin);
            if (closed) return;
            (qtConfig, preferences, displays) = loaded;
        }
        Overview = new(preferences, displays);
    }

    private async Task RefreshMoonlightRequests()
    {
        var admin = Admin;
        if (closed || !Bundled || Overview == null || admin == null) return;
        SunshinePairing[] requests;
        try { requests = await admin.Pending(); }
        catch (Exception error) when (error is InvalidOperationException or System.Net.Http.HttpRequestException or System.Text.Json.JsonException)
        {
            Debug.WriteLine(error.Message);
            return;
        }
        if (closed || admin != Admin) return;
        PairingRequests = requests.Where(request => !AutomaticPairing.IsBackgroundPairing(request.Name)).ToArray();
    }

    private void StopAutomaticPairing()
    {
        automaticPairing?.Dispose();
        automaticPairing = null;
        PairingFeedback = null;
        ReauthorizeAvailable = false;
    }

    private async Task UpdateAutomaticPairing(bool reauthorize = false)
    {
        if (closed || Admin == null) return;
        var current = network;
        if (current?.IsAuthenticated == true && !AutomaticPairing.SupportsHost(current, Admin))
        {
            StopAutomaticPairing();
            return;
        }
        bool enabled;
        try { enabled = AutomaticPairing.EnsureEnrollment(current, Admin, reauthorize); }
        catch (Exception error)
        {
            StopAutomaticPairing();
            PairingFeedback = "自动配对设置失败。" + error.Message;
            return;
        }
        if (!enabled)
        {
            StopAutomaticPairing();
            if (current?.IsAuthenticated == true && AutomaticPairing.IsConfigured)
            {
                PairingFeedback = Admin.IsManaged
                    ? "账号或被控证书已变更，请确认当前账号的设备访问权限。"
                    : "账号或被控证书已变更，自动配对暂不可用。";
                ReauthorizeAvailable = Admin.IsManaged;
            }
            return;
        }
        if (automaticPairing?.Matches(current) == true) return;
        StopAutomaticPairing();
        var receiver = new AutomaticPairing.Host(Admin, current);
        automaticPairing = receiver;
        try
        {
            await receiver.Start();
            if (automaticPairing == receiver) _ = ObserveAutomaticPairing(receiver);
        }
        catch (Exception error)
        {
            if (automaticPairing != receiver) return;
            StopAutomaticPairing();
            PairingFeedback = error.Message;
        }
    }

    private async Task ObserveAutomaticPairing(AutomaticPairing.Host receiver)
    {
        try { await receiver.Completion; }
        catch (Exception) { }
        if (closed || automaticPairing != receiver) return;
        StopAutomaticPairing();
        Changed?.Invoke();
    }

    private async Task Run(Func<Task> operation, bool showProgress = true)
    {
        if (Busy || closed) return;
        Busy = true;
        const string pending = "正在联系本机 Sunshine…";
        if (showProgress) Feedback = pending;
        Changed?.Invoke();
        try
        {
            await operation();
            if (!closed && showProgress && Feedback == pending) Feedback = "";
        }
        catch (Exception error)
        {
            if (closed) return;
            Feedback = error.Message;
            if (!Bundled && (error is SunshineCertificateAccessException ||
                error is SunshineTlsException && Admin?.SupportsCertificateImport == true))
            {
                NeedsCertificate = true;
                ConnectionNote = "需要管理员确认。Aurora 仅读取公开证书指纹，不读取私钥，也不修改 Sunshine 权限。";
            }
            else if (!NeedsAccount && Overview == null)
                ConnectionNote = Bundled ? "请重新启用本机被控。" : "暂时无法在 Aurora 内管理。可打开 Sunshine 原版设置检查。";
        }
        finally
        {
            Busy = false;
            if (!closed) Changed?.Invoke();
        }
    }

    public void Dispose()
    {
        if (closed) return;
        closed = true;
        ResetAdmin();
    }
}
