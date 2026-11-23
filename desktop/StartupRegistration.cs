using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Microsoft.Win32;

namespace Aurora;

internal enum StartupState { Off, On, OtherLocation, NeedsApproval, Unavailable }

internal static class StartupRegistration
{
    private const string Entry = "org.aurora-remote.desktop";
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ServiceManagement = "/System/Library/Frameworks/ServiceManagement.framework/ServiceManagement";
    private const string ObjectiveC = "/usr/lib/libobjc.A.dylib";
    private static readonly Lazy<nint> AppServiceClass = new(() =>
    {
        NativeLibrary.Load(ServiceManagement);
        return GetClass("SMAppService");
    });

    internal static string InitializationError { get; private set; }

    private static bool MacBundleAvailable => OperatingSystem.IsMacOSVersionAtLeast(13) &&
        File.Exists(Path.Combine(AppContext.BaseDirectory, "..", "Info.plist"));

    internal static void InitializeDefault()
    {
        if (!OperatingSystem.IsWindows() && !MacBundleAvailable) return;
        try
        {
            ApplyDefault(SettingsDirectory(), () => Read() != StartupState.Off, () => Write(true));
        }
        catch (Exception error)
        {
            Debug.WriteLine(error);
            InitializationError = "无法登记开机自启动。";
        }
    }

    internal static StartupState Read()
    {
        if (OperatingSystem.IsWindows()) return ReadWindows();
        if (!MacBundleAvailable) return StartupState.Unavailable;
        return Send(MainService(), GetSelector("status")) switch
        {
            0 => StartupState.Off,
            1 => StartupState.On,
            2 => StartupState.NeedsApproval,
            _ => StartupState.Unavailable,
        };
    }

    internal static void Change(bool enabled)
    {
        ClaimInitialization(SettingsDirectory());
        Write(enabled);
        InitializationError = null;
    }

    private static void ApplyDefault(string directory, Func<bool> hasRegistration, Action register)
    {
        if (ClaimInitialization(directory) && !hasRegistration()) register();
    }

    private static string SettingsDirectory()
    {
        var appData = Environment.GetFolderPath(OperatingSystem.IsMacOS()
            ? Environment.SpecialFolder.ApplicationData : Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(appData)) throw new IOException("无法读取 Aurora 的用户设置目录。");
        return Path.Combine(appData, "Aurora");
    }

    private static bool ClaimInitialization(string directory)
    {
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "startup-initialized");
        if (File.Exists(path)) return false;
        try
        {
            using var marker = File.Open(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read);
            return true;
        }
        catch (IOException) when (File.Exists(path)) { return false; }
    }

    private static void Write(bool enabled)
    {
        if (OperatingSystem.IsWindows()) WriteWindows(enabled);
        else if (MacBundleAvailable)
        {
            var action = enabled ? "registerAndReturnError:" : "unregisterAndReturnError:";
            if (InvokeRegistration(MainService(), GetSelector(action), 0) == 0 &&
                (enabled ? Read() is not (StartupState.On or StartupState.NeedsApproval) : Read() != StartupState.Off))
                throw new InvalidOperationException("macOS 未能更新 Aurora 的登录项。");
        }
    }

    [SupportedOSPlatform("windows")]
    private static StartupState ReadWindows()
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey);
        if (key?.GetValue(Entry) is not string command || command.Length == 0) return StartupState.Off;
        return string.Equals(command, '"' + Environment.ProcessPath + '"', StringComparison.OrdinalIgnoreCase)
            ? StartupState.On : StartupState.OtherLocation;
    }

    [SupportedOSPlatform("windows")]
    private static void WriteWindows(bool enabled)
    {
        if (!enabled)
        {
            using var existing = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
            existing?.DeleteValue(Entry, throwOnMissingValue: false);
            return;
        }

        var executable = Environment.ProcessPath;
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "dotnet", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("请使用 Aurora.exe 登记开机自启动。");
        if (string.IsNullOrWhiteSpace(executable) || !Path.IsPathFullyQualified(executable) || executable.Length + 2 > 260)
            throw new InvalidOperationException("应用路径无法登记为 Windows 启动项。请将 Aurora 放到路径较短的文件夹。");
        using var key = Registry.CurrentUser.CreateSubKey(RunKey);
        key.SetValue(Entry, '"' + executable + '"', RegistryValueKind.String);
    }

    private static nint MainService()
    {
        var service = Send(AppServiceClass.Value, GetSelector("mainAppService"));
        if (service == 0) throw new InvalidOperationException("无法读取 Aurora 的 macOS 登录项。");
        return service;
    }

    [DllImport(ObjectiveC, EntryPoint = "objc_getClass")]
    private static extern nint GetClass(string name);

    [DllImport(ObjectiveC, EntryPoint = "sel_registerName")]
    private static extern nint GetSelector(string name);

    [DllImport(ObjectiveC, EntryPoint = "objc_msgSend")]
    private static extern nint Send(nint target, nint selector);

    [DllImport(ObjectiveC, EntryPoint = "objc_msgSend")]
    private static extern byte InvokeRegistration(nint target, nint selector, nint error);
}
