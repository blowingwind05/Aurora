using System;
using System.IO;
using Microsoft.Win32;

namespace Aurora;

internal static class BundledTailscale
{
    private const string SystemService = "Tailscale";
    internal const string SystemPipe = @"ProtectedPrefix\Administrators\Tailscale\tailscaled";

    internal static string FindWindowsCli(TailscaleMode mode)
    {
        if (mode != TailscaleMode.System) return null;
        var daemon = RegisteredDaemonPath();
        if (daemon == null) return null;
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "AuroraTailnet");
        var directory = Path.GetDirectoryName(daemon);
        // Ignore an old Aurora preview registered under the official service name.
        if (string.Equals(Path.GetDirectoryName(directory), root, StringComparison.OrdinalIgnoreCase)) return null;
        var cli = Path.Combine(directory, "tailscale.exe");
        return File.Exists(cli) ? cli : null;
    }

    private static string RegisteredDaemonPath()
    {
        if (!OperatingSystem.IsWindows()) return null;
        try
        {
            using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\" + SystemService);
            if (service?.GetValue("ImagePath") is not string image) return null;
            image = image.Trim();
            const string suffix = @"\tailscaled.exe";
            var quoted = image.StartsWith('"');
            var index = quoted ? image.IndexOf('"', 1) : image.IndexOf(suffix, StringComparison.OrdinalIgnoreCase);
            if (index < 0) return null;
            var end = quoted ? index : index + suffix.Length;
            if (end + (quoted ? 1 : 0) < image.Length && !char.IsWhiteSpace(image[end + (quoted ? 1 : 0)]))
                return null;
            var path = image[(quoted ? 1 : 0)..end];
            return Path.IsPathFullyQualified(path) &&
                string.Equals(Path.GetFileName(path), "tailscaled.exe", StringComparison.OrdinalIgnoreCase) ? path : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return null;
        }
    }

    internal static WindowsServiceState ReadWindowsService(TailscaleMode mode)
    {
        if (!OperatingSystem.IsWindows()) return WindowsServiceState.Unknown;
        if (FindWindowsCli(mode) == null) return WindowsServiceState.Missing;
        return WindowsService.Read(SystemService);
    }
}
