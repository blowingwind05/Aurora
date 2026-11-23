using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Threading;

namespace Aurora;

public sealed partial class SunshinePage : UserControl, IDisposable
{
    private SunshineAdmin admin;
    private SunshineCredentialStore credentials;
    private SunshineOptionsPage optionsPage;
    private AutomaticPairing.Host automaticPairing;
    private NetworkSnapshot network;
    private SunshineAccess access;
    private bool bundled;
    private bool busy;
    private bool closed;
    private readonly DispatcherTimer saveTimer = new();
    private OverviewSettings savedOverview;
    private Task<bool> settingsSave;
    private SunshinePrivacySettings displaySettings;
    private bool loadingSettings;
    private bool discardConfirmed;
    private sealed record OverviewSettings(string Name, bool Audio, SunshineDisplaySelection Display);

    public event Action<bool> DetailsChanged;
    internal bool IsBusy => busy;
    internal bool HasPendingSettings => settingsSave != null || HasChanges;
    private bool HasChanges => optionsPage?.HasChanges ?? (savedOverview != null && savedOverview != ReadOverview());

    internal string DetailTitle { get; private set; }

    public SunshinePage()
    {
        InitializeComponent();
        saveTimer.Tick += async (_, _) =>
        {
            if (!busy && settingsSave == null) await FlushSettingsAsync(finishEditing: false);
        };
        HostName.PropertyChanged += (_, args) =>
        {
            if (args.Property == TextBox.TextProperty) QueueSave(text: true);
        };
        StreamAudio.IsCheckedChanged += (_, _) => QueueSave(text: false);
        PrivacyDisplay.SelectionChanged += (_, _) =>
        {
            UpdateDisplayControls();
            QueueSave(text: false);
        };
        PrivacyScreensaver.IsCheckedChanged += (_, _) =>
        {
            UpdateDisplayControls();
            QueueSave(text: false);
        };
        LocalDisplays.SelectionChanged += (_, _) => QueueSave(text: false);
        if (OperatingSystem.IsMacOS())
        {
            RememberLogin.IsVisible = DetailedSettings.IsVisible = false;
            MacPermissions.IsVisible = MacPermissionNote.IsVisible = true;
            MacPermissions.Click += (_, _) =>
            {
                try
                {
                    var open = new ProcessStartInfo("/usr/bin/open") { UseShellExecute = false };
                    open.ArgumentList.Add("x-apple.systempreferences:com.apple.preference.security?Privacy");
                    using var process = Process.Start(open);
                }
                catch (Exception error) { Feedback.Text = error.Message; }
            };
        }
        AuroraVisual.SetTextAction(DetailedSettings, "全部配置项", AuroraVisual.ChevronRight);
        Loaded += async (_, _) => await ReloadIfUnavailable();
        ImportCert.Click += async (_, _) => await Run(async () =>
        {
            if (!OperatingSystem.IsWindows() || bundled) return;
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
            if (SettingsPanel.IsVisible) ConnectionNote.Text = "已连接本机 Sunshine · 登录只在本次运行期间有效";
            Feedback.Text = "已从 Windows 凭据管理器删除。退出 Aurora 后需要重新登录。";
            return Task.CompletedTask;
        });
        OriginalPage.Click += (_, _) =>
        {
            try
            {
                var url = admin?.SettingsUrl ?? (bundled ? BundledSunshine.AdminEndpoint.AbsoluteUri : Upstream.SettingsUrl);
                using var process = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
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
        DetailedSettings.Click += async (_, _) =>
        {
            if (!await FinishSettingsAsync()) return;
            await Run(async () =>
            {
                optionsPage = new SunshineOptionsPage(admin);
                optionsPage.Edited += QueueSave;
                Options.Content = optionsPage;
                Options.IsVisible = true;
                ShowDetail("全部配置项");
                await optionsPage.LoadOptions();
            });
        };
        PairingReauthorize.Click += async (_, _) => await Run(() => UpdateAutomaticPairing(reauthorize: true), showProgress: false);
        ApproveMoonlight.Click += async (_, _) => await Run(async () =>
        {
            var pin = MoonlightPin.Text;
            MoonlightPin.Clear();
            await admin.Pair(MoonlightRequest.SelectedItem as SunshinePairing, pin, "Moonlight");
            await RefreshMoonlightRequests();
            Feedback.Text = "Moonlight 已配对。";
        });
        Restart.Click += async (_, _) =>
        {
            if (!await FinishSettingsAsync()) return;
            Feedback.Text = "";
            RestartConfirmation.IsVisible = true;
            CancelRestart.Focus();
        };
        CancelRestart.Click += (_, _) =>
        {
            RestartConfirmation.IsVisible = false;
            Restart.Focus();
        };
        ConfirmRestart.Click += async (_, _) =>
        {
            if (!await FinishSettingsAsync()) return;
            await Run(RestartHost);
        };
        async Task RestartHost()
        {
            RestartConfirmation.IsVisible = false;
            if (!bundled)
            {
                await admin.Restart();
                Feedback.Text = "已发送重启请求。请等待服务恢复后再连接。";
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
            Feedback.Text = "内置 Sunshine 已重启。";
        }

        UpdateButtons();
    }

    public Task ReloadIfUnavailable() => DetailTitle == null && !HasPendingSettings &&
        (bundled != BundledSunshine.IsRunning || !AccountPanel.IsVisible && !SettingsPanel.IsVisible)
        ? Run(() => Open()) : Task.CompletedTask;

    internal void SuspendManagedPairing()
    {
        StopAutomaticPairing();
        RestartConfirmation.IsVisible = false;
    }

    private void ShowDetail(string title)
    {
        RestartConfirmation.IsVisible = false;
        Overview.IsVisible = false;
        DetailTitle = title;
        DetailsChanged?.Invoke(true);
    }

    public async Task ReturnToOverview()
    {
        if (closed || DetailTitle == null || !await FinishSettingsAsync(allowDiscard: true)) return;
        var refreshPreferences = optionsPage != null;
        Options.IsVisible = false;
        Options.Content = null;
        optionsPage = null;
        Overview.IsVisible = true;
        Overview.Offset = default;
        DetailTitle = null;
        DetailsChanged?.Invoke(false);
        DetailedSettings.Focus();
        if (bundled != BundledSunshine.IsRunning) await ReloadIfUnavailable();
        else if (refreshPreferences) await Run(ShowSettings);
    }

    internal async Task<bool> FinishSettingsAsync(bool allowDiscard = false)
    {
        if (busy) return false;
        if (await FlushSettingsAsync(finishEditing: true)) return true;
        if (!allowDiscard) return false;
        if (!discardConfirmed)
        {
            discardConfirmed = true;
            ReportSettings("有设置未能保存。再次返回或退出将放弃未保存的改动。");
            return false;
        }
        optionsPage?.DiscardChanges();
        savedOverview = ReadOverview();
        return true;
    }

    public void ClearUnsubmittedSecrets()
    {
        Password.Text = Confirmation.Text = "";
        MoonlightPin.Clear();
    }

    public void Dispose()
    {
        closed = true;
        saveTimer.Stop();
        ClearUnsubmittedSecrets();
        StopAutomaticPairing();
        admin?.Dispose();
    }

    internal async Task SetNetwork(NetworkSnapshot value)
    {
        network = value;
        if (value?.IsAuthenticated != true || automaticPairing?.Matches(value) == false) StopAutomaticPairing();
        if (closed || busy || HasPendingSettings) return;
        if (bundled != BundledSunshine.IsRunning) StopAutomaticPairing();
        if (bundled != BundledSunshine.IsRunning || admin == null || !AccountPanel.IsVisible && !SettingsPanel.IsVisible)
            await ReloadIfUnavailable();
        else if (access == SunshineAccess.SignedIn)
        {
            await UpdateAutomaticPairing();
            await RefreshMoonlightRequests();
        }
    }

    private void ResetAdmin()
    {
        StopAutomaticPairing();
        admin?.Dispose();
        admin = null;
        credentials = null;
        ClearUnsubmittedSecrets();
        Username.Text = "";
        RestartConfirmation.IsVisible = false;
        ImportCert.IsVisible = ForgetLogin.IsVisible = AccountPanel.IsVisible = SettingsPanel.IsVisible = false;
        PrivacySettings.IsVisible = false;
        MoonlightPairing.IsVisible = false;
        MoonlightRequest.ItemsSource = null;
        saveTimer.Stop();
        savedOverview = null;
        displaySettings = null;
        PrivacyDisplay.ItemsSource = null;
        RememberLogin.IsChecked = false;
    }

    private async Task Open(byte[] importedPin = null)
    {
        if (!Upstream.SupportsHost || closed) return;
        ResetAdmin();
        bundled = BundledSunshine.IsRunning;
        OriginalPage.IsVisible = !bundled;
        ConnectionNote.IsVisible = true;
        ConnectionNote.Text = "正在检查本机 Sunshine…";
        var local = await Task.Run(() => bundled ? SunshineAdmin.OpenBundled() : SunshineAdmin.OpenLocal(importedPin));
        if (closed) { local.Dispose(); return; }
        admin = local;
        access = await admin.CheckAccess();
        if (closed) return;
        if (bundled)
        {
            if (access != SunshineAccess.SignedIn)
                throw new InvalidOperationException("内置被控组件未完成 Aurora 托管登录，请重新启用本机被控。");
            await ShowSettings();
            return;
        }
        if (importedPin != null) admin.RememberVerifiedCertificate();
        ImportCert.IsVisible = false;
        if (OperatingSystem.IsWindows())
            credentials = new SunshineCredentialStore(admin.CredentialScope);
        if (access == SunshineAccess.Login && credentials != null)
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
        ConnectionNote.IsVisible = false;
        AccountPanel.IsVisible = true;
    }

    private async Task ShowSettings()
    {
        var preferences = await admin.ReadPreferences();
        var displays = await admin.ReadPrivacySettings();
        if (closed) return;
        loadingSettings = true;
        try
        {
            HostName.Text = preferences.Name;
            StreamAudio.IsChecked = preferences.StreamAudio;
            LoadDisplaySettings(displays);
            savedOverview = ReadOverview();
        }
        finally { loadingSettings = false; }
        AccountPanel.IsVisible = false;
        SettingsPanel.IsVisible = true;
        ConnectionNote.IsVisible = !bundled;
        ConnectionNote.Text = ForgetLogin.IsVisible
            ? "已登录 · 凭据保存在 Windows 凭据管理器"
            : "已登录 · 本次运行期间有效";
        await UpdateAutomaticPairing();
        await RefreshMoonlightRequests();
    }

    private async Task RefreshMoonlightRequests()
    {
        var settingsAdmin = admin;
        if (closed || !bundled || !Overview.IsEffectivelyVisible || !SettingsPanel.IsVisible || settingsAdmin == null) return;
        SunshinePairing[] requests;
        try { requests = await settingsAdmin.Pending(); }
        catch (Exception error) when (error is InvalidOperationException or System.Net.Http.HttpRequestException or System.Text.Json.JsonException)
        {
            Debug.WriteLine(error.Message);
            return;
        }
        if (closed || settingsAdmin != admin) return;
        var external = requests.Where(request => !AutomaticPairing.IsBackgroundPairing(request.Name)).ToArray();
        var selected = (MoonlightRequest.SelectedItem as SunshinePairing)?.Id;
        MoonlightRequest.ItemsSource = external;
        MoonlightRequest.SelectedItem = external.FirstOrDefault(item => item.Id == selected) ?? external.FirstOrDefault();
        MoonlightPairing.IsVisible = external.Length != 0;
        if (MoonlightRequest.SelectedItem is not SunshinePairing current || (selected != null && current.Id != selected))
            MoonlightPin.Clear();
    }

    private void LoadDisplaySettings(SunshinePrivacySettings displays)
    {
        displaySettings = displays;
        var hasScreensaver = displays?.Saved.Screensaver == true || displays?.Active.Screensaver == true;
        var hasVirtualSelection = displays?.SupportsVirtualDisplay == true &&
            (displays.Saved.Id.Length != 0 || displays.Active.Id.Length != 0 || !displays.SupportsScreensaver);
        PrivacySettings.IsVisible = hasScreensaver || hasVirtualSelection;
        PrivacyDisplay.ItemsSource = null;
        if (!PrivacySettings.IsVisible) return;

        PrivacyScreensaverRow.IsVisible = hasScreensaver;
        PrivacyScreensaver.IsEnabled = displays.Saved.Screensaver;
        PrivacyDisplayRow.IsVisible = hasVirtualSelection;
        LocalDisplaysRow.IsVisible = hasVirtualSelection && displays.SupportsExtended;
        if (hasVirtualSelection)
        {
            var choices = new[] { new SunshinePrivacyDisplay("", "按原有显示设置") }
                .Concat(displays.Displays).ToList();
            var selected = choices.FirstOrDefault(display => display.Id == displays.Saved.Id);
            if (selected == null)
            {
                selected = new(displays.Saved.Id, "已选虚拟显示器不可用");
                choices.Add(selected);
            }
            PrivacyDisplay.ItemsSource = choices;
            PrivacyDisplay.SelectedItem = selected;
        }
        LocalDisplays.SelectedIndex = displays.Saved.Exclusive ? 1 : 0;
        PrivacyScreensaver.IsChecked = displays.Saved.Screensaver;
        UpdateDisplayControls();
        UpdatePrivacyNote();
    }

    private void UpdateDisplayControls()
    {
        LocalDisplays.IsEnabled = PrivacyScreensaver.IsChecked != true &&
            PrivacyDisplay.SelectedItem is SunshinePrivacyDisplay { Id.Length: > 0 };
        if (!LocalDisplays.IsEnabled) LocalDisplays.SelectedIndex = 0;
    }

    private void UpdatePrivacyNote()
    {
        if (displaySettings == null) return;
        PrivacyNote.Text = displaySettings.Saved.Screensaver
            ? "可关闭旧版自动屏保，改为在主控端的控制中心操作。"
            : PrivacyDisplayRow.IsVisible ? "断开后恢复原来的显示布局。" : "";
        if (displaySettings.Active != displaySettings.Saved) PrivacyNote.Text += " 重启被控组件后生效。";
    }

    private OverviewSettings ReadOverview()
    {
        var id = (PrivacyDisplay.SelectedItem as SunshinePrivacyDisplay)?.Id ?? "";
        var screensaver = PrivacyScreensaverRow.IsVisible && PrivacyScreensaver.IsChecked == true;
        var selection = !PrivacySettings.IsVisible ? null : new SunshineDisplaySelection(id, id.Length != 0 && !screensaver &&
            (!displaySettings.SupportsExtended || LocalDisplays.SelectedIndex == 1), screensaver);
        return new(HostName.Text?.Trim() ?? "", StreamAudio.IsChecked == true, selection);
    }

    private void QueueSave(bool text)
    {
        if (closed || loadingSettings || !HasChanges) return;
        discardConfirmed = false;
        saveTimer.Stop();
        saveTimer.Interval = TimeSpan.FromMilliseconds(text ? 650 : 1);
        saveTimer.Start();
    }

    private async Task<bool> FlushSettingsAsync(bool finishEditing)
    {
        saveTimer.Stop();
        while (HasPendingSettings)
        {
            if (!await (settingsSave ??= SaveCurrentSettings())) return false;
            if (!finishEditing) break;
        }
        return true;
    }

    private async Task<bool> SaveCurrentSettings()
    {
        await Task.Yield();
        try
        {
            if (optionsPage != null) return await optionsPage.SaveOptions();
            var submitted = ReadOverview();
            if (savedOverview == null || submitted == savedOverview) return true;
            await admin.SavePreferences(submitted.Name, submitted.Audio, submitted.Display);
            savedOverview = submitted;
            if (displaySettings != null && submitted.Display != null)
            {
                displaySettings = displaySettings with { Saved = submitted.Display };
                PrivacyScreensaver.IsEnabled = submitted.Display.Screensaver;
                UpdatePrivacyNote();
            }
            ReportSettings("已保存。重启 Sunshine 后生效。");
            return true;
        }
        catch (Exception error)
        {
            ReportSettings("保存失败。" + error.Message);
            return false;
        }
        finally { settingsSave = null; }
    }

    private void ReportSettings(string message)
    {
        if (optionsPage != null) optionsPage.Report(message);
        else Feedback.Text = message;
    }

    private void StopAutomaticPairing()
    {
        automaticPairing?.Dispose();
        automaticPairing = null;
        PairingFeedback.Text = null;
        PairingReauthorize.IsVisible = false;
    }

    private async Task UpdateAutomaticPairing(bool reauthorize = false)
    {
        if (closed || admin == null) return;
        var current = network;
        if (current?.IsAuthenticated == true && !AutomaticPairing.SupportsHost(current, admin))
        {
            StopAutomaticPairing();
            if (OperatingSystem.IsMacOS() && current.Mode == TailscaleMode.System)
                PairingFeedback.Text = "Mac 被控端使用内置 Tailscale 时可接收自动配对。";
            return;
        }
        bool enabled;
        try { enabled = AutomaticPairing.EnsureEnrollment(current, admin, reauthorize); }
        catch (Exception error)
        {
            StopAutomaticPairing();
            PairingFeedback.Text = "自动配对设置失败。" + error.Message;
            return;
        }
        if (!enabled)
        {
            StopAutomaticPairing();
            if (current?.IsAuthenticated == true && AutomaticPairing.IsConfigured)
            {
                PairingFeedback.Text = admin.IsManaged
                    ? "账号或被控证书已变更，请确认当前账号的设备访问权限。"
                    : "账号或被控证书已变更，自动配对暂不可用。";
                PairingReauthorize.IsVisible = admin.IsManaged;
            }
            return;
        }
        if (automaticPairing?.Matches(current) == true) return;
        StopAutomaticPairing();
        var receiver = new AutomaticPairing.Host(admin, current);
        automaticPairing = receiver;
        try
        {
            await receiver.Start();
            if (automaticPairing != receiver) return;
            _ = ObserveAutomaticPairing(receiver);
        }
        catch (Exception error)
        {
            if (automaticPairing != receiver) return;
            StopAutomaticPairing();
            PairingFeedback.Text = error.Message;
        }
    }

    private async Task ObserveAutomaticPairing(AutomaticPairing.Host receiver)
    {
        try { await receiver.Completion; }
        catch (Exception) { }
        if (closed || automaticPairing != receiver) return;
        StopAutomaticPairing();
    }

    private void UpdateButtons()
    {
        AccountAction.IsEnabled = Restart.IsEnabled = !busy;
        DetailedSettings.IsEnabled = ConfirmRestart.IsEnabled = ApproveMoonlight.IsEnabled = !busy;
        ImportCert.IsEnabled = ForgetLogin.IsEnabled = !busy;
    }

    private async Task Run(Func<Task> operation, bool showProgress = true)
    {
        if (busy || closed) return;
        busy = true;
        const string pendingMessage = "正在联系本机 Sunshine…";
        if (showProgress) Feedback.Text = pendingMessage;
        UpdateButtons();
        try
        {
            await operation();
            if (!closed && showProgress && Feedback.Text == pendingMessage) Feedback.Text = "";
        }
        catch (Exception error)
        {
            if (!closed)
            {
                Feedback.Text = error.Message;
                if (!bundled && (error is SunshineCertificateAccessException ||
                    error is SunshineTlsException && admin?.SupportsCertificateImport == true))
                {
                    ImportCert.IsVisible = true;
                    ConnectionNote.IsVisible = true;
                    ConnectionNote.Text = "需要管理员确认。Aurora 仅读取公开证书指纹，不读取私钥，也不修改 Sunshine 权限。";
                    return;
                }
                if (!AccountPanel.IsVisible && !SettingsPanel.IsVisible)
                    ConnectionNote.Text = bundled ? "请重新启用本机被控。" : "暂时无法在 Aurora 内管理。可打开 Sunshine 原版设置检查。";
            }
        }
        finally
        {
            busy = false;
            if (!closed) UpdateButtons();
        }
    }
}
