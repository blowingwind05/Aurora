// SPDX-License-Identifier: GPL-3.0-only
using System;
using System.IO;

namespace Aurora;

internal sealed record StartupPreference(bool Enabled, bool Available, string Feedback);

internal static class GeneralSettingsPreferences
{
    internal const string CloseChangeFailed = "无法更改关闭窗口的设置。";
    internal const string StartupChangeFailed = "无法更改开机自启动。";
    internal const string StartupReadFailed = "无法读取开机自启动状态。";
    internal const string SleepChangeFailed = "无法保存自动休眠设置。";

    internal static StartupPreference DescribeStartup(StartupState state)
    {
        var feedback = state switch
        {
            StartupState.NeedsApproval => "请在系统设置的登录项中允许 Aurora。",
            StartupState.OtherLocation => "启动项指向另一个位置的 Aurora。关闭后重新开启可更新路径。",
            StartupState.Unavailable => "此系统或应用包无法登记开机自启动。",
            _ => StartupRegistration.InitializationError ?? "",
        };
        return new StartupPreference(
            state is StartupState.On or StartupState.OtherLocation or StartupState.NeedsApproval,
            state != StartupState.Unavailable, feedback);
    }

    internal static StartupPreference ReadStartup()
    {
        try { return DescribeStartup(StartupRegistration.Read()); }
        catch (Exception) { return new StartupPreference(false, false, StartupReadFailed); }
    }

    internal static bool TrySetStartup(bool enabled)
    {
        try
        {
            StartupRegistration.Change(enabled);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    internal static bool TrySetSleepMode(HostSleepMode mode)
    {
        try
        {
            HostSleepPrevention.SetMode(mode);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }
}
