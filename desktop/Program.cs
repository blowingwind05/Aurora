using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Markup.Xaml;

namespace Aurora;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args) =>
        AppBuilder.Configure<AuroraApp>().UsePlatformDetect().StartWithClassicDesktopLifetime(args);
}

public sealed class AuroraApp : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var window = new AuroraWindow();
            desktop.MainWindow = window;
            desktop.Exit += (_, _) => EmbeddedTailnet.Stop();
            _ = EmbeddedTailnet.ResumeIfConfigured();
            void ShowSettings()
            {
                if (window.PlatformImpl == null) return;
                TrayResidency.ShowMainWindow(window);
                window.OpenApplicationSettings();
            }

            TrayResidency.Initialize(this, window, window.Close, ShowSettings);
            StartupRegistration.InitializeDefault();
            if (OperatingSystem.IsMacOS())
            {
                var settings = new NativeMenuItem("设置") { Gesture = new KeyGesture(Key.OemComma, KeyModifiers.Meta) };
                settings.Click += (_, _) => ShowSettings();
                NativeMenu.SetMenu(this, new NativeMenu { Items = { settings } });
            }
        }
        base.OnFrameworkInitializationCompleted();
    }
}
