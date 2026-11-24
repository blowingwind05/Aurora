using System;
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

internal sealed class TailscalePage : UserControl, IDisposable
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
    private readonly TailscaleAccountSession session;
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
    private TailscaleProfile[] renderedProfiles = [];
    private TailscaleAccountFeedback renderedFeedback;
    private bool settingSelection;
    private bool compact;
    private bool active;
    private bool closed;
    private bool focusPrimary;
    private Window avatarWindow;
    private (long Owner, Uri Uri)? avatarKey;
    private CancellationTokenSource avatarRequest;
    private Bitmap avatarBitmap;

    public TailscalePage(TailscaleAccountSession session)
    {
        this.session = session;
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
        retrySwitch.Click += async (_, _) => await session.RequestRetry();
        logout.Click += async (_, _) => await session.RequestLogout();
        action.Click += async (_, _) => await session.RequestPrimaryAction();
        session.Changed += Render;
        session.FocusRequested += FocusRequested;
        Render();
    }

    public Task RefreshStatus()
    {
        if (avatarBitmap == null && avatarRequest == null) avatarKey = null;
        return session.RefreshStatus();
    }

    public void FocusPrimaryAction()
    {
        if (closed) return;
        focusPrimary = true;
        Dispatcher.UIThread.Post(TryFocusPrimaryAction, DispatcherPriority.Loaded);
    }

    public void SetActive(bool value)
    {
        if (closed || active == value) return;
        active = value;
        if (!active)
        {
            CloseProfileMenu();
            focusPrimary = false;
            renderedFeedback = null;
            feedback.Clear();
            ClearAvatar();
        }
        Render();
    }

    public void Dispose()
    {
        closed = true;
        session.Changed -= Render;
        session.FocusRequested -= FocusRequested;
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
            if (choice == null || choice.Mode == session.Mode) return;
            modeSelector.IsDropDownOpen = false;
            await session.RequestModeSwitch(choice.Mode);
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
            else if (session.CanChoose && renderedProfiles.Length > 0) profileMenu.IsOpen = true;
        };
        profileTrigger.AddHandler(KeyDownEvent, (_, e) =>
        {
            if (e.Key is not (Key.Up or Key.Down) || !session.CanChoose || renderedProfiles.Length == 0) return;
            profileMenu.IsOpen = true;
            e.Handled = true;
        }, RoutingStrategies.Tunnel);
        profileMenu.Opened += (_, _) =>
        {
            profileTrigger.Classes.Add("selected");
            profileChevron.RenderTransform = new RotateTransform(180);
            Dispatcher.UIThread.Post(() =>
            {
                if (profileMenu.IsOpen) FocusProfileRow(Math.Max(0, Array.IndexOf(renderedProfiles, session.CurrentProfile)));
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
            if (!profile.IsCurrent) await session.RequestProfileSwitch(profile);
        };
        return row;
    }

    private void UpdateProfileIdentity()
    {
        var interactive = renderedProfiles.Length > 0;
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

    private void UpdateRuntimePanel()
    {
        settingSelection = true;
        try
        {
            var mode = session.Mode;
            var selected = mode == TailscaleMode.Embedded ? Embedded : SystemClient;
            var hasSystem = session.HasSystemRuntime;
            modeSelector.ItemsSource = hasSystem ? BothModes : EmbeddedOnly;
            modeSelector.IsVisible = session.SupportsEmbedded && (mode == TailscaleMode.Embedded || hasSystem);
            modeSelector.SelectedItem = modeSelector.IsVisible ? selected : null;
            fixedMode.Text = TailscaleAccountSession.DescribeMode(mode, hasSystem);
            fixedMode.IsVisible = !modeSelector.IsVisible;
            UpdateProfileIdentity();
            var switchMessage = session.Progress;
            var runtimeProgress = session.RuntimeProgress;
            modeNote.Text = runtimeProgress ? switchMessage : mode switch
            {
                TailscaleMode.Embedded => "内置 Tailscale 由 Aurora 独立运行，可与系统 Tailscale 同时使用。",
                TailscaleMode.System when hasSystem => "Aurora 使用系统 Tailscale 的当前账号连接设备。",
                _ => null
            };
            modeNote.IsVisible = modeNote.Text != null;
            switchProgress.IsVisible = runtimeProgress;
            accountSwitchNote.IsVisible = accountSwitchProgress.IsVisible = switchMessage != null && !runtimeProgress;
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
        var enabled = active && session.CanChoose;
        modeSelector.IsEnabled = enabled;
        profileTrigger.IsEnabled = enabled;
        if (!enabled) CloseProfileMenu();
        retrySwitch.IsEnabled = enabled;
        retrySwitch.IsVisible = active && session.RetryAvailable;
        action.IsEnabled = enabled;
        // The confirmation dialog restores focus to this button on cancellation.
        logout.IsEnabled = active && session.CanLogout;
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

    private void Render()
    {
        var view = session.ReadView();
        status.Text = view.Status;
        details.Text = view.Detail;
        action.Content = view.PrimaryAction;
        var connected = view.State == TailscaleState.Connected;
        var ink = view.State switch
        {
            TailscaleState.Connected => StatusBlue,
            TailscaleState.NeedsLogin or TailscaleState.NeedsApproval or TailscaleState.Starting => StatusAmber,
            _ => StatusGray
        };
        status.Foreground = statusMark.Stroke = ink;
        statusMark.Fill = connected ? ink : Brushes.Transparent;
        displayName.Text = view.DisplayName;
        loginName.Text = view.LoginName;
        loginName.IsVisible = loginName.Text != null;
        initial.Text = view.Initial;
        UpdateAvatar();
        RenderAvatar();
        connection.IsVisible = logout.IsVisible = connected;
        details.IsVisible = !connected;
        networkName.Text = view.TailnetName;
        localAddress.Text = view.LocalAddress;
        action.IsVisible = view.PrimaryAction.Length != 0;
        actions.IsVisible = action.IsVisible;
        RenderProfiles();
        UpdateRuntimePanel();

        var nextFeedback = active ? session.Feedback : null;
        if (!ReferenceEquals(renderedFeedback, nextFeedback))
        {
            renderedFeedback = nextFeedback;
            if (nextFeedback != null) RouteFeedback(nextFeedback.Runtime);
            if (nextFeedback?.Timed == true) feedback.ShowRefreshSuccess(nextFeedback.Message);
            else feedback.ShowPersistent(nextFeedback?.Message);
        }
        TryFocusPrimaryAction();
    }

    private void RenderProfiles()
    {
        var profiles = session.Profiles;
        if (ReferenceEquals(renderedProfiles, profiles)) return;
        if (profileMenu.IsOpen) CloseProfileMenu(restoreFocus: true);
        renderedProfiles = profiles;
        profileRows.Children.Clear();
        foreach (var profile in profiles) profileRows.Children.Add(CreateProfileRow(profile));
    }

    private bool CanRenderAvatar => !closed && active && !session.WaitingForLoginFocus && IsEffectivelyVisible &&
        session.IsConnected && avatarWindow is { IsVisible: true, WindowState: not WindowState.Minimized };

    private void UpdateAvatar()
    {
        (long Owner, Uri Uri)? key = CanRenderAvatar && session.Current.OwnerId is long owner && session.Current.AvatarUri != null
            ? (owner, session.Current.AvatarUri) : null;
        if (key == avatarKey) return;

        ClearAvatar();
        if (key == null) return;
        avatarKey = key;
        var request = new CancellationTokenSource();
        avatarRequest = request;
        _ = LoadAvatar(session.Current, request);
    }

    private async Task LoadAvatar(TailscaleStatus account, CancellationTokenSource request)
    {
        try
        {
            var image = await TailscaleAvatar.LoadAsync(account, request.Token);
            if (image == null) return;
            if (avatarRequest != request || request.IsCancellationRequested || !CanRenderAvatar ||
                session.Current.OwnerId != account.OwnerId || session.Current.AvatarUri != account.AvatarUri)
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

    private void FocusRequested(TailscaleAccountFocus destination)
    {
        if (!active || !IsEffectivelyVisible) return;
        switch (destination)
        {
            case TailscaleAccountFocus.Primary:
                FocusPrimaryAction();
                break;
            case TailscaleAccountFocus.Mode:
                (modeSelector.IsVisible ? (Control)modeSelector : action).Focus();
                break;
            case TailscaleAccountFocus.Profile when profileTrigger.IsVisible:
                profileTrigger.Focus();
                break;
        }
    }

    private void TryFocusPrimaryAction()
    {
        if (!active || closed || !focusPrimary || !session.CanChoose) return;
        var target = session.IsConnected ? (profileTrigger.IsVisible ? profileTrigger : logout) : action;
        if (target.IsVisible && target.IsEnabled && target.Focus()) focusPrimary = false;
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
}
