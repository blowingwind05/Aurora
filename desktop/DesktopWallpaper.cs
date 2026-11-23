using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using SkiaSharp;

namespace Aurora;

internal sealed class DesktopWallpaper
{
    internal const int MaxPreviewBytes = 512 * 1024;
    private const int MaxEdge = 960;
    private static readonly string CacheDirectory = Path.Combine(Environment.GetFolderPath(
        OperatingSystem.IsMacOS() ? Environment.SpecialFolder.ApplicationData :
        Environment.SpecialFolder.LocalApplicationData), "Aurora", "Wallpapers");
    private DateTime nextCheck;
    private bool generating;
    private (string Path, DateTime Modified, long Length) lastSource;

    // Call on the UI thread so AppKit reads the wallpaper of the active user session.
    public async Task RefreshHost()
    {
        if (generating || !BundledSunshine.IsRunning || DateTime.UtcNow < nextCheck) return;
        nextCheck = DateTime.UtcNow.AddSeconds(45);
        generating = true;
        try
        {
            var path = OperatingSystem.IsMacOS() ? MacWallpaper() : WindowsWallpaper();
            if (string.IsNullOrEmpty(path)) return;
            await Task.Run(() =>
            {
                var file = new FileInfo(path);
                if (!file.Exists || file.Length is <= 0 or > 80 * 1024 * 1024) return;
                var source = (file.FullName, file.LastWriteTimeUtc, file.Length);
                if (source == lastSource && File.Exists(BundledSunshine.WallpaperPath)) return;
                var preview = Encode(file.FullName);
                if (preview != null)
                {
                    Save(BundledSunshine.WallpaperPath, preview);
                    lastSource = source;
                }
            });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ExternalException
            or ArgumentException or System.ComponentModel.Win32Exception)
        {
            Debug.WriteLine(error);
        }
        finally { generating = false; }
    }

    private static byte[] Encode(string path, bool convertOnMac = true)
    {
        using var input = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var codec = SKCodec.Create(input);
        if (codec == null)
            return convertOnMac && OperatingSystem.IsMacOS() ? ConvertMacImage(path) : null;
        if (codec.Info.Width <= 0 || codec.Info.Height <= 0 ||
            (long)codec.Info.Width * codec.Info.Height > 100_000_000) return null;
        using var image = SKBitmap.Decode(codec);
        if (image == null) return null;
        var scale = Math.Min(1.0, (double)MaxEdge / Math.Max(image.Width, image.Height));
        using var preview = image.Resize(new SKImageInfo(Math.Max(1, (int)(image.Width * scale)),
            Math.Max(1, (int)(image.Height * scale))), SKFilterQuality.Medium);
        using var jpeg = preview?.Encode(SKEncodedImageFormat.Jpeg, 78);
        return jpeg is { Size: > 0 and <= MaxPreviewBytes } ? jpeg.ToArray() : null;
    }

    private static byte[] ConvertMacImage(string path)
    {
        // ImageIO via the built-in sips tool handles still HEIC wallpapers.
        var temp = Path.Combine(Path.GetDirectoryName(BundledSunshine.WallpaperPath), Guid.NewGuid() + ".jpg");
        try
        {
            var options = new ProcessStartInfo("/usr/bin/sips") { UseShellExecute = false,
                RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var arg in new[] { "-s", "format", "jpeg", "-Z", MaxEdge.ToString(), path, "--out", temp })
                options.ArgumentList.Add(arg);
            using var process = Process.Start(options);
            var stdout = process.StandardOutput.BaseStream.CopyToAsync(Stream.Null);
            var stderr = process.StandardError.BaseStream.CopyToAsync(Stream.Null);
            if (!process.WaitForExit(4000))
            {
                Upstream.StopProcess(process);
                return null;
            }
            Task.WhenAll(stdout, stderr).GetAwaiter().GetResult();
            return process.ExitCode == 0 && File.Exists(temp) ? Encode(temp, convertOnMac: false) : null;
        }
        finally { File.Delete(temp); }
    }

    internal static string CachedPath(Device device, NetworkSnapshot network)
    {
        var identity = device.TailnetVisible ? $"tailnet:{network?.Owner}:{network?.LocalId}:{device.Id}" : device.Id;
        return Path.Combine(CacheDirectory, Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity))) + ".jpg");
    }

    internal static async Task<bool> RefreshPreview(string path, Device device, CancellationToken token)
    {
        try
        {
            var current = new FileInfo(path);
            var revision = current.Exists && current.Length is > 0 and <= MaxPreviewBytes
                ? Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(path, token))) : "";
            foreach (var route in new[] { device.Lan, device.Tailnet })
            {
                if (route == null) continue;
                token.ThrowIfCancellationRequested();
                var (reachable, preview) = await Upstream.ReadWallpaper(route, revision, token);
                if (!reachable) continue;
                if (preview == null) return false;
                using var codec = SKCodec.Create(new MemoryStream(preview, writable: false));
                if (codec == null || codec.EncodedFormat != SKEncodedImageFormat.Jpeg ||
                    codec.Info.Width is <= 0 or > MaxEdge || codec.Info.Height is <= 0 or > MaxEdge) return false;
                token.ThrowIfCancellationRequested();
                Save(path, preview);
                return true;
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or OperationCanceledException
            or InvalidOperationException or System.ComponentModel.Win32Exception or FormatException)
        {
            Debug.WriteLine(error);
        }
        return false;
    }

    private static void Save(string path, byte[] preview)
    {
        var directory = Path.GetDirectoryName(path);
        if (OperatingSystem.IsMacOS())
            Directory.CreateDirectory(directory, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        else
            Directory.CreateDirectory(directory);
        var temp = path + "." + Guid.NewGuid().ToString("N");
        try
        {
            File.WriteAllBytes(temp, preview);
            File.Move(temp, path, overwrite: true);
        }
        finally { File.Delete(temp); }
    }

    private static string WindowsWallpaper()
    {
        if (!OperatingSystem.IsWindows()) return null;
        var type = Type.GetTypeFromCLSID(new Guid("C2CF3110-460E-4FC1-B9D0-8A1C0C9CC4BD"));
        object instance = null;
        try
        {
            instance = Activator.CreateInstance(type);
            if (instance is IDesktopWallpaper wallpaper && wallpaper.GetMonitorDevicePathCount(out var count) == 0)
            {
                for (uint monitor = 0; monitor < count; monitor++)
                    if (wallpaper.GetMonitorDevicePathAt(monitor, out var id) == 0 &&
                        wallpaper.GetMonitorRECT(id, out var rect) == 0 &&
                        rect.Left == 0 && rect.Top == 0 && wallpaper.GetWallpaper(id, out var path) == 0 &&
                        !string.IsNullOrEmpty(path)) return path;
            }
        }
        catch (Exception error) when (error is COMException or UnauthorizedAccessException) { }
        finally { if (instance != null) Marshal.ReleaseComObject(instance); }
        var fallback = new StringBuilder(32768);
        return SystemParametersInfo(0x0073, (uint)fallback.Capacity, fallback, 0) ? fallback.ToString() : null;
    }

    private static string MacWallpaper()
    {
        var screens = Send(objc_getClass("NSScreen"), sel_registerName("screens"));
        if (screens == IntPtr.Zero || Send(screens, sel_registerName("count")) == IntPtr.Zero) return null;
        var screen = Send(screens, sel_registerName("firstObject"));
        var workspace = Send(objc_getClass("NSWorkspace"), sel_registerName("sharedWorkspace"));
        var url = SendWithArgument(workspace, sel_registerName("desktopImageURLForScreen:"), screen);
        if (url == IntPtr.Zero || !SendBool(url, sel_registerName("isFileURL"))) return null;
        return Marshal.PtrToStringUTF8(Send(Send(url, sel_registerName("path")), sel_registerName("UTF8String")));
    }

    [ComImport, Guid("B92B56A9-8B55-4E14-9A89-0199BBB6F93B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDesktopWallpaper
    {
        [PreserveSig] int SetWallpaper(IntPtr monitor, IntPtr path);
        [PreserveSig] int GetWallpaper([MarshalAs(UnmanagedType.LPWStr)] string monitor,
            [MarshalAs(UnmanagedType.LPWStr)] out string path);
        [PreserveSig] int GetMonitorDevicePathAt(uint index, [MarshalAs(UnmanagedType.LPWStr)] out string monitor);
        [PreserveSig] int GetMonitorDevicePathCount(out uint count);
        [PreserveSig] int GetMonitorRECT([MarshalAs(UnmanagedType.LPWStr)] string monitor, out Rect rect);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SystemParametersInfoW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SystemParametersInfo(uint action, uint size, StringBuilder path, uint flags);

    private const string ObjC = "/usr/lib/libobjc.A.dylib";

    [DllImport(ObjC)] private static extern IntPtr objc_getClass(string name);
    [DllImport(ObjC)] private static extern IntPtr sel_registerName(string name);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")] private static extern IntPtr Send(IntPtr target, IntPtr selector);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    private static extern IntPtr SendWithArgument(IntPtr target, IntPtr selector, IntPtr argument);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    [return: MarshalAs(UnmanagedType.I1)]
    private static extern bool SendBool(IntPtr target, IntPtr selector);
}
