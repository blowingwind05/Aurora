using System;
using System.Diagnostics;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace Aurora;

internal sealed class TrayResidency
{
    private static TrayResidency current;

    private readonly Application app;
    private readonly Window window;
    private readonly Action requestExit;
    private readonly Action openSettings;
    private readonly string exitOnClosePath;
    private TrayIcon tray;
    private bool requestingExit;

    private TrayResidency(Application app, Window window, Action requestExit, Action openSettings, string dataDirectory)
    {
        this.app = app;
        this.window = window;
        this.requestExit = requestExit;
        this.openSettings = openSettings;
        exitOnClosePath = Path.Combine(dataDirectory, "Aurora", "exit-on-close");
        if (!File.Exists(exitOnClosePath)) ShowTray();
        window.Closed += (_, _) =>
        {
            RemoveTray();
            current = null;
        };
    }

    internal static bool IsAvailable => current != null;
    internal static bool CloseToTray => current?.tray != null;

    internal static void Initialize(Application app, Window window, Action requestExit, Action openSettings = null)
    {
        if (current != null || !(OperatingSystem.IsWindows() || OperatingSystem.IsMacOS())) return;

        var dataDirectory = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(dataDirectory)) return;

        current = new TrayResidency(app, window, requestExit, openSettings, dataDirectory);
    }

    internal static bool TrySetCloseToTray(bool enabled) => current?.TrySetEnabled(enabled) ?? !enabled;

    internal static bool TryHideOnClose(Window window, WindowClosingEventArgs args)
    {
        var residency = current;
        if (residency?.tray == null || residency.window != window || residency.requestingExit ||
            args.CloseReason != WindowCloseReason.WindowClosing) return false;

        args.Cancel = true;
        window.Hide();
        return true;
    }

    internal static void ShowMainWindow(Window window)
    {
        if (window.PlatformImpl == null) return;
        if (window.WindowState == WindowState.Minimized) window.WindowState = WindowState.Normal;
        if (!window.IsVisible) window.Show();
        window.Activate();
    }

    private bool TrySetEnabled(bool enabled)
    {
        if (enabled == (tray != null)) return true;
        if (enabled && !ShowTray()) return false;

        try
        {
            if (enabled) File.Delete(exitOnClosePath);
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(exitOnClosePath));
                using var preference = File.Open(exitOnClosePath, FileMode.OpenOrCreate, FileAccess.Write);
                RemoveTray();
            }
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Debug.WriteLine(error);
            if (enabled) RemoveTray();
            return false;
        }
    }

    private bool ShowTray()
    {
        var mac = OperatingSystem.IsMacOS();
        var asset = mac ? "AuroraMenuBarTemplate@2x.png" : "Aurora.ico";
        using var glyph = AssetLoader.Open(new Uri("avares://Aurora/Assets/" + asset));

        var icon = new TrayIcon();
        if (icon.NativeMenuExporter == null)
        {
            icon.Dispose();
            return false;
        }

        var show = new NativeMenuItem("打开 Aurora");
        show.Click += (_, _) => ShowMainWindow(window);
        var exit = new NativeMenuItem("退出 Aurora");
        exit.Click += (_, _) => Exit();
        var menu = new NativeMenu { Items = { show } };
        if (openSettings != null)
        {
            var settings = new NativeMenuItem("设置");
            if (!mac) settings.Icon = LoadMenuIcon("settings");
            settings.Click += (_, _) => openSettings();
            menu.Items.Add(settings);
        }
        if (!mac)
        {
            show.Icon = LoadMenuIcon("open");
            exit.Icon = LoadMenuIcon("dismiss");
        }
        menu.Items.Add(new NativeMenuItemSeparator());
        menu.Items.Add(exit);

        icon.Menu = menu;
        icon.ToolTipText = "Aurora 远程桌面";
        if (mac) MacOSProperties.SetIsTemplateIcon(icon, true);
        icon.Icon = new WindowIcon(glyph);
        icon.IsVisible = true;
        if (!mac) icon.Clicked += (_, _) => ShowMainWindow(window);
        if (TrayIcon.GetIcons(app) is { } icons) icons.Add(icon);
        else TrayIcon.SetIcons(app, new TrayIcons { icon });
        tray = icon;
        return true;
    }

    private static Bitmap LoadMenuIcon(string name)
    {
        using var stream = AssetLoader.Open(new Uri($"avares://Aurora/Assets/icons/ic_fluent_{name}_16_regular.png"));
        return new Bitmap(stream);
    }

    private void RemoveTray()
    {
        if (tray == null) return;
        TrayIcon.GetIcons(app)?.Remove(tray);
        tray = null;
    }

    private void Exit()
    {
        if (requestingExit) return;
        requestingExit = true;
        // Close() raises Closing synchronously. The window owns any subsequent shutdown work.
        try { requestExit(); }
        finally { requestingExit = false; }
    }
}
