using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal enum WindowsServiceState { Unknown, Missing, Stopped, Starting, Running }

internal static class WindowsService
{
    private const int ErrorServiceMissing = 1060;
    private const uint ScManagerConnect = 0x0001;
    private const uint ServiceQueryStatus = 0x0004;

    internal static WindowsServiceState Read(string name)
    {
        if (!OperatingSystem.IsWindows()) return WindowsServiceState.Unknown;
        using var manager = OpenSCManager(null, null, ScManagerConnect);
        if (manager.IsInvalid) return WindowsServiceState.Unknown;
        using var service = OpenService(manager, name, ServiceQueryStatus);
        var openError = Marshal.GetLastPInvokeError();
        if (service.IsInvalid)
            return openError == ErrorServiceMissing ? WindowsServiceState.Missing : WindowsServiceState.Unknown;
        if (!QueryServiceStatus(service, out var status)) return WindowsServiceState.Unknown;
        return status.CurrentState switch
        {
            1 => WindowsServiceState.Stopped,
            2 or 3 or 5 => WindowsServiceState.Starting,
            4 => WindowsServiceState.Running,
            _ => WindowsServiceState.Unknown
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ServiceStatus
    {
        public uint ServiceType, CurrentState, ControlsAccepted, Win32ExitCode, ServiceSpecificExitCode, CheckPoint, WaitHint;
    }

    private sealed class ServiceHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private ServiceHandle() : base(ownsHandle: true) { }
        protected override bool ReleaseHandle() => CloseServiceHandle(handle);
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ServiceHandle OpenSCManager(string machine, string database, uint access);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ServiceHandle OpenService(ServiceHandle manager, string service, uint access);

    [DllImport("advapi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryServiceStatus(ServiceHandle service, out ServiceStatus status);

    [DllImport("advapi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseServiceHandle(IntPtr service);
}
