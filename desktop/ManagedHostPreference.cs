using System;
using System.IO;

namespace Aurora;

internal sealed class ManagedHostPreference(string path)
{
    internal static ManagedHostPreference ForCurrentUser()
    {
        var directory = Environment.GetFolderPath(OperatingSystem.IsMacOS()
            ? Environment.SpecialFolder.ApplicationData : Environment.SpecialFolder.LocalApplicationData,
            Environment.SpecialFolderOption.DoNotVerify);
        return new(string.IsNullOrWhiteSpace(directory) ? null : Path.Combine(directory, "Aurora", "allow-managed-host"));
    }

    internal bool Enabled => File.Exists(path);

    // Only explicit changes to "允许本机被控" change this marker.
    internal void SetEnabled(bool enabled)
    {
        if (string.IsNullOrEmpty(path)) throw new IOException("找不到 Aurora 的用户设置目录。");
        var directory = Path.GetDirectoryName(path);
        if (!enabled)
        {
            try { File.Delete(path); }
            catch (DirectoryNotFoundException) { }
            return;
        }
        Directory.CreateDirectory(directory);
        using var marker = File.Open(path, FileMode.OpenOrCreate, FileAccess.Write, FileShare.Read);
    }
}
