using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace Aurora;

internal sealed partial class TailscalePage : UserControl, IDisposable
{
    private sealed record ModeChoice(TailscaleMode Mode, string Label)
    {
        public override string ToString() => Label;
    }

    private sealed class SwitchComboBox : ComboBox
    {
        protected override Type StyleKeyOverride => typeof(ComboBox);

        protected override void OnKeyDown(KeyEventArgs e)
        {
            // A closed settings ComboBox normally commits on arrow keys.
            if (!e.Handled && !IsDropDownOpen && e.Key is Key.Up or Key.Down && !e.KeyModifiers.HasFlag(KeyModifiers.Alt))
            {
                IsDropDownOpen = true;
                e.Handled = true;
            }
            base.OnKeyDown(e);
        }

        protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
        {
            if (IsDropDownOpen) base.OnPointerWheelChanged(e);
        }
    }

    private static readonly ModeChoice Embedded = new(TailscaleMode.Embedded, "内置 Tailscale");
    private static readonly ModeChoice SystemClient = new(TailscaleMode.System, "系统 Tailscale");
    private static readonly ModeChoice[] BothModes = { Embedded, SystemClient };
    private static readonly ModeChoice[] EmbeddedOnly = { Embedded };
    private const string LogoutIcon = "M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4 m7 14 5-5-5-5 M21 12H9";
    private static readonly IBrush StatusBlue = Brush.Parse("#244FC5");
    private static readonly IBrush StatusAmber = Brush.Parse("#906016");
    private static readonly IBrush StatusGray = Brush.Parse("#59667A");
    private static readonly IBrush AvatarBackground = Brush.Parse("#DEECFA");
    private readonly Func<Task<bool>> confirmLogout;
    private readonly Func<string, string, Task<bool>> confirmSwitch;
    private readonly Func<bool> hasActiveSession;
    private readonly Action<string> showSwitchSuccess;
    private readonly SemaphoreSlim operation = new(1, 1);
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(3) };
    private readonly Border card = new() { Classes = { "panel" }, Padding = new Thickness(14, 13, 14, 10) };
    private readonly Grid identity = new() { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
    private readonly Grid identityDetails = new() { ColumnDefinitions = new ColumnDefinitions("40,*,Auto") };
    private readonly ContentControl staticIdentity = new() { HorizontalAlignment = HorizontalAlignment.Left };
    private readonly Button profileTrigger = new()
    {
        Classes = { "nav" }, IsVisible = false, MinHeight = 40, Padding = new Thickness(0, 0, 8, 0),
        HorizontalAlignment = HorizontalAlignment.Left, HorizontalContentAlignment = HorizontalAlignment.Stretch
    };
    private readonly Popup profileMenu = new() { Placement = PlacementMode.BottomEdgeAlignedLeft, VerticalOffset = 4, IsLightDismissEnabled = true, ShouldUseOverlayLayer = true };
    private readonly StackPanel profileRows = new() { Spacing = 1 };
    private readonly Viewbox profileChevron;
    private readonly Control tailscaleMark = AuroraVisual.TailnetMark(size: 28);
    private readonly Border avatar = new() { Width = 40, Height = 40, CornerRadius = new CornerRadius(20), Background = AvatarBackground };
    private readonly TextBlock initial = new()
    {
        FontSize = 18, FontWeight = FontWeight.SemiBold, Foreground = StatusBlue,
        HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
    };
    private readonly TextBlock displayName = new()
    {
        Classes = { "sectionTitle" }, FontSize = 15, TextWrapping = TextWrapping.Wrap
    };
    private readonly TextBlock loginName = new() { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap };
    private readonly Ellipse statusMark = new() { Width = 7, Height = 7, StrokeThickness = 1.5 };
    private readonly TextBlock status = new() { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly StackPanel indicator = new() { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Spacing = 6 };
    private readonly TextBlock details = new() { Classes = { "muted" }, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock networkName = new() { FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock localAddress = new() { FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly Grid connection = new()
    {
        ColumnDefinitions = new ColumnDefinitions("*,*"), RowDefinitions = new RowDefinitions("Auto,Auto")
    };
    private readonly Button action = new() { Classes = { "action", "accent" } };
    private readonly Button logout = new()
    {
        Classes = { "quietAction" }, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(8, 0, 0, 0)
    };
    private readonly StackPanel actions = new() { HorizontalAlignment = HorizontalAlignment.Right, Orientation = Orientation.Horizontal, Spacing = 7 };
    private readonly ComboBox modeSelector = Selector("Tailscale 运行方式");
    private readonly TextBlock fixedMode = new() { VerticalAlignment = VerticalAlignment.Center, FontWeight = FontWeight.SemiBold };
    private readonly TextBlock modeNote = new() { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar switchProgress = new() { MinHeight = 3, Height = 3, Value = 55, IsVisible = false };
    private readonly TextBlock accountSwitchNote = new() { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap, IsVisible = false };
    private readonly ProgressBar accountSwitchProgress = new() { MinHeight = 3, Height = 3, Value = 55, IsVisible = false };
    private readonly Button retrySwitch = new() { Classes = { "action" }, Content = "重试", HorizontalAlignment = HorizontalAlignment.Left, IsVisible = false };
    private readonly ContentControl runtimeFeedback = new() { IsVisible = false };
    private readonly ContentControl accountFeedback = new() { IsVisible = false };
    private readonly StackPanel operationFeedback;
    private readonly RefreshNotice feedback = new()
    {
        Child = new TextBlock { Classes = { "muted" }, FontSize = 13, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0) }
    };
    private TailscaleStatus current = new(TailscaleState.Unknown);
    private TailscaleMode mode = TailscaleRuntime.Mode;
    private TailscaleProfile[] profiles = Array.Empty<TailscaleProfile>();
    private TailscaleProfile currentProfile;
    private int readVersion = TailscaleRuntime.Generation;
    private int foregroundOperations;
    private bool settingSelection;
    private bool loginAfterSwitch;
    private bool switchingProfile;
    private Func<Task> retry;
    private string switchMessage;
    private string address;
    private bool compact;
    private bool hasStatus;
    private bool active;
    private bool closed;
    private bool confirmingLogout;
    private bool focusPrimary;
    private bool focusLoginAfterLogout;
    private Window avatarWindow;
    private (long Owner, Uri Uri)? avatarKey;
    private CancellationTokenSource avatarRequest;
    private Bitmap avatarBitmap;

    public event Action<TailscaleStatus> StatusChanged;

    public TailscalePage(Func<Task<bool>> confirmLogout, Func<string, string, Task<bool>> confirmSwitch = null,
        Func<bool> hasActiveSession = null, Action<string> showSwitchSuccess = null, bool externalPresentation = false)
    {
        this.externalPresentation = externalPresentation;
        this.confirmLogout = confirmLogout ?? throw new ArgumentNullException(nameof(confirmLogout));
        this.confirmSwitch = confirmSwitch;
        this.hasActiveSession = hasActiveSession;
        this.showSwitchSuccess = showSwitchSuccess;
        avatar.Child = initial;
        var icon = new Grid { Width = 40, Height = 40, Children = { tailscaleMark, avatar } };
        tailscaleMark.HorizontalAlignment = HorizontalAlignment.Center;
        indicator.Children.Add(statusMark);
        indicator.Children.Add(status);
        var nameAndStatus = new Grid
        {
            HorizontalAlignment = HorizontalAlignment.Left,
            ColumnDefinitions = new ColumnDefinitions("*,Auto"), RowDefinitions = new RowDefinitions("Auto,Auto"),
            Children = { displayName, indicator }
        };
        Grid.SetColumn(indicator, 1);
        indicator.Margin = new Thickness(8, 0, 0, 0);
        var account = new StackPanel
        {
            Margin = new Thickness(10, 0, 0, 0), Spacing = 2, VerticalAlignment = VerticalAlignment.Center,
            Children = { nameAndStatus, loginName }
        };
        Grid.SetColumn(account, 1);
        profileChevron = AuroraVisual.Icon(AuroraVisual.ChevronDown, profileTrigger, size: 16);
        profileChevron.Margin = new Thickness(6, 0, 0, 0);
        profileChevron.IsVisible = false;
        Grid.SetColumn(profileChevron, 2);
        identityDetails.Children.Add(icon);
        identityDetails.Children.Add(account);
        identityDetails.Children.Add(profileChevron);
        staticIdentity.Content = identityDetails;
        identity.Children.Add(staticIdentity);
        identity.Children.Add(profileTrigger);
        var logoutText = new TextBlock { Text = "退出登录", VerticalAlignment = VerticalAlignment.Center };
        logout.Content = new StackPanel
        {
            Orientation = Orientation.Horizontal, Spacing = 5,
            Children = { AuroraVisual.Icon(LogoutIcon, logout, size: 16), logoutText }
        };
        AutomationProperties.SetName(logout, "退出登录");
        ToolTip.SetTip(logout, "退出登录");
        Grid.SetColumn(logout, 1);
        identity.Children.Add(logout);
        var menuFrame = CreateProfileMenu();
        profileMenu.Child = menuFrame;
        identity.Children.Add(profileMenu);

        var networkField = ConnectionField("所属网络", networkName);
        var addressField = ConnectionField("本机 Tailscale 地址", localAddress);
        Grid.SetColumn(addressField, 1);
        connection.Children.Add(networkField);
        connection.Children.Add(addressField);
        actions.Children.Add(action);
        var progress = new StackPanel { Spacing = 5, Children = { accountSwitchNote, accountSwitchProgress } };
        accountSwitchNote.PropertyChanged += (_, args) =>
        {
            if (args.Property == IsVisibleProperty) progress.IsVisible = accountSwitchNote.IsVisible;
        };
        progress.IsVisible = false;
        AutomationProperties.SetLiveSetting(accountSwitchNote, AutomationLiveSetting.Polite);
        card.Child = new StackPanel { Spacing = 20, Children = { identity, progress, connection, details, actions, accountFeedback } };
        card.SizeChanged += (_, args) =>
        {
            menuFrame.Width = Math.Clamp(args.NewSize.Width - 28, 0, 340);
            var narrow = args.NewSize.Width < 380;
            if (compact == narrow) return;
            compact = narrow;
            Grid.SetColumn(indicator, compact ? 0 : 1);
            Grid.SetRow(indicator, compact ? 1 : 0);
            indicator.Margin = compact ? new Thickness(0, 3, 0, 0) : new Thickness(8, 0, 0, 0);
            logoutText.IsVisible = !compact;
            connection.ColumnDefinitions[1].Width = compact ? new GridLength(0) : GridLength.Star;
            connection.RowSpacing = compact ? 20 : 0;
            Grid.SetColumn(addressField, compact ? 0 : 1);
            Grid.SetRow(addressField, compact ? 1 : 0);
        };

        var runtimeCard = CreateRuntimePanel();
        AutomationProperties.SetName(card, "当前账号");
        operationFeedback = new StackPanel { Spacing = 6, IsVisible = false, Children = { feedback, retrySwitch } };
        accountFeedback.Content = operationFeedback;
        feedback.PropertyChanged += (_, args) =>
        {
            if (args.Property == IsVisibleProperty)
            {
                operationFeedback.IsVisible = feedback.IsVisible;
                accountFeedback.IsVisible = accountFeedback.Content != null && feedback.IsVisible;
                runtimeFeedback.IsVisible = runtimeFeedback.Content != null && feedback.IsVisible;
            }
        };
        Content = new ScrollViewer
        {
            Margin = new Thickness(-5, 0, 0, 0),
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Content = new StackPanel
            {
                Margin = new Thickness(5, 0, 0, 0),
                Spacing = 12,
                Children = { runtimeCard, card, CreateHelp() }
            }
        };
        retrySwitch.Click += async (_, _) =>
        {
            if (CanChoose && retry != null) await retry();
        };
        logout.Click += async (_, _) => await RequestLogout();
        action.Click += async (_, _) => await RequestPrimaryAction();
        timer.Tick += async (_, _) => await Run(() => Read(), background: true);
        TailscaleRuntime.ContextChanging += RuntimeChanging;
        TailscaleRuntime.ContextChanged += RuntimeChanged;
        TailscaleRuntime.ModeChanged += RuntimeChanged;
        UpdateRuntimePanel();
        Render();
    }

    public Task RefreshStatus() => !CanChoose ? Task.CompletedTask : Run(async () =>
    {
        if (TailscaleRuntime.IsTransitioning) return;
        if (avatarBitmap == null && avatarRequest == null) avatarKey = null;
        await Read(refreshAddress: true, refreshProfiles: true);
        UpdateRuntimePanel();
        if (closed || !active) return;
        if (current.State == TailscaleState.Connected)
        {
            RouteFeedback(runtime: false);
            feedback.ShowRefreshSuccess("检测完成，Tailscale 已连接。");
        }
        else SetFeedback($"检测完成，Tailscale {status.Text}。");
    }, keepFeedback: true);

    public void FocusPrimaryAction()
    {
        if (closed) return;
        focusPrimary = true;
        Dispatcher.UIThread.Post(TryFocusPrimaryAction, DispatcherPriority.Loaded);
    }

    public void SetActive(bool active)
    {
        if (closed || this.active == active) return;
        this.active = active;
        UpdateActions();
        if (active)
        {
            timer.Start();
            _ = Run(() => Read(refreshAddress: true, refreshProfiles: true), keepFeedback: true);
            TryFocusPrimaryAction();
        }
        else
        {
            timer.Stop();
            profileMenu.IsOpen = false;
            focusPrimary = focusLoginAfterLogout = false;
            feedback.Clear();
            SetRetry(null);
            ClearAvatar();
        }
    }

    public void Dispose()
    {
        closed = true;
        timer.Stop();
        TailscaleRuntime.ContextChanging -= RuntimeChanging;
        TailscaleRuntime.ContextChanged -= RuntimeChanged;
        TailscaleRuntime.ModeChanged -= RuntimeChanged;
        ClearAvatar();
        DetachAvatarWindow();
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        if (closed) return;
        avatarWindow = TopLevel.GetTopLevel(this) as Window;
        if (avatarWindow != null) avatarWindow.PropertyChanged += AvatarWindowChanged;
        UpdateAvatar();
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        CloseProfileMenu();
        ClearAvatar();
        DetachAvatarWindow();
        base.OnDetachedFromVisualTree(e);
    }

    private void DetachAvatarWindow()
    {
        if (avatarWindow != null) avatarWindow.PropertyChanged -= AvatarWindowChanged;
        avatarWindow = null;
    }

    private void AvatarWindowChanged(object sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property != IsVisibleProperty && e.Property != Window.WindowStateProperty) return;
        UpdateAvatar();
        if (avatarWindow is { IsVisible: true, WindowState: not WindowState.Minimized })
            Dispatcher.UIThread.Post(UpdateAvatar, DispatcherPriority.Loaded);
    }

    private static StackPanel ConnectionField(string label, TextBlock value) => new()
    {
        Spacing = 5,
        Margin = new Thickness(0, 0, 10, 0),
        Children = { new TextBlock { Text = label, Classes = { "muted" }, FontSize = 12 }, value }
    };

    private static ComboBox Selector(string label)
    {
        var selector = new SwitchComboBox
        {
            Classes = { "settingField" }, FontSize = 13, HorizontalAlignment = HorizontalAlignment.Stretch,
            IsTextSearchEnabled = false
        };
        AutomationProperties.SetName(selector, label);
        return selector;
    }

    private Control CreateRuntimePanel()
    {
        modeSelector.SelectionChanged += async (_, _) =>
        {
            if (settingSelection) return;
            var choice = modeSelector.SelectedItem as ModeChoice;
            UpdateRuntimePanel();
            if (choice == null || choice.Mode == mode) return;
            modeSelector.IsDropDownOpen = false;
            await RequestModeSwitch(choice);
        };
        AutomationProperties.SetLiveSetting(modeNote, AutomationLiveSetting.Polite);
        return CreateSelectorCard("运行方式", new Grid { Children = { modeSelector, fixedMode } },
            modeNote, switchProgress, runtimeFeedback);
    }

    private Border CreateProfileMenu()
    {
        var menu = new Border
        {
            Width = 340, Padding = new Thickness(5), CornerRadius = new CornerRadius(8),
            Background = Brushes.White, BorderBrush = Brush.Parse("#D0D9E8"), BorderThickness = new Thickness(1),
            BoxShadow = BoxShadows.Parse("0 5 16 #2619283F"),
            Child = new StackPanel
            {
                Children =
                {
                    new ScrollViewer
                    {
                        Content = profileRows, MaxHeight = 252, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled
                    },
                    new TextBlock
                    {
                        Text = "切换也会更改系统 Tailscale 的当前账号。", TextWrapping = TextWrapping.Wrap,
                        Classes = { "muted" }, FontSize = 12, Margin = new Thickness(7, 8, 7, 5)
                    }
                }
            }
        };
        profileMenu.PlacementTarget = profileTrigger;
        AutomationProperties.SetName(profileTrigger, "切换系统 Tailscale 账号");
        profileTrigger.Click += (_, _) =>
        {
            if (profileMenu.IsOpen) CloseProfileMenu(restoreFocus: true);
            else if (CanChoose && profiles.Length > 0) profileMenu.IsOpen = true;
        };
        profileTrigger.AddHandler(KeyDownEvent, (_, e) =>
        {
            if (e.Key is not (Key.Up or Key.Down) || !CanChoose || profiles.Length == 0) return;
            profileMenu.IsOpen = true;
            e.Handled = true;
        }, RoutingStrategies.Tunnel);
        profileMenu.Opened += (_, _) =>
        {
            profileTrigger.Classes.Add("selected");
            profileChevron.RenderTransform = new RotateTransform(180);
            Dispatcher.UIThread.Post(() =>
            {
                if (profileMenu.IsOpen) FocusProfileRow(Math.Max(0, Array.IndexOf(profiles, currentProfile)));
            }, DispatcherPriority.Loaded);
        };
        profileMenu.Closed += (_, _) =>
        {
            profileTrigger.Classes.Remove("selected");
            profileChevron.RenderTransform = null;
        };
        menu.AddHandler(KeyDownEvent, (_, e) =>
        {
            if (e.Key is Key.Escape or Key.Tab)
            {
                CloseProfileMenu(restoreFocus: true);
                e.Handled = true;
                return;
            }
            if (e.Key is not (Key.Up or Key.Down or Key.Home or Key.End)) return;
            var index = profileRows.Children.IndexOf(e.Source as Control);
            var next = e.Key switch
            {
                Key.Home => 0,
                Key.End => profileRows.Children.Count - 1,
                Key.Up => index - 1,
                _ => index + 1
            };
            FocusProfileRow(next);
            e.Handled = true;
        }, RoutingStrategies.Tunnel);
        return menu;
    }

    private void FocusProfileRow(int index)
    {
        if (profileRows.Children.Count == 0) return;
        var row = profileRows.Children[Math.Clamp(index, 0, profileRows.Children.Count - 1)];
        row.Focus(NavigationMethod.Directional);
        row.BringIntoView();
    }

    private void CloseProfileMenu(bool restoreFocus = false)
    {
        profileMenu.IsOpen = false;
        if (restoreFocus && profileTrigger.IsVisible && profileTrigger.IsEnabled) profileTrigger.Focus();
    }

    private Button CreateProfileRow(TailscaleProfile profile)
    {
        var name = string.IsNullOrWhiteSpace(profile.Name) ? profile.LoginName : profile.Name;
        var detail = string.Join(" · ", new[] { profile.LoginName == name ? null : profile.LoginName, profile.Tailnet }
            .Where(label => !string.IsNullOrWhiteSpace(label)));
        var labels = new StackPanel
        {
            Spacing = 2,
            Children =
            {
                new TextBlock { Text = name, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap },
                new TextBlock { Text = detail, Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap, IsVisible = detail.Length > 0 }
            }
        };
        var row = new Button
        {
            Classes = { "nav" }, Padding = new Thickness(7, 5), MinHeight = 45,
            HorizontalContentAlignment = HorizontalAlignment.Stretch
        };
        var content = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 8, Children = { labels } };
        if (profile.IsCurrent)
        {
            row.Classes.Add("selected");
            var tick = AuroraVisual.Icon("M4.5 12.75 9 17.25 19.5 6.75", row, size: 16, strokeThickness: 3);
            Grid.SetColumn(tick, 1);
            content.Children.Add(tick);
        }
        row.Content = content;
        var accessibleName = detail.Length == 0 ? name : $"{name}，{detail}";
        AutomationProperties.SetName(row, profile.IsCurrent ? $"{accessibleName}，当前账号" : accessibleName);
        row.Click += async (_, _) =>
        {
            CloseProfileMenu(restoreFocus: true);
            if (!profile.IsCurrent) await RequestProfileSwitch(profile);
        };
        return row;
    }

    private void UpdateProfileIdentity()
    {
        var interactive = mode == TailscaleMode.System && profiles.Length > 0;
        if (profileTrigger.IsVisible != interactive)
        {
            CloseProfileMenu();
            if (interactive)
            {
                staticIdentity.Content = null;
                profileTrigger.Content = identityDetails;
            }
            else
            {
                profileTrigger.Content = null;
                staticIdentity.Content = identityDetails;
            }
            staticIdentity.IsVisible = !interactive;
            profileTrigger.IsVisible = interactive;
            profileChevron.IsVisible = interactive;
        }
        AutomationProperties.SetHelpText(profileTrigger, loginName.Text == null ? displayName.Text : $"{displayName.Text}，{loginName.Text}");
    }

    private static Border CreateSelectorCard(string title, Control value, params Control[] details)
    {
        var headline = new TextBlock { Text = title, Classes = { "sectionTitle" }, VerticalAlignment = VerticalAlignment.Center };
        var row = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,235"), RowDefinitions = new RowDefinitions("Auto,Auto"),
            ColumnSpacing = 10, Children = { headline, value }
        };
        Grid.SetColumn(value, 1);
        var body = new StackPanel { Spacing = 8, Children = { row } };
        foreach (var detail in details) body.Children.Add(detail);
        var panel = new Border
        {
            Classes = { "panel" }, Padding = new Thickness(14, 12),
            Child = body
        };
        panel.SizeChanged += (_, args) =>
        {
            var narrow = args.NewSize.Width < 390;
            row.ColumnDefinitions[1].Width = new GridLength(narrow ? 0 : 235);
            Grid.SetColumn(value, narrow ? 0 : 1);
            Grid.SetRow(value, narrow ? 1 : 0);
            value.Margin = new Thickness(0, narrow ? 6 : 0, 0, 0);
        };
        return panel;
    }

    private bool CanChoose => active && !closed && !confirmingLogout &&
        foregroundOperations == 0 && !TailscaleRuntime.IsTransitioning &&
        mode == TailscaleRuntime.Mode && readVersion == TailscaleRuntime.Generation;

    private void UpdateRuntimePanel()
    {
        settingSelection = true;
        try
        {
            var selected = mode == TailscaleMode.Embedded ? Embedded : SystemClient;
            var hasSystem = TailscaleRuntime.HasSystemRuntime;
            modeSelector.ItemsSource = hasSystem ? BothModes : EmbeddedOnly;
            modeSelector.IsVisible = TailscaleRuntime.SupportsEmbedded && (mode == TailscaleMode.Embedded || hasSystem);
            modeSelector.SelectedItem = modeSelector.IsVisible ? selected : null;
            fixedMode.Text = DescribeMode(mode, hasSystem);
            fixedMode.IsVisible = !modeSelector.IsVisible;
            UpdateProfileIdentity();
            var runtimeProgress = switchMessage != null && !switchingProfile;
            modeNote.Text = runtimeProgress ? switchMessage : mode switch
            {
                TailscaleMode.Embedded => "内置 Tailscale 由 Aurora 独立运行，可与系统 Tailscale 同时使用。",
                TailscaleMode.System when hasSystem => "Aurora 使用系统 Tailscale 的当前账号连接设备。",
                _ => null
            };
            modeNote.IsVisible = modeNote.Text != null;
            switchProgress.IsVisible = runtimeProgress;
            accountSwitchNote.IsVisible = accountSwitchProgress.IsVisible = switchMessage != null && switchingProfile;
            accountSwitchNote.Text = accountSwitchNote.IsVisible ? switchMessage : null;
            var animate = switchMessage != null && !RefreshNotice.ReduceMotion();
            switchProgress.IsIndeterminate = runtimeProgress && animate;
            accountSwitchProgress.IsIndeterminate = accountSwitchProgress.IsVisible && animate;
        }
        finally { settingSelection = false; }
        UpdateActions();
    }

    private void UpdateActions()
    {
        var enabled = CanChoose;
        modeSelector.IsEnabled = enabled;
        profileTrigger.IsEnabled = enabled;
        if (!enabled) CloseProfileMenu();
        retrySwitch.IsEnabled = enabled;
        action.IsEnabled = enabled && hasStatus;
        // The confirmation dialog restores focus to this button on cancellation.
        logout.IsEnabled = hasStatus && (enabled || confirmingLogout && foregroundOperations == 0);
        QtPresentationChanged?.Invoke();
    }

    private void SetProfiles(TailscaleProfile[] next)
    {
        var available = next?.Where(profile => !string.IsNullOrWhiteSpace(profile.Id) &&
            (!string.IsNullOrWhiteSpace(profile.Name) || !string.IsNullOrWhiteSpace(profile.LoginName)))
            .DistinctBy(profile => profile.Id, StringComparer.Ordinal).ToArray() ?? Array.Empty<TailscaleProfile>();
        if (!profiles.SequenceEqual(available))
        {
            if (profileMenu.IsOpen) CloseProfileMenu(restoreFocus: true);
            profiles = available;
            currentProfile = profiles.FirstOrDefault(profile => profile.IsCurrent);
            profileRows.Children.Clear();
            foreach (var profile in profiles) profileRows.Children.Add(CreateProfileRow(profile));
            Render();
        }
    }

    private void InvalidateAccount()
    {
        hasStatus = false;
        address = null;
        focusLoginAfterLogout = false;
        loginAfterSwitch = false;
        ClearAvatar();
    }

    private void SynchronizeMode()
    {
        var next = TailscaleRuntime.Mode;
        if (next == mode) return;
        mode = next;
        InvalidateAccount();
        SetProfiles(null);
        Render();
    }

    private void RuntimeChanging()
    {
        Dispatcher.UIThread.Post(() => { if (!closed) UpdateActions(); });
    }

    private void RuntimeChanged() => Dispatcher.UIThread.Post(() =>
    {
        if (closed || switchMessage != null || TailscaleRuntime.IsTransitioning) return;
        var version = TailscaleRuntime.Generation;
        var changed = mode != TailscaleRuntime.Mode || readVersion != version;
        readVersion = version;
        SynchronizeMode();
        if (!changed) return;
        SetRetry(null);
        InvalidateAccount();
        Render();
        if (active)
            _ = Run(() => Read(refreshAddress: true, forceStatus: true, refreshProfiles: true), keepFeedback: true);
    });

    private Task RequestModeSwitch(ModeChoice choice)
    {
        if (!CanChoose || choice.Mode == mode ||
            (choice.Mode == TailscaleMode.Embedded ? !TailscaleRuntime.SupportsEmbedded : !TailscaleRuntime.HasSystemRuntime))
            return Task.CompletedTask;

        return SwitchSelection(modeSelector.IsVisible ? modeSelector : action, () => choice.Mode != mode, () => mode == choice.Mode,
            () => ConfirmChange($"切换到{choice.Label}？",
                choice.Mode == TailscaleMode.Embedded
                    ? "Aurora 将通过内置 Tailscale 登录的网络发现和连接设备。"
                    : "Aurora 将通过系统 Tailscale 当前账号所在的网络发现和连接设备。"),
            () => TailscaleRuntime.SwitchAsync(choice.Mode), () => RequestModeSwitch(choice),
            $"正在切换到{choice.Label}…", $"已切换至{choice.Label}");
    }

    private Task RequestProfileSwitch(TailscaleProfile choice)
    {
        bool IsAvailable() => mode == TailscaleMode.System && choice.Id != currentProfile?.Id &&
            profiles.Any(profile => profile.Id == choice.Id);
        if (!CanChoose || !IsAvailable()) return Task.CompletedTask;
        var account = string.IsNullOrWhiteSpace(choice.LoginName) ? choice.Name : choice.LoginName;
        return SwitchSelection(profileTrigger, IsAvailable, () => currentProfile?.Id == choice.Id,
            () => hasActiveSession?.Invoke() == true
                ? ConfirmChange("切换系统 Tailscale 账号？",
                    $"将切换到 {account}，并更改系统 Tailscale 的当前账号。切换可能会断开正在进行的远控会话。")
                : Task.FromResult(true),
            () => TailscaleRuntime.SwitchSystemProfileAsync(choice.Id), () => RequestProfileSwitch(choice),
            "正在切换系统 Tailscale 账号…", $"已切换至 {account}");
    }

    private Task<bool> ConfirmChange(string title, string description)
    {
        if (confirmSwitch != null) return confirmSwitch(title, description);
        SetFeedback("无法打开切换确认。");
        return Task.FromResult(false);
    }

    private async Task SwitchSelection(Control selector, Func<bool> isApplicable, Func<bool> isConfirmed, Func<Task<bool>> approve,
        Func<Task> change, Func<Task> retryChange, string progress, string success)
    {
        var selectedMode = mode;
        var version = TailscaleRuntime.Generation;
        var declined = false;
        var succeeded = false;
        var forAccount = selector == profileTrigger;
        await Run(async () =>
        {
            if (!active || !IsCurrentContext(selectedMode, version) || !isApplicable()) return;
            if (!await approve())
            {
                declined = true;
                return;
            }
            if (!active || !IsCurrentContext(selectedMode, version) || !isApplicable()) return;
            switchingProfile = forAccount;
            switchMessage = progress;
            UpdateRuntimePanel();
            string failure = null;
            var retryable = false;
            try
            {
                await change();
                if (!closed)
                {
                    InvalidateAccount();
                    SetProfiles(null);
                    Render();
                }
            }
            catch (OperationCanceledException) { failure = "已取消切换。"; }
            catch (Exception error)
            {
                failure = error.Message;
                retryable = true;
            }

            try
            {
                if (closed) return;
                switchMessage = "正在检查当前 Tailscale 状态…";
                UpdateRuntimePanel();
                await Read(refreshAddress: true, forceStatus: true, refreshProfiles: true);
                if (failure == null && (!hasStatus || current.State == TailscaleState.Unknown || !isConfirmed()))
                    failure = "暂时无法确认切换结果，请刷新状态。";
                if (failure == null && current.State == TailscaleState.NeedsLogin)
                {
                    loginAfterSwitch = forAccount || current.OwnerId.HasValue;
                    focusPrimary = true;
                    Render();
                }
            }
            catch (Exception error) { failure ??= error.Message; }
            finally
            {
                switchMessage = null;
                switchingProfile = false;
                if (!closed)
                {
                    UpdateRuntimePanel();
                    if (failure != null)
                    {
                        SetFeedback(failure, runtime: !forAccount);
                        if (retryable && isApplicable()) SetRetry(retryChange);
                    }
                    else succeeded = true;
                }
            }
        });
        if (CanChoose && (declined || forAccount && !loginAfterSwitch) && selector.IsVisible) selector.Focus();
        if (succeeded && active && !closed) showSwitchSuccess?.Invoke(success);
    }

    private Control CreateHelp()
    {
        var help = new StackPanel
        {
            IsVisible = false, Spacing = 8, Margin = new Thickness(0, 0, 5, 0),
            Children =
            {
                new TextBlock
                {
                    Classes = { "muted" },
                    Text = "在两台设备上登录同一 Tailscale 账号，回到「我的设备」连接电脑。"
                        + "如果设备在线但无法连接，检查对端的 Sunshine 是否已运行。",
                    TextWrapping = TextWrapping.Wrap
                }
            }
        };
        var toggle = new ToggleButton
        {
            Classes = { "quietAction" }, HorizontalAlignment = HorizontalAlignment.Left
        };
        var arrow = AuroraVisual.SetTextAction(toggle, "连接帮助", AuroraVisual.ChevronDown);
        toggle.IsCheckedChanged += (_, _) =>
        {
            help.IsVisible = toggle.IsChecked == true;
            arrow.RenderTransform = help.IsVisible ? new RotateTransform(180) : null;
        };
        return new StackPanel { Spacing = 5, Margin = new Thickness(0, 8, 0, 0), Children = { toggle, help } };
    }

    private Task RequestPrimaryAction()
    {
        if (!CanChoose || !hasStatus || action.Content is not string { Length: > 0 }) return Task.CompletedTask;
        return mode == TailscaleMode.System && current.State == TailscaleState.NotInstalled
            ? RequestModeSwitch(Embedded) : Run(Activate);
    }

    private async Task Activate()
    {
        var state = current.State;
        var selectedMode = mode;
        var version = TailscaleRuntime.Generation;
        if (!IsCurrentContext(selectedMode, version)) return;

        if (state == TailscaleState.NotInstalled)
        {
            if (selectedMode != TailscaleMode.Embedded || !TailscaleRuntime.SupportsEmbedded) return;
        }
        else if (state is not (TailscaleState.Stopped or TailscaleState.NeedsLogin)) return;

        if (state != TailscaleState.NeedsLogin || current.LoginUri == null)
        {
            await Task.Run(Upstream.RequestTailscaleConnection);
            if (!IsCurrentContext(selectedMode, version)) return;
            await Read();
        }
        if (!active || !IsCurrentContext(selectedMode, version) ||
            state != TailscaleState.NeedsLogin || current.State != TailscaleState.NeedsLogin) return;

        if (current.LoginUri == null)
            SetFeedback("正在等待 Tailscale 生成官方登录链接。链接就绪后，请再次点击「登录 Tailscale」。");
        else if (!IsOfficialLoginUri(current.LoginUri))
            SetFeedback("无法验证 Tailscale 登录链接，请刷新状态后重试。");
        else if (Open(current.LoginUri.AbsoluteUri))
        {
            SetFeedback("请在浏览器完成 Tailscale 登录，Aurora 会自动刷新。");
        }
    }

    private static bool IsOfficialLoginUri(Uri uri) =>
        uri is { IsAbsoluteUri: true, Scheme: "https", Host: "login.tailscale.com", IsDefaultPort: true, UserInfo.Length: 0 }
        && uri.AbsolutePath.StartsWith("/a/", StringComparison.Ordinal);

    private async Task RequestLogout()
    {
        if (!CanChoose || !hasStatus || current.State != TailscaleState.Connected) return;
        var selectedMode = mode;
        var version = TailscaleRuntime.Generation;
        var owner = current.OwnerId;
        confirmingLogout = true;
        UpdateActions();
        try
        {
            if (!await confirmLogout() || closed) return;
            await Run(async () =>
            {
                if (!active || !IsCurrentContext(selectedMode, version) || !hasStatus ||
                    current.State != TailscaleState.Connected || current.OwnerId != owner) return;
                await Task.Run(() => Upstream.LogoutTailscale());
                if (closed) return;
                focusLoginAfterLogout = active;
                ClearAvatar();
                await Read(forceStatus: true, refreshProfiles: true);
            });
        }
        catch (Exception error)
        {
            SetFeedback(error.Message);
        }
        finally
        {
            confirmingLogout = false;
            if (!closed) UpdateActions();
        }
    }

    private bool IsCurrentContext(TailscaleMode selectedMode, int version) =>
        !closed && !TailscaleRuntime.IsTransitioning && selectedMode == TailscaleRuntime.Mode &&
        version == TailscaleRuntime.Generation;

    private async Task Read(bool refreshAddress = false, bool forceStatus = false, bool refreshProfiles = false)
    {
        if (closed || TailscaleRuntime.IsTransitioning) return;
        SynchronizeMode();
        var selectedMode = mode;
        var version = readVersion = TailscaleRuntime.Generation;
        TailscaleStatus next;
        try { next = await Task.Run(Upstream.ReadTailscale); }
        catch
        {
            if (!IsCurrentContext(selectedMode, version)) return;
            address = null;
            Publish(new(TailscaleState.Unknown), forceStatus);
            if (refreshProfiles) await RefreshSystemProfiles(selectedMode, version);
            throw;
        }
        if (!IsCurrentContext(selectedMode, version)) return;

        var accountChanged = hasStatus && (current.OwnerId != next.OwnerId || current.TailnetName != next.TailnetName);
        var networkChanged = !hasStatus || current.State != next.State || accountChanged;
        if (networkChanged) address = null;
        if (accountChanged && selectedMode == TailscaleMode.System) SetProfiles(null);
        Publish(next, forceStatus);
        if (IsCurrentContext(selectedMode, version) && next.State == TailscaleState.Connected && (networkChanged || refreshAddress))
        {
            string nextAddress = null;
            try { nextAddress = (await Task.Run(Upstream.ReadNetwork)).LocalAddress?.ToString(); }
            catch (Exception error)
            {
                Debug.WriteLine(error);
            }
            if (!IsCurrentContext(selectedMode, version)) return;
            address = nextAddress;
            Render();
        }
        if (refreshProfiles || accountChanged) await RefreshSystemProfiles(selectedMode, version);
    }

    private async Task RefreshSystemProfiles(TailscaleMode selectedMode, int version)
    {
        if (!active || selectedMode != TailscaleMode.System || !IsCurrentContext(selectedMode, version)) return;
        TailscaleProfile[] next = null;
        try { next = await Task.Run(TailscaleRuntime.ReadSystemProfiles); }
        catch (NotSupportedException) { }
        catch (Exception error) { Debug.WriteLine(error); }
        if (active && IsCurrentContext(selectedMode, version)) SetProfiles(next);
    }

    private void Publish(TailscaleStatus next, bool force)
    {
        var notify = !hasStatus || current != next || force;
        if (next.State != TailscaleState.NeedsLogin) loginAfterSwitch = false;
        hasStatus = true;
        current = next;
        Render();
        if (notify) StatusChanged?.Invoke(current);
    }

    internal static string DescribeMode(TailscaleMode mode, bool hasSystem) =>
        mode == TailscaleMode.Embedded ? Embedded.Label : hasSystem ? SystemClient.Label : "暂不可用";

    internal static (string Status, string Detail, string Action) DescribeStatus(
        TailscaleState? state, TailscaleMode mode, bool supportsEmbedded, bool loginAfterSwitch = false) =>
        state switch
        {
            null => ("正在检查", "正在读取当前运行方式的连接状态。", ""),
            TailscaleState.NotInstalled when mode == TailscaleMode.Embedded && supportsEmbedded =>
                ("未启动", "启动内置 Tailscale 后即可登录。", "启动内置 Tailscale"),
            TailscaleState.NotInstalled when supportsEmbedded =>
                ("暂不可用", "可以使用内置 Tailscale 连接设备。", "使用内置 Tailscale"),
            TailscaleState.NotInstalled => ("暂不可用", "当前无法建立连接，可刷新状态后重试。", ""),
            TailscaleState.NeedsLogin when loginAfterSwitch =>
                ("未登录", "此账号需要重新登录。", "登录 Tailscale"),
            TailscaleState.NeedsLogin => ("未登录", "登录后可发现同一 Tailscale 网络中的设备。", "登录 Tailscale"),
            TailscaleState.NeedsApproval => ("等待批准", "你的 Tailscale 网络需要管理员批准这台设备。批准后会自动更新。", ""),
            TailscaleState.Stopped when mode == TailscaleMode.Embedded =>
                ("已停止", "启动内置 Tailscale 后即可连接设备。", "启动内置 Tailscale"),
            TailscaleState.Stopped => ("已暂停", "恢复异地连接并保留现有设置。", "恢复连接"),
            TailscaleState.Starting => ("正在启动", "稍等片刻，Aurora 会自动刷新。", ""),
            TailscaleState.Connected => ("已连接", "", ""),
            _ when mode == TailscaleMode.Embedded => ("状态未知", "暂时无法读取内置 Tailscale，请刷新状态后重试。", ""),
            _ => ("状态未知", "暂时无法读取 Tailscale。请确认系统客户端和后台服务已启动，再刷新状态。", "")
        };
    private void Render()
    {
        (status.Text, details.Text, action.Content) =
            DescribeStatus(hasStatus ? current.State : null, mode, TailscaleRuntime.SupportsEmbedded, loginAfterSwitch);
        var connected = hasStatus && current.State == TailscaleState.Connected;
        var ink = !hasStatus ? StatusGray : current.State switch
        {
            TailscaleState.Connected => StatusBlue,
            TailscaleState.NeedsLogin or TailscaleState.NeedsApproval or TailscaleState.Starting => StatusAmber,
            _ => StatusGray
        };
        status.Foreground = statusMark.Stroke = ink;
        statusMark.Fill = connected ? ink : Brushes.Transparent;
        var profile = hasStatus && mode == TailscaleMode.System ? currentProfile : null;
        var actualLogin = connected ? current.LoginName ?? profile?.LoginName : profile?.LoginName;
        displayName.Text = (connected ? current.DisplayName : null) ?? profile?.Name ?? actualLogin ?? "Tailscale";
        loginName.Text = actualLogin != displayName.Text ? actualLogin : null;
        loginName.IsVisible = loginName.Text != null;
        initial.Text = connected ? current.AccountInitial : null;
        UpdateAvatar();
        RenderAvatar();
        connection.IsVisible = logout.IsVisible = connected;
        details.IsVisible = !connected;
        networkName.Text = connected ? current.TailnetName ?? "暂不可用" : null;
        localAddress.Text = connected ? address ?? "暂不可用" : null;
        action.IsVisible = action.Content is string text && text.Length != 0;
        actions.IsVisible = action.IsVisible;
        UpdateRuntimePanel();
    }

    private bool CanRenderAvatar => !externalPresentation && !closed && active && hasStatus && !focusLoginAfterLogout && IsEffectivelyVisible &&
        current.State == TailscaleState.Connected && avatarWindow is { IsVisible: true, WindowState: not WindowState.Minimized };

    private void UpdateAvatar()
    {
        (long Owner, Uri Uri)? key = CanRenderAvatar && current.OwnerId is long owner && current.AvatarUri != null
            ? (owner, current.AvatarUri) : null;
        if (key == avatarKey) return;

        ClearAvatar();
        if (key == null) return;
        avatarKey = key;
        var request = new CancellationTokenSource();
        avatarRequest = request;
        _ = LoadAvatar(current, request);
    }

    private async Task LoadAvatar(TailscaleStatus account, CancellationTokenSource request)
    {
        try
        {
            var image = await TailscaleAvatar.LoadAsync(account, request.Token);
            if (image == null) return;
            if (avatarRequest != request || request.IsCancellationRequested || !CanRenderAvatar ||
                current.OwnerId != account.OwnerId || current.AvatarUri != account.AvatarUri)
            {
                image.Dispose();
                return;
            }
            avatarBitmap = image;
            avatar.Background = new ImageBrush { Source = image, Stretch = Stretch.UniformToFill };
            RenderAvatar();
        }
        catch (OperationCanceledException) when (request.IsCancellationRequested) { }
        finally
        {
            if (avatarRequest == request) avatarRequest = null;
            request.Dispose();
        }
    }

    private void ClearAvatar()
    {
        var request = avatarRequest;
        avatarRequest = null;
        avatarKey = null;
        request?.Cancel();
        avatar.Background = AvatarBackground;
        avatarBitmap?.Dispose();
        avatarBitmap = null;
        RenderAvatar();
    }

    private void RenderAvatar()
    {
        initial.IsVisible = avatarBitmap == null;
        avatar.IsVisible = avatarBitmap != null || initial.Text != null;
        tailscaleMark.IsVisible = !avatar.IsVisible;
    }

    private void TryFocusPrimaryAction()
    {
        if (!active || closed || !hasStatus || (!focusPrimary && !focusLoginAfterLogout)) return;
        if (focusLoginAfterLogout && current.State is not (TailscaleState.NeedsLogin or TailscaleState.Stopped or TailscaleState.NotInstalled)) return;
        if (externalPresentation && CanChoose && action.IsVisible)
        {
            focusPrimary = focusLoginAfterLogout = false;
            qtFocusVersion++;
            QtPresentationChanged?.Invoke();
            return;
        }
        var target = current.State == TailscaleState.Connected ? (profileTrigger.IsVisible ? profileTrigger : logout) : action;
        if (target.IsVisible && target.IsEnabled && target.Focus())
            focusPrimary = focusLoginAfterLogout = false;
    }

    private async Task Run(Func<Task> work, bool background = false, bool keepFeedback = false)
    {
        if (closed) return;
        if (background)
        {
            if (confirmingLogout || !operation.Wait(0)) return;
        }
        else
        {
            foregroundOperations++;
            UpdateActions();
            await operation.WaitAsync();
        }

        try
        {
            if (closed) return;
            if (!background && !keepFeedback) SetFeedback(null);
            await work();
        }
        catch (Exception error)
        {
            if (!background) SetFeedback(error.Message);
        }
        finally
        {
            if (!background) foregroundOperations--;
            operation.Release();
            if (!closed)
            {
                UpdateActions();
                TryFocusPrimaryAction();
            }
        }
    }

    private void RouteFeedback(bool runtime)
    {
        var target = runtime ? runtimeFeedback : accountFeedback;
        if (target.Content != null) return;
        var previous = runtime ? accountFeedback : runtimeFeedback;
        previous.Content = null;
        previous.IsVisible = false;
        target.Content = operationFeedback;
        target.IsVisible = feedback.IsVisible;
    }

    private void SetFeedback(string message, bool runtime = false)
    {
        if (closed || !active) return;
        SetRetry(null);
        if (message != null) RouteFeedback(runtime);
        feedback.ShowPersistent(message);
        QtPresentationChanged?.Invoke();
    }

    private void SetRetry(Func<Task> change)
    {
        retry = active ? change : null;
        retrySwitch.IsVisible = retry != null;
        QtPresentationChanged?.Invoke();
    }

    private bool Open(string url)
    {
        try
        {
            using var browser = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            return true;
        }
        catch (Exception error)
        {
            SetFeedback("无法打开：" + error.Message);
            return false;
        }
    }
}
