using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal static class BundledSunshine
{
    private static readonly string MacHostContents = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory,
        "..", "Helpers", "AuroraHost.app", "Contents"));
    private static readonly string Payload = OperatingSystem.IsMacOS()
        ? Path.Combine(MacHostContents, "Resources")
        : Path.Combine(AppContext.BaseDirectory, "Components", "Sunshine");
    private static readonly string Executable = OperatingSystem.IsMacOS()
        ? Path.Combine(MacHostContents, "MacOS", "AuroraHost")
        : Path.Combine(Payload, "AuroraHost.exe");
    private static readonly string Root = Path.Combine(
        Environment.GetFolderPath(OperatingSystem.IsMacOS() ? Environment.SpecialFolder.ApplicationData
            : Environment.SpecialFolder.LocalApplicationData), "Aurora", "Sunshine", "Bundled");
    private static readonly string Profile = Path.Combine(Root, "config");
    private static readonly string LogPath = Path.Combine(Profile, "sunshine.log");
    private static readonly SemaphoreSlim Lifecycle = new(1);
    private static readonly object ProcessLock = new();
    // Aurora uses a separate port family so an existing Sunshine service can keep running.
    internal const int BasePort = 48989;
    private static readonly int[] TcpPorts = { BasePort - 5, BasePort, BasePort + 1, BasePort + 21 };
    private static readonly int[] UdpPorts = { BasePort + 9, BasePort + 10, BasePort + 11, BasePort + 13 };
    private const string CertificateFile = "credentials/cacert.pem";
    internal const string AdminUsername = "aurora-local";
    // Matches the managed companion's restart exit code.
    private const int RestartRequested = 42;
    private static Host host;
    private static string adminPassword;

    private sealed record Host(Process Process, SafeFileHandle Job, EventWaitHandle StopSignal, FileStream ProfileLock) : IDisposable
    {
        public void RequestStop()
        {
            if (StopSignal != null) StopSignal.Set();
            else Process.StandardInput.Dispose();
        }

        public void Dispose()
        {
            Job?.Dispose();
            Process.Dispose();
            StopSignal?.Dispose();
            ProfileLock.Dispose();
        }
    }

    internal static Uri AdminEndpoint { get; } = new($"https://127.0.0.1:{BasePort + 1}/");
    internal static string ConfigPath => Path.Combine(Profile, "sunshine.conf");
    internal static string WallpaperPath => Path.Combine(Profile, "aurora-wallpaper.jpg");
    internal static string CertificatePath => Path.GetFullPath(Path.Combine(Profile, CertificateFile));
    internal static IReadOnlyDictionary<string, string> ManagedOptions { get; } =
        new ReadOnlyDictionary<string, string>(new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["port"] = BasePort.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ["origin_web_ui_allowed"] = "pc",
            ["upnp"] = "disabled",
            ["system_tray"] = "disabled",
            ["cert"] = CertificateFile,
            ["pkey"] = "credentials/cakey.pem",
            ["file_state"] = "sunshine_state.json",
            ["credentials_file"] = "account.json",
            ["file_apps"] = "apps.json",
            ["log_path"] = "sunshine.log"
        });
    internal static bool IsAvailable => (OperatingSystem.IsWindows() || OperatingSystem.IsMacOSVersionAtLeast(14, 2)) &&
        File.Exists(Executable) && (!OperatingSystem.IsWindows() || File.Exists(Path.Combine(Payload, "zlib1.dll"))) &&
        File.Exists(Path.Combine(Payload, "assets", "apps.json")) &&
        File.Exists(Path.Combine(Payload, "assets", "web", "index.html"));

    internal static bool IsRunning
    {
        get { lock (ProcessLock) return host?.Process.HasExited == false; }
    }

    internal static string ReadAdminPassword()
    {
        lock (ProcessLock)
        {
            if (host?.Process.HasExited == false && adminPassword != null) return adminPassword;
        }
        throw new InvalidOperationException("内置 Sunshine 尚未运行。");
    }

    internal static bool IsReady() => IsRunning && ProbeAdmin().GetAwaiter().GetResult() && IsRunning;

    internal static Task StartAsync() => RunHost(restart: false);
    internal static Task RestartAsync() => RunHost(restart: true);

    private static async Task RunHost(bool restart)
    {
        if (!OperatingSystem.IsWindows() && !OperatingSystem.IsMacOS()) throw new PlatformNotSupportedException();
        await Lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            if (restart && !IsRunning)
                throw new InvalidOperationException("Aurora 内置的 Sunshine 已退出，请重新启用。");
            if (restart || !IsRunning)
                await StartFresh().ConfigureAwait(false);
            else
                await WaitForReady().ConfigureAwait(false);
        }
        finally { Lifecycle.Release(); }
    }

    internal static async Task StopAsync()
    {
        await Lifecycle.WaitAsync().ConfigureAwait(false);
        try { await StopCore().ConfigureAwait(false); }
        finally { Lifecycle.Release(); }
    }

    private static async Task StartFresh()
    {
        await StopCore().ConfigureAwait(false);
        try
        {
            await Task.Run(StartCore).ConfigureAwait(false);
            await WaitForReady().ConfigureAwait(false);
            using var admin = SunshineAdmin.OpenBundled();
            if (await admin.CheckAccess().ConfigureAwait(false) != SunshineAccess.SignedIn)
                throw new InvalidOperationException("内置被控组件未完成 Aurora 托管登录，请更新 Aurora 后重试。");
            HostSleepPrevention.SetHostActive(true);
        }
        catch
        {
            await StopCore().ConfigureAwait(false);
            throw;
        }
    }

    private static void StartCore()
    {
        if (!IsAvailable) throw new InvalidOperationException("此版本缺少完整的 Sunshine 组件。");
        if (OperatingSystem.IsWindows())
        {
            var ip = IPGlobalProperties.GetIPGlobalProperties();
            if (ip.GetActiveTcpListeners().Any(endpoint => TcpPorts.Contains(endpoint.Port)) ||
                ip.GetActiveUdpListeners().Any(endpoint => UdpPorts.Contains(endpoint.Port)))
                throw new InvalidOperationException("Aurora 内置 Sunshine 所需的端口已被其他程序占用。");
        }
        if (OperatingSystem.IsMacOS())
            Directory.CreateDirectory(Profile, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        else
            Directory.CreateDirectory(Profile);
        FileStream profileLock;
        try { profileLock = new FileStream(Path.Combine(Root, "running.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException error) { throw new InvalidOperationException("其他 Aurora 窗口已占用内置 Sunshine 的配置。", error); }

        EventWaitHandle stop = null;
        Process child = null;
        SafeFileHandle job = null;
        try
        {
            if (OperatingSystem.IsMacOS())
            {
                try
                {
                    foreach (var port in TcpPorts)
                    {
                        using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
                        probe.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                        probe.Bind(new IPEndPoint(IPAddress.Any, port));
                        probe.Listen(1);
                    }
                    foreach (var port in UdpPorts)
                    {
                        using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                        probe.Bind(new IPEndPoint(IPAddress.Any, port));
                    }
                }
                catch (SocketException error) when (error.SocketErrorCode == SocketError.AddressAlreadyInUse)
                {
                    throw new InvalidOperationException("Aurora 内置 Sunshine 所需的端口已被其他程序占用。", error);
                }
            }
            if (!File.Exists(ConfigPath))
                File.WriteAllLines(ConfigPath, ManagedOptions.Select(setting => $"{setting.Key} = {setting.Value}"));

            var options = new ProcessStartInfo(Executable)
            {
                WorkingDirectory = Payload,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true
            };
            var password = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
            options.Environment["AURORA_SUNSHINE_PROFILE"] = Profile;
            if (OperatingSystem.IsWindows())
            {
                var eventName = @"Local\AuroraSunshineStop-" + Guid.NewGuid().ToString("N");
                stop = new EventWaitHandle(false, EventResetMode.ManualReset, eventName, out var created);
                if (!created) throw new InvalidOperationException("无法为内置 Sunshine 创建专用停机信号。");
                options.Environment["AURORA_SUNSHINE_STOP_EVENT"] = eventName;
            }
            foreach (var setting in ManagedOptions)
                options.ArgumentList.Add($"{setting.Key}={setting.Value}");

            child = Process.Start(options) ?? throw new InvalidOperationException("无法启动内置 Sunshine。");
            if (OperatingSystem.IsWindows())
            {
                try { job = WindowsChildProcess.StopWithParent(child); }
                catch (Win32Exception error) { throw new InvalidOperationException("无法将 Sunshine 进程绑定到 Aurora。", error); }
            }
            var packet = Encoding.ASCII.GetBytes(password);
            try
            {
                child.StandardInput.BaseStream.Write(packet);
                child.StandardInput.BaseStream.Flush();
            }
            finally { CryptographicOperations.ZeroMemory(packet); }
            if (OperatingSystem.IsWindows()) child.StandardInput.Close();
            var started = new Host(child, job, stop, profileLock);
            lock (ProcessLock)
            {
                host = started;
                adminPassword = password;
            }
            _ = ObserveExit(started);
        }
        catch
        {
            try
            {
                if (child?.HasExited == false) child.Kill(entireProcessTree: true);
            }
            catch (Exception error) when (error is InvalidOperationException or Win32Exception) { }
            finally
            {
                job?.Dispose();
                child?.Dispose();
                stop?.Dispose();
                profileLock.Dispose();
            }
            throw;
        }
    }

    private static async Task ObserveExit(Host started)
    {
        try
        {
            await started.Process.WaitForExitAsync().ConfigureAwait(false);
            await Lifecycle.WaitAsync().ConfigureAwait(false);
            try
            {
                lock (ProcessLock)
                    if (host != started) return;
                if (started.Process.ExitCode == RestartRequested)
                    await StartFresh().ConfigureAwait(false);
                else
                    await StopCore().ConfigureAwait(false);
            }
            finally { Lifecycle.Release(); }
        }
        catch (Exception error) { Trace.TraceError("Sunshine: " + error.Message); }
    }

    private static async Task StopCore()
    {
        Host current;
        lock (ProcessLock)
        {
            current = host;
            host = null;
            adminPassword = null;
        }
        HostSleepPrevention.SetHostActive(false);
        if (current == null) return;

        using (current)
        {
            if (current.Process.HasExited) return;
            try { current.RequestStop(); }
            catch (IOException) when (current.Process.HasExited) { return; }
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(11));
            try { await current.Process.WaitForExitAsync(timeout.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) when (timeout.IsCancellationRequested)
            {
                if (current.Job != null) current.Job.Dispose();
                else if (!current.Process.HasExited)
                {
                    try { current.Process.Kill(entireProcessTree: true); }
                    catch (InvalidOperationException) when (current.Process.HasExited) { }
                }
                using var forcedExitTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                await current.Process.WaitForExitAsync(forcedExitTimeout.Token).ConfigureAwait(false);
            }
        }
    }

    private static async Task WaitForReady()
    {
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < TimeSpan.FromSeconds(60))
        {
            if (!IsRunning)
                throw new InvalidOperationException("Sunshine 启动后已退出。日志：" + LogPath);
            if (await ProbeAdmin().ConfigureAwait(false) && IsRunning) return;
            await Task.Delay(200).ConfigureAwait(false);
        }
        throw new TimeoutException("内置 Sunshine 管理服务未能启动。日志：" + LogPath);
    }

    private static async Task<bool> ProbeAdmin()
    {
        try
        {
            using var certificate = X509Certificate2.CreateFromPem(File.ReadAllText(CertificatePath));
            var expected = certificate.GetCertHash(HashAlgorithmName.SHA256);
            using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(500));
            using var client = new TcpClient(AddressFamily.InterNetwork);
            await client.ConnectAsync(IPAddress.Loopback, AdminEndpoint.Port, timeout.Token).ConfigureAwait(false);
            using var tls = new SslStream(client.GetStream(), leaveInnerStreamOpen: false, (_, presented, _, _) =>
                presented != null && CryptographicOperations.FixedTimeEquals(expected, presented.GetCertHash(HashAlgorithmName.SHA256)));
            await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions { TargetHost = AdminEndpoint.Host },
                timeout.Token).ConfigureAwait(false);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or SocketException or
            AuthenticationException or CryptographicException or OperationCanceledException) { return false; }
    }
}
