using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal enum HostSleepMode { System, OnAc, Always }

internal static class HostSleepPrevention
{
    private const string Reason = "Aurora 正在允许本机被控";
    private static readonly object Gate = new();
    private static readonly string SettingsPath = GetSettingsPath();
    private static HostSleepMode mode = ReadMode();
    private static bool hostActive;
    private static bool exiting;
    private static Timer powerSourceTimer;
    private static IDisposable assertion;
    private static WindowsResumeRegistration resumeRegistration;

    static HostSleepPrevention()
    {
        AppDomain.CurrentDomain.ProcessExit += (_, _) =>
        {
            lock (Gate)
            {
                exiting = true;
                hostActive = false;
                Update();
            }
        };
    }

    internal static HostSleepMode Mode
    {
        get { lock (Gate) return mode; }
    }

    internal static void SetMode(HostSleepMode value)
    {
        if (value is not (HostSleepMode.System or HostSleepMode.OnAc or HostSleepMode.Always))
            throw new ArgumentOutOfRangeException(nameof(value));

        lock (Gate)
        {
            SaveMode(value);
            mode = value;
            Update();
        }
    }

    internal static void SetHostActive(bool active)
    {
        lock (Gate)
        {
            hostActive = active && !exiting;
            Update();
        }
    }

    // The mode, timers and native handles are all changed under Gate.
    private static void Update()
    {
        bool enabled = hostActive && mode != HostSleepMode.System;
        if (enabled && mode == HostSleepMode.OnAc)
        {
            powerSourceTimer ??= new Timer(_ =>
            {
                lock (Gate) RefreshAssertion();
            }, null, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(2));
        }
        else
        {
            powerSourceTimer?.Dispose();
            powerSourceTimer = null;
        }

        if (enabled && OperatingSystem.IsWindows())
            resumeRegistration ??= WindowsResumeRegistration.Create();
        else
        {
            resumeRegistration?.Dispose();
            resumeRegistration = null;
        }

        RefreshAssertion();
    }

    private static void RefreshAssertion()
    {
        if (hostActive && (mode == HostSleepMode.Always || (mode == HostSleepMode.OnAc && IsOnAc())))
        {
            if (assertion == null)
            {
                if (OperatingSystem.IsWindows()) assertion = WindowsAssertion.Create();
                else if (OperatingSystem.IsMacOS()) assertion = MacAssertion.Create();
            }
        }
        else ReleaseAssertion();
    }

    private static void ReleaseAssertion()
    {
        assertion?.Dispose();
        assertion = null;
    }

    private static bool IsOnAc()
    {
        if (OperatingSystem.IsWindows())
            return GetSystemPowerStatus(out var status) && status.AcLineStatus == 1;
        if (!OperatingSystem.IsMacOS()) return false;

        using var info = IOPSCopyPowerSourcesInfo();
        if (info.IsInvalid) return false;

        // This reports the supplying source, including AC on desktops without a battery.
        nint source = IOPSGetProvidingPowerSourceType(info);
        if (source == 0) return false;
        using var ac = CreateCFString("AC Power");
        return !ac.IsInvalid && CFEqual(source, ac);
    }

    private static string GetSettingsPath()
    {
        var folder = Environment.GetFolderPath(OperatingSystem.IsMacOS()
            ? Environment.SpecialFolder.ApplicationData : Environment.SpecialFolder.LocalApplicationData,
            Environment.SpecialFolderOption.DoNotVerify);
        return string.IsNullOrEmpty(folder) ? null : Path.Combine(folder, "Aurora", "host-sleep-mode");
    }

    private static HostSleepMode ReadMode()
    {
        if (SettingsPath == null) return HostSleepMode.System;
        try
        {
            return File.ReadAllText(SettingsPath).Trim() switch
            {
                nameof(HostSleepMode.OnAc) => HostSleepMode.OnAc,
                nameof(HostSleepMode.Always) => HostSleepMode.Always,
                _ => HostSleepMode.System,
            };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return HostSleepMode.System;
        }
    }

    private static void SaveMode(HostSleepMode value)
    {
        if (SettingsPath == null) throw new IOException("Aurora 的用户设置目录不可用。");
        string temporary = SettingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath));
            File.WriteAllText(temporary, value.ToString());
            File.Move(temporary, SettingsPath, overwrite: true);
        }
        catch (UnauthorizedAccessException error)
        {
            throw new IOException("无法保存 Aurora 的睡眠设置。", error);
        }
        finally
        {
            try { File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                Debug.WriteLine(error);
            }
        }
    }

    private const string Kernel32 = "kernel32.dll";
    private const string PowrProf = "powrprof.dll";
    private const string IOKit = "/System/Library/Frameworks/IOKit.framework/IOKit";
    private const string CoreFoundation = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";

    [StructLayout(LayoutKind.Sequential)]
    private struct SystemPowerStatus
    {
        public byte AcLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag;
        public uint BatteryLifeTime, BatteryFullLifeTime;
    }

    [DllImport(Kernel32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetSystemPowerStatus(out SystemPowerStatus status);

    [StructLayout(LayoutKind.Sequential)]
    private struct ReasonContext
    {
        public uint Version, Flags;
        public ReasonData Reason;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct ReasonData
    {
        [FieldOffset(0)] public nint SimpleReasonString;
        [FieldOffset(0)] public DetailedReason Detailed;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DetailedReason
    {
        public nint LocalizedReasonModule;
        public uint LocalizedReasonId, ReasonStringCount;
        public nint ReasonStrings;
    }

    private sealed class WindowsAssertion : SafeHandleZeroOrMinusOneIsInvalid
    {
        private const int SystemRequired = 1;
        private bool requested;

        private WindowsAssertion() : base(true) { }

        internal static WindowsAssertion Create()
        {
            nint text = Marshal.StringToHGlobalUni(Reason);
            WindowsAssertion request;
            try
            {
                var context = new ReasonContext
                {
                    Version = 0,
                    Flags = 1, // POWER_REQUEST_CONTEXT_SIMPLE_STRING
                    Reason = new ReasonData { SimpleReasonString = text },
                };
                request = PowerCreateRequest(ref context);
            }
            finally { Marshal.FreeHGlobal(text); }

            if (!request.IsInvalid && PowerSetRequest(request, SystemRequired))
            {
                request.requested = true;
                return request;
            }

            Debug.WriteLine($"Aurora could not request Windows idle sleep prevention: {Marshal.GetLastWin32Error()}");
            request.Dispose();
            return null;
        }

        protected override bool ReleaseHandle()
        {
            if (requested) PowerClearRequest(handle, SystemRequired);
            return CloseHandle(handle);
        }

        [DllImport(Kernel32, SetLastError = true)]
        private static extern WindowsAssertion PowerCreateRequest(ref ReasonContext context);

        [DllImport(Kernel32, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PowerSetRequest(WindowsAssertion request, int type);

        [DllImport(Kernel32)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PowerClearRequest(nint request, int type);

        [DllImport(Kernel32)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(nint handle);
    }

    private sealed class WindowsResumeRegistration : SafeHandleZeroOrMinusOneIsInvalid
    {
        private const uint NotifyCallback = 2; // DEVICE_NOTIFY_CALLBACK
        private const uint ResumeAutomatic = 0x12; // PBT_APMRESUMEAUTOMATIC
        private static readonly PowerNotificationCallback Callback = OnPowerNotification;

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate uint PowerNotificationCallback(nint context, uint type, nint setting);

        [StructLayout(LayoutKind.Sequential)]
        private struct Subscription
        {
            public nint Callback, Context;
        }

        private WindowsResumeRegistration(nint token) : base(true) => SetHandle(token);

        internal static WindowsResumeRegistration Create()
        {
            var subscription = new Subscription { Callback = Marshal.GetFunctionPointerForDelegate(Callback) };
            uint error = PowerRegisterSuspendResumeNotification(NotifyCallback, ref subscription, out nint token);
            if (error == 0 && token != 0) return new WindowsResumeRegistration(token);
            Debug.WriteLine($"Aurora could not subscribe to Windows power resume: {error}");
            return null;
        }

        private static uint OnPowerNotification(nint context, uint type, nint setting)
        {
            if (type == ResumeAutomatic)
                ThreadPool.QueueUserWorkItem(_ =>
                {
                    lock (Gate)
                    {
                        // Windows may retire a request when the user explicitly sleeps the PC.
                        ReleaseAssertion();
                        RefreshAssertion();
                    }
                });
            return 0;
        }

        protected override bool ReleaseHandle() => PowerUnregisterSuspendResumeNotification(handle) == 0;

        [DllImport(PowrProf)]
        private static extern uint PowerRegisterSuspendResumeNotification(uint flags, ref Subscription recipient, out nint token);

        [DllImport(PowrProf)]
        private static extern uint PowerUnregisterSuspendResumeNotification(nint token);
    }

    private sealed class MacAssertion : SafeHandleZeroOrMinusOneIsInvalid
    {
        private MacAssertion(uint identifier) : base(true) => SetHandle((nint)identifier);

        internal static MacAssertion Create()
        {
            using var type = CreateCFString("PreventUserIdleSystemSleep");
            using var name = CreateCFString(Reason);
            if (type.IsInvalid || name.IsInvalid) return null;
            const uint assertionLevelOn = 255;
            int result = IOPMAssertionCreateWithName(type, assertionLevelOn, name, out uint identifier);
            if (result == 0 && identifier != 0) return new MacAssertion(identifier);
            Debug.WriteLine($"Aurora could not request macOS idle sleep prevention: 0x{result:X8}");
            return null;
        }

        protected override bool ReleaseHandle() => IOPMAssertionRelease((uint)handle) == 0;

        [DllImport(IOKit)]
        private static extern int IOPMAssertionCreateWithName(CfReference type, uint level, CfReference name,
            out uint identifier);

        [DllImport(IOKit)]
        private static extern int IOPMAssertionRelease(uint identifier);
    }

    private sealed class CfReference : SafeHandleZeroOrMinusOneIsInvalid
    {
        private CfReference() : base(true) { }

        protected override bool ReleaseHandle()
        {
            CFRelease(handle);
            return true;
        }
    }

    private static CfReference CreateCFString(string text) => CFStringCreateWithCString(0, text, 0x08000100);

    [DllImport(IOKit)]
    private static extern CfReference IOPSCopyPowerSourcesInfo();

    [DllImport(IOKit)]
    private static extern nint IOPSGetProvidingPowerSourceType(CfReference info);

    [DllImport(CoreFoundation)]
    private static extern CfReference CFStringCreateWithCString(nint allocator,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text, uint encoding);

    [DllImport(CoreFoundation)]
    [return: MarshalAs(UnmanagedType.U1)]
    private static extern bool CFEqual(nint first, CfReference second);

    [DllImport(CoreFoundation)]
    private static extern void CFRelease(nint reference);
}
