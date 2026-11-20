using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using Microsoft.Win32;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Aurora
{
    internal sealed class Device
    {
        public string Id;
        public string Name;
        public string Platform;
        public IPAddress Address;
        public bool Online;
        public StreamHost Lan;
        public StreamHost Tailnet;
        public bool TailnetVisible;
        public bool CanConnect => Lan != null || Tailnet != null;
    }

    internal sealed class NetworkSnapshot
    {
        public IPAddress LocalAddress;
        public long Owner;
        public string LocalId;
        public List<Device> Devices = new List<Device>();
    }

    internal sealed class HostStatus
    {
        public bool Installed;
        public bool SettingsAvailable;
    }

    internal sealed class MoonlightNotFoundException : InvalidOperationException
    {
        public MoonlightNotFoundException() : base("本机没有安装 Moonlight。请从官方发布页安装后再连接。") { }
    }

    internal static class Upstream
    {
        public const string SettingsUrl = "https://localhost:47990";
        public const string ReleasesUrl = "https://github.com/SunshineStream/Sunshine/releases/latest";
        public const string MoonlightReleasesUrl = "https://github.com/moonlight-stream/moonlight-qt/releases/latest";

        internal static string FindMacApplication(string name)
        {
            var user = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return new[] { "/Applications", Path.Combine(user, "Applications") }
                .Select(root => Path.Combine(root, name + ".app")).FirstOrDefault(Directory.Exists);
        }

        private static IEnumerable<string> MacTailscaleCandidates()
        {
            var bundle = FindMacApplication("Tailscale");
            if (bundle != null) yield return Path.Combine(bundle, "Contents", "MacOS", "Tailscale");
            yield return "/usr/local/bin/tailscale";
            yield return "/opt/homebrew/bin/tailscale";
        }

        private static string FindProgram(string relativePath, string name)
        {
            foreach (var root in new[]
            {
                Environment.GetEnvironmentVariable("ProgramW6432"),
                Environment.GetEnvironmentVariable("ProgramFiles"),
                Environment.GetEnvironmentVariable("ProgramFiles(x86)")
            })
            {
                if (string.IsNullOrEmpty(root)) continue;
                var path = Path.Combine(root, relativePath);
                if (File.Exists(path)) return path;
            }
            foreach (var root in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
            {
                var directory = root.Trim().Trim('"');
                if (!Path.IsPathRooted(directory)) continue;
                var path = Path.Combine(directory, name);
                if (File.Exists(path)) return path;
            }
            return null;
        }

        private static string RunHidden(string executable, string arguments, int timeout, bool tailscaleMode = false)
        {
            var options = new ProcessStartInfo(executable, arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            if (tailscaleMode && OperatingSystem.IsMacOS())
                options.Environment["TAILSCALE_BE_CLI"] = "1";
            using (var process = Process.Start(options))
            {
                var output = process.StandardOutput.ReadToEndAsync();
                var error = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(timeout))
                {
                    try { process.Kill(); }
                    catch (InvalidOperationException) { }
                    throw new System.TimeoutException("组件没有及时响应，请稍后重试。");
                }
                if (!Task.WaitAll(new Task[] { output, error }, 5000))
                    throw new System.TimeoutException("组件已退出，但没有关闭输出。");
                if (process.ExitCode == 0) return output.Result;
                var detail = (string.IsNullOrWhiteSpace(error.Result) ? output.Result : error.Result).Trim();
                if (detail.Length > 1000) detail = detail.Substring(detail.Length - 1000);
                throw new InvalidOperationException("退出码 " + process.ExitCode + "：" + detail);
            }
        }

        private static string FindTailscale() =>
            OperatingSystem.IsMacOS() ? MacTailscaleCandidates().FirstOrDefault(File.Exists)
                : FindProgram(@"Tailscale\tailscale.exe", "tailscale.exe");

        public static NetworkSnapshot ReadNetwork()
        {
            var path = FindTailscale();
            if (path == null) throw new InvalidOperationException("找不到 Tailscale，请先安装并登录。");
            return ParseNetwork(RunHidden(path, "status --json", 8000, tailscaleMode: true));
        }

        internal static Process StartTailscale(params string[] arguments)
        {
            var path = FindTailscale() ?? throw new InvalidOperationException("请先安装 Tailscale。");
            var options = new ProcessStartInfo(path)
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
            };
            if (OperatingSystem.IsMacOS()) options.Environment["TAILSCALE_BE_CLI"] = "1";
            foreach (var argument in arguments) options.ArgumentList.Add(argument);
            var process = Process.Start(options);
            process.ErrorDataReceived += (_, _) => { };
            process.BeginErrorReadLine();
            return process;
        }

        public static TailscaleStatus ReadTailscale()
        {
            var path = FindTailscale();
            if (path == null) return new(TailscaleState.NotInstalled);
            return TailscaleStatus.Parse(RunHidden(path, "status --json", 8000, tailscaleMode: true));
        }

        public static void RequestTailscaleConnection()
        {
            var path = FindTailscale() ?? throw new InvalidOperationException("请先安装 Tailscale。");
            try { RunHidden(path, "up", 5000, tailscaleMode: true); }
            catch (System.TimeoutException) { }
        }

        public static void InstallTailscale()
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
            if (FindTailscale() != null) return;
            InstallWindowsPackage("Tailscale.Tailscale");
        }

        private static object Field(IDictionary<string, object> data, string key)
        {
            object value;
            return data != null && data.TryGetValue(key, out value) ? value : null;
        }

        private static string Text(IDictionary<string, object> data, string key)
        {
            return Field(data, key) as string ?? "";
        }

        private static long Number(object value)
        {
            if (value is int) return (int)value;
            if (value is long) return (long)value;
            return 0;
        }

        private static bool Flag(IDictionary<string, object> data, string key)
        {
            var value = Field(data, key);
            return value is bool && (bool)value;
        }

        private static bool Tagged(IDictionary<string, object> data)
        {
            var tags = Field(data, "Tags") as ICollection;
            return tags != null && tags.Count > 0;
        }

        private static IPAddress PickAddress(object value)
        {
            var candidates = value as IEnumerable;
            IPAddress ipv6 = null;
            if (candidates == null) return null;
            foreach (var candidate in candidates)
            {
                var literal = candidate as string;
                IPAddress address;
                if (literal == null || literal.Contains("%") || !IPAddress.TryParse(literal, out address)) continue;
                var bytes = address.GetAddressBytes();
                if (address.AddressFamily == AddressFamily.InterNetwork)
                {
                    if (literal != address.ToString() || bytes[0] != 100 || (bytes[1] & 192) != 64) continue;
                    if ((bytes[1] == 100 && (bytes[2] == 0 || bytes[2] == 100)) ||
                        (bytes[1] == 115 && (bytes[2] == 92 || bytes[2] == 93)) ||
                        literal == "100.101.102.103") continue;
                    return address;
                }
                var canonical = address.ToString();
                if (ipv6 == null && canonical.StartsWith("fd7a:115c:a1e0:", StringComparison.Ordinal) &&
                    !canonical.StartsWith("fd7a:115c:a1e0:b1a:", StringComparison.Ordinal) &&
                    canonical != "fd7a:115c:a1e0::53") ipv6 = address;
            }
            return ipv6;
        }

        internal static NetworkSnapshot ParseNetwork(string json)
        {
            using var document = JsonDocument.Parse(json);
            var data = ToObject(document.RootElement) as IDictionary<string, object>;
            var state = Text(data, "BackendState");
            if (state != "Running")
                throw new InvalidOperationException("Tailscale 尚未连接，请打开 Tailscale 检查登录状态。");
            var local = Field(data, "Self") as IDictionary<string, object>;
            var owner = Number(Field(local, "UserID"));
            if (owner <= 0 || Tagged(local))
                throw new InvalidOperationException("无法确认本机 Tailscale 账号，或本机使用了设备标签。");
            var snapshot = new NetworkSnapshot
            {
                LocalAddress = PickAddress(Field(data, "TailscaleIPs")) ?? PickAddress(Field(local, "TailscaleIPs")),
                Owner = owner,
                LocalId = Text(local, "ID")
            };
            var peers = Field(data, "Peer") as IDictionary<string, object>;
            if (peers == null) return snapshot;
            foreach (var entry in peers.Values)
            {
                var peer = entry as IDictionary<string, object>;
                var id = Text(peer, "ID");
                if (string.IsNullOrWhiteSpace(id) || id == Text(local, "ID") || Number(Field(peer, "UserID")) != owner ||
                    Tagged(peer) || Flag(peer, "ShareeNode") || Number(Field(peer, "AltSharerUserID")) != 0) continue;
                var address = PickAddress(Field(peer, "TailscaleIPs"));
                if (address == null) continue;
                var name = Text(peer, "HostName");
                if (name.Length == 0) name = Text(peer, "DNSName").TrimEnd('.');
                snapshot.Devices.Add(new Device
                {
                    Id = id, Name = name.Length == 0 ? id : name, Address = address,
                    Online = Flag(peer, "Online"), Platform = Text(peer, "OS")
                });
            }
            snapshot.Devices = snapshot.Devices.OrderByDescending(device => device.Online)
                .ThenBy(device => device.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
            return snapshot;
        }

        private static object ToObject(JsonElement element)
        {
            switch (element.ValueKind)
            {
                case JsonValueKind.Object:
                    return element.EnumerateObject().ToDictionary(property => property.Name, property => ToObject(property.Value));
                case JsonValueKind.Array:
                    return element.EnumerateArray().Select(ToObject).ToArray();
                case JsonValueKind.String:
                    return element.GetString();
                case JsonValueKind.Number:
                    return element.TryGetInt64(out var value) ? value : 0L;
                case JsonValueKind.True:
                    return true;
                case JsonValueKind.False:
                    return false;
                default:
                    return null;
            }
        }

        public static HostStatus ReadHost()
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
            using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\SunshineService");
            var processes = Process.GetProcessesByName("sunshine");
            var running = processes.Length != 0;
            foreach (var process in processes) process.Dispose();
            var installed = service != null || running || FindProgram(@"Sunshine\sunshine.exe", "sunshine.exe") != null;
            if (running)
            {
                foreach (var address in new[] { IPAddress.Loopback, IPAddress.IPv6Loopback })
                {
                    try
                    {
                        using (var client = new TcpClient(address.AddressFamily))
                        {
                            if (client.ConnectAsync(address, 47990).Wait(250) && client.Connected)
                                return new HostStatus { Installed = true, SettingsAvailable = true };
                        }
                    }
                    catch (SocketException) { }
                    catch (AggregateException) { }
                }
            }
            return new HostStatus { Installed = installed };
        }

        public static void InstallSunshine()
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
            if (ReadHost().Installed) throw new InvalidOperationException("已检测到 Sunshine，不会覆盖安装。请刷新状态。");
            InstallWindowsPackage("LizardByte.Sunshine");
        }

        private static void InstallWindowsPackage(string packageId)
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var alias = Path.Combine(local, @"Microsoft\WindowsApps\winget.exe");
            var winget = File.Exists(alias) ? alias : FindProgram("winget.exe", "winget.exe");
            if (winget == null) throw new InvalidOperationException("未找到 WinGet。请更新 Microsoft Store 的「应用安装程序」，或使用官方下载。");
            RunHidden(winget, $"install --id {packageId} --exact --source winget --interactive --disable-interactivity", -1);
        }

        private static string FindMoonlight()
        {
            var mac = OperatingSystem.IsMacOS();
            var integrated = mac
                ? Path.Combine(AppContext.BaseDirectory, "AuroraStream")
                : Path.Combine(AppContext.BaseDirectory, "Streamer", "AuroraStream.exe");
            if (File.Exists(integrated)) return integrated;
            var bundle = mac ? FindMacApplication("Moonlight") : null;
            var path = mac ? bundle == null ? null : Path.Combine(bundle, "Contents", "MacOS", "Moonlight")
                : FindProgram(@"Moonlight Game Streaming\Moonlight.exe", "Moonlight.exe");
            if (!mac)
            {
                var separate = Path.Combine(AppContext.BaseDirectory, "Moonlight", "Moonlight.exe");
                if (File.Exists(separate)) path = separate;
            }
            if (path == null || !File.Exists(path)) throw new MoonlightNotFoundException();
            return path;
        }

        public static Process LaunchMoonlight(StreamHost route, bool pair) =>
            StartMoonlight(FindMoonlight(), MoonlightArguments(route, pair), redirect: false);

        internal static Process StartMoonlight(string path, string[] arguments, bool redirect)
        {
            var options = new ProcessStartInfo(path)
            {
                WorkingDirectory = Path.GetDirectoryName(path),
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = redirect,
                RedirectStandardError = redirect
            };
            foreach (var argument in StreamRuntimeArguments(path, arguments)) options.ArgumentList.Add(argument);
            return Process.Start(options);
        }

        internal static string[] StreamRuntimeArguments(string path, string[] arguments) =>
            Path.GetFileNameWithoutExtension(path) == "AuroraStream"
                ? new[] { "--aurora-runtime" }.Concat(arguments).ToArray()
                : arguments;

        internal static Task<(int ExitCode, string Output)> ListMoonlight(StreamHost route, CancellationToken token) =>
            RunMoonlight(FindMoonlight(), new[] { "list", "--csv", route.Endpoint.ToString() }, token);

        internal static async Task<string> FindBackgroundMoonlight(CancellationToken token)
        {
            var path = FindMoonlight();
            var (exit, output) = await RunMoonlight(path, new[] { "pair", "--help" }, token);
            if (exit != 0 || !output.Contains("--aurora-background", StringComparison.Ordinal) ||
                !output.Contains("--aurora-pair-id", StringComparison.Ordinal))
                throw new InvalidOperationException("本机串流组件还不支持后台配对。首次连接需要带后台配对功能的极光串流组件。");
            return path;
        }

        internal static async Task<(int ExitCode, string Output)> RunMoonlight(string path, string[] arguments, CancellationToken token)
        {
            using var process = StartMoonlight(path, arguments, redirect: true);
            var output = process.StandardOutput.ReadToEndAsync(token);
            var error = process.StandardError.BaseStream.CopyToAsync(Stream.Null, token);
            try
            {
                await process.WaitForExitAsync(token);
                await error;
                return (process.ExitCode, await output);
            }
            finally { StopProcess(process); }
        }

        internal static void StopProcess(Process process)
        {
            if (process == null) return;
            try { if (!process.HasExited) process.Kill(); }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        }

        internal static string[] MoonlightArguments(StreamHost route, bool pair) => pair
            ? new[] { "pair", route.Endpoint.ToString() }
            : new[] { "stream", route.Endpoint.ToString(), "Desktop" };
    }
}
