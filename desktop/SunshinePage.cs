using System;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Threading;

namespace Aurora;

public sealed partial class SunshinePage : UserControl, IDisposable
{
    private readonly SunshineHostSession session;
    private readonly bool ownsSession;
    private SunshineOptionsPage optionsPage;
    private string shownSession;
    private string shownFeedback;
    private bool shownSavedLogin;
    private SunshineOverview shownOverview;
    private SunshinePairing[] shownRequests;
    private bool closed;
    private readonly DispatcherTimer saveTimer = new();
    private OverviewSettings savedOverview;
    private Task<bool> settingsSave;
    private SunshinePrivacySettings displaySettings;
    private bool loadingSettings;
    private bool loadingDetail;
    private bool discardConfirmed;
    private sealed record OverviewSettings(string Name, bool Audio, SunshineDisplaySelection Display);

    public event Action<bool> DetailsChanged;
    internal bool IsBusy => loadingDetail;
    internal bool HasPendingSettings => settingsSave != null || HasChanges;
    private bool HasChanges => optionsPage?.HasChanges ?? (savedOverview != null && savedOverview != ReadOverview());

    internal string DetailTitle { get; private set; }

    public SunshinePage() : this(new SunshineHostSession(), ownsSession: true) { }

    internal SunshinePage(SunshineHostSession session) : this(session, ownsSession: false) { }

    private SunshinePage(SunshineHostSession session, bool ownsSession)
    {
        this.session = session;
        this.ownsSession = ownsSession;
        InitializeComponent();
        saveTimer.Tick += async (_, _) =>
        {
            if (!session.Busy && settingsSave == null) await FlushSettingsAsync(finishEditing: false);
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
                    SunshineHostSession.OpenMacPrivacySettings();
                }
                catch (Exception error) { Feedback.Text = error.Message; }
            };
        }
        AuroraVisual.SetTextAction(DetailedSettings, "全部配置项", AuroraVisual.ChevronRight);
        Loaded += async (_, _) => await ReloadIfUnavailable();
        ImportCert.Click += async (_, _) => await session.ImportCertificate();
        ForgetLogin.Click += async (_, _) => await session.ForgetCredentials();
        OriginalPage.Click += (_, _) => session.OpenOriginalSettings();
        AccountAction.Click += async (_, _) =>
        {
            var password = Password.Text ?? "";
            var confirmation = Confirmation.Text ?? "";
            Password.Text = Confirmation.Text = "";
            await session.SubmitCredentials(Username.Text?.Trim() ?? "", password, confirmation, RememberLogin.IsChecked == true);
        };
        DetailedSettings.Click += async (_, _) =>
        {
            if (!await FinishSettingsAsync()) return;
            await RunDetail(async () =>
            {
                optionsPage = new SunshineOptionsPage(session.Admin);
                optionsPage.Edited += QueueSave;
                Options.Content = optionsPage;
                Options.IsVisible = true;
                ShowDetail("全部配置项");
                await optionsPage.LoadOptions();
            });
        };
        PairingReauthorize.Click += async (_, _) => await session.ReauthorizePairing();
        ApproveMoonlight.Click += async (_, _) =>
        {
            var pin = MoonlightPin.Text;
            MoonlightPin.Clear();
            await session.ApprovePairing(MoonlightRequest.SelectedItem as SunshinePairing, pin);
        };
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
            RestartConfirmation.IsVisible = false;
            await session.Restart();
        };
        session.Changed += UpdateFromSession;
        UpdateFromSession();
    }

    private Task ReloadIfUnavailable() => Upstream.SupportsHost && DetailTitle == null && !HasPendingSettings &&
        (session.Bundled != BundledSunshine.IsRunning || !AccountPanel.IsVisible && !SettingsPanel.IsVisible)
        ? session.ReloadIfUnavailable() : Task.CompletedTask;

    internal void DismissRestartConfirmation() => RestartConfirmation.IsVisible = false;

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
        if (refreshPreferences) await session.RefreshSettings();
        else await ReloadIfUnavailable();
    }

    internal async Task<bool> FinishSettingsAsync(bool allowDiscard = false)
    {
        if (session.Busy) return false;
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
        session.Changed -= UpdateFromSession;
        if (ownsSession) session.Dispose();
    }

    private void UpdateFromSession()
    {
        if (closed) return;
        if (shownSession != session.StateId)
        {
            shownSession = session.StateId;
            ClearUnsubmittedSecrets();
            Username.Text = session.SavedUsername ?? "";
            RememberLogin.IsChecked = false;
            DismissRestartConfirmation();
            saveTimer.Stop();
            savedOverview = null;
        }
        if (shownSavedLogin && !session.SavedLogin) RememberLogin.IsChecked = false;
        shownSavedLogin = session.SavedLogin;
        OriginalPage.IsVisible = !session.Bundled;
        ImportCert.IsVisible = session.NeedsCertificate;
        ForgetLogin.IsVisible = session.SavedLogin;
        AccountPanel.IsVisible = session.NeedsAccount;
        SettingsPanel.IsVisible = session.Overview != null;
        ConnectionNote.Text = session.ConnectionNote;
        ConnectionNote.IsVisible = !string.IsNullOrEmpty(session.ConnectionNote);
        PairingFeedback.Text = session.PairingFeedback;
        PairingReauthorize.IsVisible = session.ReauthorizeAvailable;
        if (shownFeedback != session.Feedback)
        {
            shownFeedback = session.Feedback;
            Feedback.Text = shownFeedback;
        }
        if (session.NeedsAccount)
        {
            var setup = session.Access == SunshineAccess.Setup;
            Subtitle.Text = setup ? "首次启用：在这里创建 Sunshine 本机管理账号。" : "使用你已有的 Sunshine 管理账号登录。";
            AccountTitle.Text = setup ? "创建管理账号" : "登录 Sunshine";
            AccountAction.Content = setup ? "创建并登录" : "登录";
            Confirmation.IsVisible = setup;
        }
        if (!ReferenceEquals(shownOverview, session.Overview))
        {
            shownOverview = session.Overview;
            loadingSettings = true;
            try
            {
                if (shownOverview == null) LoadDisplaySettings(null);
                else
                {
                    HostName.Text = shownOverview.Preferences.Name;
                    StreamAudio.IsChecked = shownOverview.Preferences.StreamAudio;
                    LoadDisplaySettings(shownOverview.Displays);
                    savedOverview = ReadOverview();
                }
            }
            finally { loadingSettings = false; }
        }
        if (!ReferenceEquals(shownRequests, session.PairingRequests))
        {
            shownRequests = session.PairingRequests;
            var selected = (MoonlightRequest.SelectedItem as SunshinePairing)?.Id;
            MoonlightRequest.ItemsSource = shownRequests;
            MoonlightRequest.SelectedItem = shownRequests.FirstOrDefault(item => item.Id == selected) ?? shownRequests.FirstOrDefault();
            MoonlightPairing.IsVisible = shownRequests.Length != 0;
            if (MoonlightRequest.SelectedItem is not SunshinePairing current || (selected != null && current.Id != selected))
                MoonlightPin.Clear();
        }
        UpdateButtons();
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
            await session.Admin.SavePreferences(submitted.Name, submitted.Audio, submitted.Display);
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

    private void UpdateButtons()
    {
        var enabled = !session.Busy && !loadingDetail;
        AccountAction.IsEnabled = Restart.IsEnabled = enabled;
        DetailedSettings.IsEnabled = ConfirmRestart.IsEnabled = ApproveMoonlight.IsEnabled = enabled;
        ImportCert.IsEnabled = ForgetLogin.IsEnabled = enabled;
    }

    private async Task RunDetail(Func<Task> operation)
    {
        if (closed || session.Busy || loadingDetail) return;
        loadingDetail = true;
        UpdateButtons();
        try { await operation(); }
        catch (Exception error) { if (!closed) Feedback.Text = error.Message; }
        finally
        {
            loadingDetail = false;
            if (!closed) UpdateButtons();
        }
    }
}
