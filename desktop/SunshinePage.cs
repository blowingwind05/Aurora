using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Threading.Tasks;
using Avalonia.Controls;

namespace Aurora;

public sealed partial class SunshinePage : UserControl, IDisposable
{
    private SunshineAdmin admin;
    private SunshineCredentialStore credentials;
    private SunshineOptionsPage optionsPage;
    private AutomaticPairing.Host automaticPairing;
    private NetworkSnapshot network;
    private SunshineAccess access;
    private bool busy;
    private bool closed;
    private bool confirmRestart;
    public event Action<bool> DetailsChanged;

    public SunshinePage()
    {
        InitializeComponent();
        Loaded += async (_, _) => await ReloadIfUnavailable();
        ImportCert.Click += async (_, _) => await Run(async () =>
        {
            if (!OperatingSystem.IsWindows()) return;
            try { await Open(await SunshineTrust.ReadElevated()); }
            catch (Win32Exception error) when (error.NativeErrorCode == 1223)
            {
                Feedback.Text = "已取消管理员授权。没有导入证书，也不会发送账号密码。";
            }
        });
        ForgetLogin.Click += async (_, _) => await Run(() =>
        {
            credentials.Forget();
            ForgetLogin.IsVisible = false;
            RememberLogin.IsChecked = false;
            if (SettingsPanel.IsVisible) Subtitle.Text = "已连接本机 Sunshine · 登录只在本次运行期间有效";
            Feedback.Text = "已从 Windows 凭据管理器删除。退出极光后需要重新登录。";
            return Task.CompletedTask;
        });
        OriginalPage.Click += (_, _) =>
        {
            try
            {
                using var process = Process.Start(new ProcessStartInfo(admin?.SettingsUrl ?? Upstream.SettingsUrl) { UseShellExecute = true });
            }
            catch (Exception error) { Feedback.Text = error.Message; }
        };
        AccountAction.Click += async (_, _) => await Run(async () =>
        {
            var username = Username.Text?.Trim() ?? "";
            var password = Password.Text ?? "";
            var confirmation = Confirmation.Text ?? "";
            var remember = RememberLogin.IsChecked == true;
            Password.Text = Confirmation.Text = "";
            if (access == SunshineAccess.Setup) await admin.CreateAccount(username, password, confirmation);
            else await admin.Login(username, password);
            access = SunshineAccess.SignedIn;
            if (!closed && remember)
            {
                try { credentials.Save(username, password); ForgetLogin.IsVisible = true; }
                catch (Exception error) { Feedback.Text = "已登录，但无法记住登录：" + error.Message; }
            }
            else if (!closed && ForgetLogin.IsVisible)
            {
                try { credentials.Forget(); ForgetLogin.IsVisible = false; }
                catch (Exception error) { Feedback.Text = "已登录，但无法清除先前保存的凭据：" + error.Message; }
            }
            await ShowSettings();
        });
        DetailedSettings.Click += async (_, _) => await Run(async () =>
        {
            optionsPage = new SunshineOptionsPage(admin, ReturnToOverview);
            Options.Content = optionsPage;
            Overview.IsVisible = false;
            Options.IsVisible = true;
            DetailsChanged?.Invoke(true);
            await optionsPage.LoadOptions();
        });
        Save.Click += async (_, _) => await Run(async () =>
        {
            await admin.SavePreferences(HostName.Text?.Trim() ?? "", StreamAudio.IsChecked == true);
            Feedback.Text = "已保存。重启 Sunshine 后生效。";
        });
        Restart.Click += async (_, _) => await Run(async () =>
        {
            if (!confirmRestart)
            {
                confirmRestart = true;
                Restart.Content = "确认重启并断开串流";
                Feedback.Text = "再次点击确认重启，当前串流会断开。";
                return;
            }
            confirmRestart = false;
            Restart.Content = "重启 Sunshine";
            await admin.Restart();
            Feedback.Text = "已发送重启请求。请等待服务恢复后再连接。";
        });
        ReloadPairs.Click += async (_, _) => await Run(LoadPairs);
        AutoPairToggle.Click += async (_, _) =>
        {
            if (busy || closed || admin == null) return;
            await Run(async () =>
            {
                var enabled = AutoPairToggle.IsChecked == true;
                try
                {
                    if (enabled && network == null) throw new InvalidOperationException("请先在本机登录 Tailscale。");
                    AutomaticPairing.SetConfigured(enabled ? network : null, admin);
                    await UpdateAutomaticPairing();
                }
                catch
                {
                    AutoPairToggle.IsChecked = AutomaticPairing.IsConfigured;
                    throw;
                }
            });
        };
        Pairings.SelectionChanged += (_, _) =>
        {
            ClientName.Text = Pairings.SelectedItem is SunshinePairing pairing ? pairing.Name : "";
            UpdateButtons();
        };
        ApprovePair.Click += async (_, _) => await Run(async () =>
        {
            var pin = Pin.Text ?? "";
            Pin.Text = "";
            await admin.Pair(Pairings.SelectedItem as SunshinePairing, pin, ClientName.Text?.Trim() ?? "");
            Feedback.Text = "Sunshine 已接受 PIN，请回到主控端继续。";
            await LoadPairs();
        });
        UpdateButtons();
    }

    public Task ReloadIfUnavailable() => optionsPage == null && !AccountPanel.IsVisible && !SettingsPanel.IsVisible
        ? Run(() => Open()) : Task.CompletedTask;

    private Task ReturnToOverview()
    {
        Options.IsVisible = false;
        Options.Content = null;
        optionsPage = null;
        Overview.IsVisible = true;
        DetailsChanged?.Invoke(false);
        return Run(ShowSettings);
    }

    public bool CanLeaveApp() => !busy && (optionsPage?.CanLeave() ?? true);

    public void ClearUnsubmittedSecrets() => Password.Text = Confirmation.Text = Pin.Text = "";

    public void Dispose()
    {
        closed = true;
        ClearUnsubmittedSecrets();
        StopAutomaticPairing();
        admin?.Dispose();
    }

    internal async Task SetNetwork(NetworkSnapshot value)
    {
        network = value;
        if (closed || busy) return;
        if (admin == null || !AccountPanel.IsVisible && !SettingsPanel.IsVisible)
            await ReloadIfUnavailable();
        else if (access == SunshineAccess.SignedIn)
            await UpdateAutomaticPairing();
    }

    private async Task Open(byte[] importedPin = null)
    {
        if (!OperatingSystem.IsWindows() || closed) return;
        StopAutomaticPairing();
        admin?.Dispose();
        admin = null;
        credentials = null;
        ForgetLogin.IsVisible = AccountPanel.IsVisible = SettingsPanel.IsVisible = false;
        RememberLogin.IsChecked = false;
        var local = await Task.Run(() => SunshineAdmin.OpenLocal(importedPin));
        if (closed) { local.Dispose(); return; }
        admin = local;
        access = await admin.CheckAccess();
        if (closed) return;
        if (importedPin != null) admin.RememberVerifiedCertificate();
        ImportCert.IsVisible = false;
        credentials = new SunshineCredentialStore(admin.CredentialScope);
        if (access == SunshineAccess.Login)
        {
            SunshineLogin saved = null;
            try { saved = credentials.Read(); }
            catch (Exception error) { Feedback.Text = "无法读取已保存的登录：" + error.Message; }
            ForgetLogin.IsVisible = saved != null;
            if (saved != null)
            {
                try
                {
                    await admin.Login(saved.Username, saved.Password);
                    access = SunshineAccess.SignedIn;
                }
                catch (SunshineLoginRejectedException)
                {
                    Username.Text = saved.Username;
                    Feedback.Text = "已保存的凭据不再有效，请重新登录。勾选「记住登录」可替换旧凭据。";
                }
            }
        }
        if (closed) return;
        if (access == SunshineAccess.SignedIn) await ShowSettings();
        else ShowAccount();
    }

    private void ShowAccount()
    {
        var setup = access == SunshineAccess.Setup;
        Subtitle.Text = setup ? "首次启用：在这里创建 Sunshine 本机管理账号。" : "使用你已有的 Sunshine 管理账号登录。";
        AccountTitle.Text = setup ? "创建管理账号" : "登录 Sunshine";
        AccountAction.Content = setup ? "创建并登录" : "登录";
        Confirmation.IsVisible = setup;
        AccountPanel.IsVisible = true;
    }

    private async Task ShowSettings()
    {
        var preferences = await admin.ReadPreferences();
        if (closed) return;
        HostName.Text = preferences.Name;
        StreamAudio.IsChecked = preferences.StreamAudio;
        AccountPanel.IsVisible = false;
        SettingsPanel.IsVisible = true;
        Subtitle.Text = ForgetLogin.IsVisible
            ? "已连接本机 Sunshine · 登录保存在 Windows 凭据管理器"
            : "已连接本机 Sunshine · 登录只在本次运行期间有效";
        await UpdateAutomaticPairing();
        try { await LoadPairs(); }
        catch (Exception error) { PairHint.Text = error.Message + " 可用下方的原版设置继续。"; }
    }

    private void StopAutomaticPairing()
    {
        automaticPairing?.Dispose();
        automaticPairing = null;
    }

    private async Task UpdateAutomaticPairing()
    {
        if (closed || admin == null) return;
        var current = network;
        var enabled = AutomaticPairing.IsConfiguredFor(current, admin);
        AutoPairToggle.IsChecked = AutomaticPairing.IsConfigured;
        if (!enabled)
        {
            StopAutomaticPairing();
            AutoPairStatus.Text = current?.LocalAddress == null ? "连接 Tailscale 后，可允许自己的设备自动配对。"
                : AutomaticPairing.IsConfigured ? "当前账号或 Sunshine 证书已变更。关闭后重新开启可更新授权。"
                : "开启后，对端极光点「连接桌面」即可。被控电脑需运行极光。";
            return;
        }
        if (automaticPairing?.Matches(current) == true) return;
        StopAutomaticPairing();
        var receiver = new AutomaticPairing.Host(admin, current);
        automaticPairing = receiver;
        AutoPairStatus.Text = "正在启动自动配对…";
        try
        {
            await receiver.Start();
            if (automaticPairing != receiver) return;
            AutoPairStatus.Text = "已就绪。自己的 Tailscale 设备可以直接从极光连接。";
            _ = ObserveAutomaticPairing(receiver);
        }
        catch (Exception error)
        {
            if (automaticPairing != receiver) return;
            StopAutomaticPairing();
            AutoPairStatus.Text = error.Message;
        }
    }

    private async Task ObserveAutomaticPairing(AutomaticPairing.Host receiver)
    {
        try { await receiver.Completion; }
        catch (Exception) { }
        if (closed || automaticPairing != receiver) return;
        StopAutomaticPairing();
        AutoPairStatus.Text = "自动配对暂时不可用，极光会自动重试。";
    }

    private async Task LoadPairs()
    {
        var selected = (Pairings.SelectedItem as SunshinePairing)?.Id;
        var pairs = await admin.Pending();
        if (closed) return;
        Pairings.ItemsSource = pairs;
        Pairings.SelectedItem = Array.Find(pairs, pair => pair.Id == selected);
        PairHint.Text = pairs.Length == 0 ? "暂无待确认请求。先在主控端发起配对，再点刷新。"
            : $"有 {pairs.Length} 条待确认请求。请选择正确的设备，再输入主控端显示的 PIN。";
    }

    private void UpdateButtons()
    {
        AccountAction.IsEnabled = Save.IsEnabled = Restart.IsEnabled = ReloadPairs.IsEnabled = !busy;
        DetailedSettings.IsEnabled = !busy;
        ImportCert.IsEnabled = ForgetLogin.IsEnabled = AutoPairToggle.IsEnabled = !busy;
        ApprovePair.IsEnabled = !busy && Pairings.SelectedItem is SunshinePairing;
    }

    private async Task Run(Func<Task> operation)
    {
        if (busy || closed) return;
        busy = true;
        Feedback.Text = "正在联系本机 Sunshine…";
        UpdateButtons();
        try
        {
            await operation();
            if (!closed && Feedback.Text == "正在联系本机 Sunshine…") Feedback.Text = "";
        }
        catch (Exception error)
        {
            if (!closed)
            {
                Feedback.Text = error.Message;
                if (error is SunshineCertificateAccessException || error is SunshineTlsException && admin?.SupportsCertificateImport == true)
                {
                    ImportCert.IsVisible = true;
                    Subtitle.Text = "需要管理员确认。极光仅读取公开证书指纹，不读取私钥，也不修改 Sunshine 权限。";
                    return;
                }
                if (!AccountPanel.IsVisible && !SettingsPanel.IsVisible)
                    Subtitle.Text = "暂时无法在极光内管理。可打开 Sunshine 原版设置检查。";
            }
        }
        finally
        {
            busy = false;
            if (!closed) UpdateButtons();
        }
    }
}
