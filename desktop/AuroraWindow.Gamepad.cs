using System;
using System.Threading.Tasks;

namespace Aurora;

public sealed partial class AuroraWindow
{
    private bool installingGamepadDriver;
    private bool dismissedGamepadReminder;
    private bool gamepadDriverNeedsRestart;
    private WindowsServiceState gamepadDriverState;
    private string gamepadDriverMessage;

    private void InitializeGamepadDriver()
    {
        if (!OperatingSystem.IsWindows()) return;

        PropertyChanged += (_, args) =>
        {
            if (args.Property != IsVisibleProperty || !IsVisible) return;
            dismissedGamepadReminder = false;
            RefreshGamepadDriver();
        };
        Opened += (_, _) => RefreshGamepadDriver();
        Activated += (_, _) => RefreshGamepadDriver();
        DismissGamepadReminder.Click += (_, _) =>
        {
            dismissedGamepadReminder = true;
            RenderGamepadDriver();
        };
        InstallGamepadReminder.Click += async (_, _) => await InstallGamepadDriver();
        InstallHostGamepad.Click += async (_, _) => await InstallGamepadDriver();
    }

    private void RefreshGamepadDriver()
    {
        if (installingGamepadDriver) return;
        gamepadDriverState = ViGEmBus.ReadState();
        RenderGamepadDriver();
    }

    private void RenderGamepadDriver()
    {
        var missing = gamepadDriverState == WindowsServiceState.Missing;
        var canInstall = missing && !gamepadDriverNeedsRestart;
        GamepadReminder.IsVisible = !dismissedGamepadReminder && (missing || gamepadDriverNeedsRestart);
        HostGamepadDriver.IsVisible = !GamepadReminder.IsVisible;
        GamepadReminderText.Text = gamepadDriverMessage ??
            "本机未安装手柄驱动。安装 ViGEmBus 后，远程手柄可用于这台电脑。";
        HostGamepadText.Text = gamepadDriverMessage ?? (gamepadDriverState switch
        {
            WindowsServiceState.Missing => "未安装 ViGEmBus",
            WindowsServiceState.Unknown => "暂时无法检测手柄驱动",
            _ => "ViGEmBus 已安装"
        });
        InstallGamepadReminder.IsVisible = InstallHostGamepad.IsVisible = canInstall;
        InstallGamepadReminder.IsEnabled = InstallHostGamepad.IsEnabled = !installingGamepadDriver;
        DismissGamepadReminder.IsEnabled = !installingGamepadDriver;
    }

    private async Task InstallGamepadDriver()
    {
        if (installingGamepadDriver || gamepadDriverNeedsRestart || gamepadDriverState != WindowsServiceState.Missing) return;

        installingGamepadDriver = true;
        gamepadDriverMessage = "正在准备 ViGEmBus 安装程序…";
        RenderGamepadDriver();
        try
        {
            var result = await ViGEmBus.InstallAsync(new Progress<string>(message =>
            {
                if (closed || !installingGamepadDriver) return;
                gamepadDriverMessage = message;
                RenderGamepadDriver();
            }));
            gamepadDriverNeedsRestart = result == GamepadInstallResult.RestartRequired;
            gamepadDriverMessage = result switch
            {
                GamepadInstallResult.RestartRequired => "ViGEmBus 已安装，重启 Windows 后可使用远程手柄。",
                GamepadInstallResult.Canceled => "已取消安装 ViGEmBus。",
                _ => null
            };
            if (result == GamepadInstallResult.Installed && !closed)
                UpdateNotice.ShowRefreshSuccess("ViGEmBus 已安装");
        }
        catch (Exception error)
        {
            gamepadDriverMessage = error.Message;
        }
        finally
        {
            installingGamepadDriver = false;
            if (!closed) RefreshGamepadDriver();
        }
    }
}
