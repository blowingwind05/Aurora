using System;
using System.IO;
using System.Linq;
using System.Linq.Expressions;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Presenters;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Aurora;
using SkiaSharp;

internal static class WindowsVisualTests
{
    private const BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Instance;

    [STAThread]
    private static int Main(string[] args)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Run on the Windows Avalonia backend.");
        var output = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "AuroraVisualTests");
        var baseline = args.Contains("--baseline");
        Directory.CreateDirectory(output);
        AppBuilder.Configure<AuroraApp>().UsePlatformDetect().SetupWithoutStarting();
        using var completion = new CancellationTokenSource(TimeSpan.FromSeconds(35));
        Exception failure = new TimeoutException("The visual tests did not finish.");
        Dispatcher.UIThread.Post(async () =>
        {
            try
            {
                if (args.Contains("--stream")) await CheckStreamWindow(output);
                else
                {
                    await CheckButtons(output, baseline);
                    if (!baseline)
                    {
                        await CheckStreamWindow(output);
                        await CheckHost(output);
                        await CheckStartup(output);
                    }
                }
                failure = null;
            }
            catch (Exception error) { failure = error; }
            finally { completion.Cancel(); }
        });
        Dispatcher.UIThread.MainLoop(completion.Token);
        if (failure == null) return 0;
        Console.Error.WriteLine(failure);
        return 1;
    }

    private static Window Preview(double width, Control content) => new()
    {
        Width = width, Height = 675, Content = content, ShowActivated = false, ShowInTaskbar = false, Opacity = 0,
        Background = Brushes.White, FontFamily = new FontFamily("Segoe UI, Microsoft YaHei UI")
    };

    private static async Task CheckButtons(string output, bool baseline)
    {
        var normal = new Button { Content = "重启 Sunshine", Classes = { "action" } };
        var login = new Button { Content = "登录 Tailscale", Classes = { "action", "accent" } };
        var go = new Button { Content = "前往登录", Classes = { "action", "accent" } };
        var english = new Button { Content = "Apply changes", Classes = { "action" } };
        var dynamic = new Button { Content = "关闭被控", Classes = { "action", "danger" } };
        var buttons = new[] { login, go, normal, english, dynamic };
        var grid = new StackPanel { HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left, Spacing = 8, Margin = new Thickness(16) };
        foreach (var button in buttons) grid.Children.Add(button);
        var dataContext = new object();
        var icon = new Border { Width = 12, Height = 12, Background = Brushes.CornflowerBlue };
        var visualButton = new Button { Classes = { "action" }, Content = icon, DataContext = dataContext };
        grid.Children.Add(visualButton);
        var buttonSheet = new Border { Background = Brush.Parse("#F4F6FA"), Child = grid };
        var window = Preview(520, buttonSheet);
        window.Show();
        try
        {
            await Until(() => buttons.All(button => button.Bounds.Height > 0));
            Require(Presenter(visualButton).Child == icon && icon.DataContext == dataContext,
                "The text style replaced custom visual content or changed its data context.");
            dynamic.Content = "关闭 Sunshine 并应用 Display 配置";
            await Until(() => Presenter(dynamic).Child is TextBlock text && text.Text == (string)dynamic.Content);
            Console.WriteLine($"Native Windows scale {window.RenderScaling:0.##}. Rendered at 100% and 125%.");
            foreach (var scale in new[] { 1.0, 1.25 })
            {
                foreach (var button in buttons)
                {
                    var samples = new[] { "normal", "hover", "disabled", "keyboard" };
                    Size frame = button.Bounds.Size;
                    foreach (var state in samples)
                    {
                        var pseudo = (IPseudoClasses)button.Classes;
                        if (state == "hover") pseudo.Add(":pointerover");
                        if (state == "disabled") button.IsEnabled = false;
                        if (state == "keyboard")
                            Require(button.Focus(NavigationMethod.Tab), $"Keyboard focus failed for {button.Content}.");
                        try
                        {
                            await Task.Delay(100);
                            var presenter = Presenter(button);
                            var text = (TextBlock)presenter.Child;
                            var buttonCenter = button.Bounds.Height / 2;
                            var presenterCenter = presenter.TranslatePoint(new Point(presenter.Bounds.Width / 2, presenter.Bounds.Height / 2), button).Value.Y;
                            var textCenter = text.GetVisualParent().TranslatePoint(text.Bounds.Center, button).Value.Y;
                            var renderedCenter = text.TranslatePoint(new Point(text.Bounds.Width / 2, text.Bounds.Height / 2), button).Value.Y;
                            using var picture = Draw(button, scale);
                            if (state == "disabled" && button == login)
                                picture.Save(Path.Combine(output, $"disabled-{(baseline ? "before" : "after")}-{scale * 100:0}.png"));
                            var inkCenter = InkCenter(picture, scale);
                            Console.WriteLine($"{scale:0%} {state,-8} {button.Content}: {presenter.VerticalContentAlignment}, frame={buttonCenter:0.00}, presenter={presenterCenter:0.00}, text={textCenter:0.00} ({text.Bounds.Height:0.00} high), line={text.TextLayout.Height:0.00}, ink={inkCenter:0.00} DIP");
                            Require(frame == button.Bounds.Size, "The interaction changed the click area.");
                            if (!baseline)
                            {
                                Require(Math.Abs(presenterCenter - buttonCenter) <= 0.51 && Math.Abs(textCenter - buttonCenter) <= 0.51,
                                    "The button or text layout box is off-center.");
                                Require(Math.Abs(renderedCenter - textCenter) <= 0.1, "The alignment moved the text visual's box.");
                                Require(Math.Abs(inkCenter - buttonCenter) <= 1, "Visible ink is off-center.");
                                if (state == "keyboard") Require(button.IsFocused, "Keyboard focus was lost.");
                            }
                        }
                        finally
                        {
                            pseudo.Remove(":pointerover");
                            button.IsEnabled = true;
                        }
                    }
                }
                using var screenshot = Draw(buttonSheet, scale);
                screenshot.Save(Path.Combine(output, $"buttons-{(baseline ? "before" : "after")}-{scale * 100:0}.png"));
            }
        }
        finally { window.Close(); }
    }

    private static ContentPresenter Presenter(Button button) => button.GetVisualDescendants().OfType<ContentPresenter>()
        .Single(node => node.Name == "PART_ContentPresenter");

    private static RenderTargetBitmap Draw(Control control, double scale)
    {
        var pixels = new PixelSize((int)Math.Ceiling(control.Bounds.Width * scale), (int)Math.Ceiling(control.Bounds.Height * scale));
        var bitmap = new RenderTargetBitmap(pixels, new Vector(96 * scale, 96 * scale));
        bitmap.Render(control);
        return bitmap;
    }

    private static double InkCenter(RenderTargetBitmap picture, double scale)
    {
        using var encoded = new MemoryStream();
        picture.Save(encoded);
        encoded.Position = 0;
        using var pixels = SKBitmap.Decode(encoded);
        var top = pixels.Height;
        var bottom = -1;
        var safe = (int)Math.Ceiling(6 * scale);
        for (var y = safe; y < pixels.Height - safe; y++)
        {
            var background = pixels.GetPixel(safe, y);
            for (var x = safe + 1; x < pixels.Width - safe; x++)
            {
                var pixel = pixels.GetPixel(x, y);
                if (pixel.Alpha < 32 || Math.Max(Math.Abs(pixel.Red - background.Red),
                    Math.Max(Math.Abs(pixel.Green - background.Green), Math.Abs(pixel.Blue - background.Blue))) < 35) continue;
                top = Math.Min(top, y);
                bottom = Math.Max(bottom, y);
            }
        }
        Require(bottom >= top, "No text ink was found.");
        return (top + bottom + 1) / (2 * scale);
    }

    private static async Task CheckHost(string output)
    {
        var assembly = typeof(AuroraWindow).Assembly;
        var hostStatus = assembly.GetType("Aurora.HostStatus", true);
        var managed = true;
        var running = true;
        var rejectsStop = false;
        var starts = 0;
        var stops = 0;
        object ReadStatus()
        {
            var status = Activator.CreateInstance(hostStatus);
            hostStatus.GetField("Managed").SetValue(status, managed);
            hostStatus.GetField("Installed").SetValue(status, running);
            hostStatus.GetField("SettingsAvailable").SetValue(status, running);
            return status;
        }
        var read = Expression.Lambda(typeof(Func<>).MakeGenericType(hostStatus),
            Expression.Convert(Expression.Invoke(Expression.Constant((Func<object>)ReadStatus)), hostStatus)).Compile();
        Func<Task> start = () => { starts++; running = true; return Task.CompletedTask; };
        Func<Task> stop = () =>
        {
            stops++;
            if (rejectsStop) throw new InvalidOperationException("测试停止失败");
            running = false;
            return Task.CompletedTask;
        };
        var shell = (AuroraWindow)Activator.CreateInstance(typeof(AuroraWindow), Private, null, new object[] { read, start, stop }, null);
        typeof(AuroraWindow).GetField("host", Private).SetValue(shell, ReadStatus());
        shell.FindControl<Grid>("DevicesPage").IsVisible = false;
        shell.FindControl<Grid>("HostPage").IsVisible = true;
        shell.FindControl<TextBlock>("PageTitle").Text = "本机被控";
        var mainContent = (Control)shell.Content;
        shell.Content = null;
        var window = Preview(1000, mainContent);
        var allow = shell.FindControl<ToggleSwitch>("AllowManagedHost");
        var confirmation = shell.FindControl<Grid>("ConfirmationOverlay");
        var cancel = shell.FindControl<Button>("CancelConfirmation");
        var apply = shell.FindControl<Button>("ApplyConfirmation");
        var addresses = shell.FindControl<Grid>("HostAddresses");
        var settings = shell.FindControl<ContentControl>("HostSettings");
        var sunshine = new SunshinePage();
        settings.Content = sunshine;
        sunshine.FindControl<Border>("SettingsPanel").IsVisible = true;
        sunshine.FindControl<Border>("PrivacySettings").IsVisible = true;
        using var sunshineLifetime = sunshine;
        window.Show();
        try
        {
            Call(shell, "RenderHost", (object)null);
            Call(shell, "UpdateActions");
            foreach (var width in new[] { 1000.0, 449.0 })
            {
                window.Width = width;
                Call(shell, "ResizeShell", width);
                await Until(() => Math.Abs(window.ClientSize.Width - width) < 1.5 && sunshine.Bounds.Width > 0);
                window.UpdateLayout();
                CheckHeading(sunshine, "CommonHeading", "SettingsPanel", out var common);
                CheckHeading(sunshine, "DisplayHeading", "PrivacySettings", out var display);
                Require(Math.Abs(common.X - display.X) < 0.6 && Math.Abs(common.Y - display.Y) < 0.6,
                    $"Group headers differ at {width} DIP: {common}, {display}.");
                Console.WriteLine($"{width:0} DIP headings share the card inset {common.X:0.00}, {common.Y:0.00}.");
                using (var picture = Draw(mainContent, 1.0)) picture.Save(Path.Combine(output, $"host-{width:0}.png"));
                allow.Focus(NavigationMethod.Tab);
                window.UpdateLayout();
                var addressPosition = addresses.TranslatePoint(default, mainContent).Value;
                allow.IsChecked = false;
                Require(confirmation.IsVisible && allow.IsChecked == true && stops == 0, "Stopping needs confirmation.");
                window.UpdateLayout();
                var dialog = confirmation.Children.Single();
                var dialogCenter = dialog.TranslatePoint(new Point(dialog.Bounds.Width / 2, dialog.Bounds.Height / 2), confirmation).Value;
                Require(Math.Abs(dialogCenter.X - confirmation.Bounds.Width / 2) < 0.8 &&
                    Math.Abs(dialogCenter.Y - confirmation.Bounds.Height / 2) < 0.8, "Confirmation must stay centered over the page.");
                Require(cancel.IsFocused && !allow.IsEffectivelyEnabled && Equals(apply.Content, "关闭被控"),
                    "The dialog must focus Cancel and block controls behind it.");
                var dialogAddressPosition = addresses.TranslatePoint(default, mainContent).Value;
                Require(Math.Abs(dialogAddressPosition.X - addressPosition.X) < 0.1 && Math.Abs(dialogAddressPosition.Y - addressPosition.Y) < 0.1,
                    $"Showing the dialog moved the address area from {addressPosition} to {dialogAddressPosition}.");
                using (var picture = Draw(mainContent, 1.0)) picture.Save(Path.Combine(output, $"host-confirm-{width:0}.png"));
                if (width > 470) Click(cancel);
                else cancel.RaiseEvent(new KeyEventArgs { RoutedEvent = InputElement.KeyDownEvent, Key = Key.Escape });
                Require(!confirmation.IsVisible && allow.IsChecked == true && stops == 0, "Cancel must leave the host enabled.");
                Require(allow.IsFocused, "Cancel must return focus to the managed switch.");
            }
            rejectsStop = true;
            allow.IsChecked = false;
            Click(apply);
            await Until(() => allow.IsEnabled && stops == 1);
            Require(allow.IsChecked == true && addresses.IsVisible, "A failed stop must keep the actual running state.");
            rejectsStop = false;
            allow.IsChecked = false;
            Click(apply);
            await Until(() => allow.IsEnabled && allow.IsChecked == false && stops == 2);
            Require(!addresses.IsVisible && !settings.IsVisible, "A stopped host must hide addresses and settings.");
            allow.IsChecked = true;
            await Until(() => allow.IsEnabled && allow.IsChecked == true && starts == 1);
            Require(addresses.IsVisible && settings.IsVisible, "Starting again must restore addresses and settings.");
            managed = false;
            typeof(AuroraWindow).GetField("host", Private).SetValue(shell, ReadStatus());
            Call(shell, "RenderHost", (object)null);
            Require(!shell.FindControl<Grid>("ManagedHostRow").IsVisible && starts == 1 && stops == 2,
                "External Sunshine must stay outside the managed switch.");
            Console.WriteLine("Managed hosting confirmed, canceled, escaped, rejected and restarted. External Sunshine stayed outside the switch.");
        }
        finally { window.Close(); }
    }

    private static void CheckHeading(SunshinePage page, string title, string card, out Point center)
    {
        var text = page.FindControl<TextBlock>(title);
        center = text.TranslatePoint(new Point(0, text.Bounds.Height / 2), page.FindControl<Border>(card)).Value;
    }

    private static async Task CheckStartup(string output)
    {
        var assembly = typeof(AuroraWindow).Assembly;
        var directory = Path.Combine(Path.GetTempPath(), "AuroraStartupTest-" + Guid.NewGuid().ToString("N"));
        var apply = assembly.GetType("Aurora.StartupRegistration", true).GetMethod("ApplyDefault", BindingFlags.Static | BindingFlags.NonPublic);
        var registered = false;
        var registrations = 0;
        var arguments = new object[] { directory, (Func<bool>)(() => registered), (Action)(() => { registered = true; registrations++; }) };
        apply.Invoke(null, arguments);
        Require(registered && registrations == 1, "The first launch must default to startup enabled.");
        registered = false;
        apply.Invoke(null, arguments);
        Require(!registered && registrations == 1, "An explicit opt-out must survive later launches.");
        File.Delete(Path.Combine(directory, "startup-initialized"));
        Directory.Delete(directory);

        var page = (UserControl)Activator.CreateInstance(assembly.GetType("Aurora.StreamSettingsPage", true));
        var startupSheet = new Border { Background = Brush.Parse("#F4F6FA"), Padding = new Thickness(16), Child = page };
        var window = Preview(570, startupSheet);
        window.Show();
        try
        {
            await Until(() => page.GetVisualDescendants().OfType<RadioButton>().Any(button => Equals(button.Content, "界面")));
            page.GetVisualDescendants().OfType<RadioButton>().Single(button => Equals(button.Content, "界面")).IsChecked = true;
            await Until(() => page.GetVisualDescendants().OfType<ToggleSwitch>().Any(toggle =>
                AutomationProperties.GetName(toggle) == "开机自启动" && toggle.Bounds.Height > 0));
            window.UpdateLayout();
            using var picture = Draw(startupSheet, 1.0);
            picture.Save(Path.Combine(output, "startup.png"));
        }
        finally { window.Close(); }
        Console.WriteLine("Startup defaults on once and preserves opt-out. The preview did not write to Windows startup settings.");
    }

    private static async Task CheckStreamWindow(string output)
    {
        var type = typeof(AuroraWindow).Assembly.GetType("Aurora.StreamSessionWindow", true);
        Func<Window, Task> retry = _ => Task.CompletedTask;
        var window = (Window)Activator.CreateInstance(type, Private, null, new object[] { "远程电脑", retry }, null);
        window.ShowActivated = false;
        window.ShowInTaskbar = false;
        window.Opacity = 0;
        var toolbar = (Grid)type.GetField("toolbar", Private).GetValue(window);
        var popup = (Popup)type.GetField("fullscreenControls", Private).GetValue(window);
        var maximize = (Button)type.GetField("maximize", Private).GetValue(window);
        var disconnect = (Button)type.GetField("disconnect", Private).GetValue(window);
        var slot = (Grid)type.GetField("disconnectSlot", Private).GetValue(window);
        var restore = (MenuItem)type.GetField("fullscreenItem", Private).GetValue(window);
        var frame = (Border)window.Content;
        using var stop = new CancellationTokenSource();

        try
        {
            Call(window, "ShowConnecting", stop);
            foreach (var state in new[] { WindowState.Normal, WindowState.Maximized })
            {
                window.WindowState = state;
                await Until(() => window.WindowState == state && toolbar.Bounds.Width > 400);
                await Task.Delay(100);
                window.UpdateLayout();
                Require(frame.Margin == window.OffScreenMargin, "The frame lost Windows' off-screen margins.");
                foreach (var button in toolbar.GetVisualDescendants().OfType<Button>())
                {
                    var midpoint = button.TranslatePoint(new Point(button.Bounds.Width / 2, button.Bounds.Height / 2), toolbar).Value.Y;
                    Require(Math.Abs(midpoint - toolbar.Bounds.Height / 2) <= 0.51, "Caption control is off center.");
                }

                if (state == WindowState.Maximized)
                    Require(AutomationProperties.GetName(maximize) == "还原窗口", "Maximized windows need the restore caption.");
            }
            double reserve = slot.Bounds.Width;
            Call(window, "ShowFailure", "测试");
            window.UpdateLayout();
            Require(!disconnect.IsVisible && Math.Abs(slot.Bounds.Width - reserve) <= 0.51,
                "Failure needs a single close button and must preserve the toolbar spacing.");
            Call(window, "ShowConnecting", stop);
            Require(disconnect.IsVisible && AutomationProperties.GetName(disconnect) == "取消", "Retry did not restore Cancel.");

            Call(window, "ShowVideo");
            var indicator = (Button)type.GetField("statusButton", Private).GetValue(window);
            var detail = (Flyout)indicator.Flyout;
            var panel = (Control)type.GetField("connectionDetails", Private).GetValue(window);
            var sampleType = typeof(AuroraWindow).Assembly.GetType("Aurora.StreamConnectionSample", true);
            var read = sampleType.GetMethod("Read", BindingFlags.Static | BindingFlags.NonPublic);
            Require(read.Invoke(null, new object[] { "aurora-connection/1,?" }) == null, "An invalid telemetry line was accepted.");
            var sample = read.Invoke(null, new object[] { "aurora-connection/1,1920,1080,2,5994,18600,120,1400,12,0" });
            Call(panel, "Update", sample);
            Require(AutomationProperties.GetName(indicator).Contains("查看连接详情"), "The connected status should expose session details.");
            detail.ShowAt(indicator);
            await Until(() => detail.IsOpen && panel.Bounds.Width > 0);
            Require(TopLevel.GetTopLevel(panel) != window, "Connection details must be above the embedded video.");
            var fields = panel.GetVisualDescendants().OfType<TextBlock>().Where(text => text.IsVisible).Select(text => text.Text).ToArray();
            Require(fields.Contains("1920 × 1080") && fields.Contains("HEVC") && fields.Contains("12 ms"), "Actual telemetry fields are missing.");
            using (var detailsPicture = Draw(panel.GetVisualAncestors().OfType<FlyoutPresenter>().First(), 1.25))
                detailsPicture.Save(Path.Combine(output, "stream-connection-details.png"));
            detail.Hide();
            var noLatency = read.Invoke(null, new object[] { "aurora-connection/1,1920,1080,2,5994,18600,120,1400,0,0" });
            Call(panel, "Update", noLatency);
            Require(!panel.GetVisualDescendants().OfType<TextBlock>().Any(text => text.Text == "网络延迟"), "An unavailable metric should be hidden.");
            Console.WriteLine("Windows connection details parse, placement and unavailable fields verified.");
            Call(window, "ToggleFullscreen");
            await Until(() => popup.IsOpen && popup.Child.Bounds.Height > 0);
            Require(!toolbar.IsVisible && Equals(restore.Header, "退出全屏"), "Fullscreen controls are unavailable.");
            Require(AutomationProperties.GetName((Button)popup.Child) == "控制中心", "The floating button needs an accessible label.");
            var popupRoot = TopLevel.GetTopLevel(popup.Child);
            Require(popupRoot != window, "The floating button must be above the native video surface.");
            var buttonOrigin = popup.Child.PointToScreen(default);
            var screen = window.Screens.ScreenFromWindow(window).Bounds;
            Require(screen.Contains(buttonOrigin), "The floating button is outside the stream monitor.");

            var trigger = (Button)popup.Child;
            trigger.Flyout.ShowAt(trigger);
            await Until(() => ((MenuFlyout)trigger.Flyout).IsOpen);
            Require(restore.IsEffectivelyEnabled, "The fullscreen menu cannot restore the window.");
            trigger.Flyout.Hide();
            restore.RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent));
            await Until(() => window.WindowState == WindowState.Maximized && !popup.IsOpen && toolbar.IsVisible);
            await Task.Delay(100);
            window.UpdateLayout();
            using var picture = Draw(toolbar, 1);
            picture.Save(Path.Combine(output, "stream-toolbar.png"));
            Console.WriteLine("Windows stream caption alignment, retry, native full-screen menu and restore verified.");
        }
        finally { Call(window, "Finish"); }
    }

    private static void Click(Button button) => button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
    private static void Call(object target, string name, params object[] args) => target.GetType().GetMethod(name, Private).Invoke(target, args);

    private static async Task Until(Func<bool> ready)
    {
        for (var i = 0; i < 100; i++)
        {
            if (ready()) return;
            await Task.Delay(20);
        }
        throw new InvalidOperationException("The visual or state transition did not complete.");
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
