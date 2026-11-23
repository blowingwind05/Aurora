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
    internal enum DevicePowerAction { Restart, Shutdown }
    internal enum SessionPrivacyCommand { Status, Enable, Disable }
    internal enum SessionPrivacyState { Unavailable, Off, On }

    internal sealed class Device
    {
        public string Id;
        public string Name;
        public string Platform;
        public IPAddress Address;
        public IPAddress AlternateAddress;
        public IPAddress LanAddress;
        public bool Online;
        public StreamHost Lan;
        public StreamHost Tailnet;
        public bool TailnetVisible;
        public bool CanConnect => Lan != null || Tailnet != null;
        public bool SupportsPower => Lan?.SupportsPower == true || Tailnet?.SupportsPower == true;
    }

    internal sealed class NetworkSnapshot
    {
        public IPAddress LocalAddress;
        public long Owner;
        public string LocalId;
        public TailscaleMode Mode;
        public int Generation;
        public List<Device> Devices = new List<Device>();
        public bool IsAuthenticated => Owner > 0 && !string.IsNullOrWhiteSpace(LocalId) && LocalAddress != null;
    }

    internal sealed class HostStatus
    {
        public bool Installed;
        public bool SettingsAvailable;
        public bool Managed;
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

        private static string RunHidden(ProcessStartInfo options, int timeout)
        {
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

        internal static bool HasSystemTailscale => FindSystemTailscale() != null;

        private static string FindSystemTailscale() => OperatingSystem.IsWindows()
            ? BundledTailscale.FindWindowsCli(TailscaleMode.System) : null;

        public static NetworkSnapshot ReadNetwork()
        {
            var mode = TailscaleRuntime.Mode;
            var generation = TailscaleRuntime.Generation;
            VerifyActiveMode(mode);
            var snapshot = ParseNetwork(ReadStatusJson(mode));
            VerifyActiveMode(mode);
            if (generation != TailscaleRuntime.Generation) throw new InvalidOperationException("Tailscale 账号已切换，请重新连接。");
            snapshot.Mode = mode;
            snapshot.Generation = generation;
            return snapshot;
        }

        private static void VerifyActiveMode(TailscaleMode mode)
        {
            if (TailscaleRuntime.IsTransitioning || mode != TailscaleRuntime.Mode)
                throw new InvalidOperationException("Tailscale 连接方式或账号正在切换，请稍后重试。");
        }

        private static ProcessStartInfo TailscaleCommand(TailscaleMode mode, string[] arguments)
        {
            if (mode != TailscaleMode.System) throw new InvalidOperationException("内置 Tailscale 通过 Aurora 网络组件连接。");
            var path = FindSystemTailscale() ?? throw new InvalidOperationException("请在「Tailscale 账户」中启用 Tailscale。");
            var options = new ProcessStartInfo(path)
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            };
            // Windows pipe names are case-insensitive. Spelling out the name in lowercase also
            // keeps the upstream CLI from falling back to its per-user development pipe.
            options.ArgumentList.Add(@"--socket=\\.\pipe\" + BundledTailscale.SystemPipe.ToLowerInvariant());
            foreach (var argument in arguments) options.ArgumentList.Add(argument);
            return options;
        }

        private static string RunTailscale(TailscaleMode mode, int timeout, params string[] arguments) =>
            RunHidden(TailscaleCommand(mode, arguments), timeout);

        private static string ReadStatusJson(TailscaleMode mode) => mode == TailscaleMode.Embedded
            ? EmbeddedTailnet.ReadStatus() : RunTailscale(mode, 8000, "status", "--json");

        internal static Process StartTailscale(NetworkSnapshot network, params string[] arguments)
        {
            VerifyActiveMode(network.Mode);
            if (network.Generation != TailscaleRuntime.Generation)
                throw new InvalidOperationException("Tailscale 账号已切换，请重新连接。");
            var options = TailscaleCommand(network.Mode, arguments);
            options.RedirectStandardInput = true;
            var process = Process.Start(options);
            process.ErrorDataReceived += (_, _) => { };
            process.BeginErrorReadLine();
            return process;
        }

        public static TailscaleStatus ReadTailscale() => ReadTailscaleSnapshot().Status;

        internal static (TailscaleStatus Status, NetworkSnapshot Network) ReadAccountNetwork()
        {
            var mode = TailscaleRuntime.Mode;
            var generation = TailscaleRuntime.Generation;
            var (status, json) = ReadTailscaleSnapshot(mode);
            if (status.State != TailscaleState.Connected) return (status, null);
            var network = ParseNetwork(json);
            network.Mode = mode;
            network.Generation = generation;
            VerifyActiveMode(mode);
            if (generation != TailscaleRuntime.Generation) return (new(TailscaleState.Starting), null);
            return (status, network.IsAuthenticated ? network : null);
        }

        private static (TailscaleStatus Status, string Json) ReadTailscaleSnapshot(TailscaleMode? selected = null)
        {
            var mode = selected ?? TailscaleRuntime.Mode;
            if (TailscaleRuntime.IsTransitioning || mode != TailscaleRuntime.Mode) return (new(TailscaleState.Starting), null);
            if (mode == TailscaleMode.Embedded && !EmbeddedTailnet.IsAvailable || mode == TailscaleMode.System && !HasSystemTailscale)
                return (new(TailscaleState.NotInstalled), null);
            if (OperatingSystem.IsWindows() && mode == TailscaleMode.System)
            {
                var service = BundledTailscale.ReadWindowsService(mode);
                if (service == WindowsServiceState.Starting) return (new(TailscaleState.Starting), null);
                if (service == WindowsServiceState.Stopped) return (new(TailscaleState.Stopped), null);
            }
            var json = ReadStatusJson(mode);
            return (TailscaleStatus.Parse(json), json);
        }

        internal static void LogoutTailscale()
        {
            var mode = TailscaleRuntime.Mode;
            VerifyActiveMode(mode);
            if (mode == TailscaleMode.Embedded) EmbeddedTailnet.Request("logout").GetAwaiter().GetResult();
            else RunTailscale(mode, 8000, "logout");
        }

        public static void RequestTailscaleConnection()
        {
            var mode = TailscaleRuntime.Mode;
            VerifyActiveMode(mode);
            if (mode == TailscaleMode.Embedded)
            {
                EmbeddedTailnet.Request("login").GetAwaiter().GetResult();
                return;
            }
            try { RunTailscale(mode, 5000, "up"); }
            catch (System.TimeoutException) { }
        }

        internal static TailscaleProfile[] ReadSystemTailscaleProfiles()
        {
            if (!HasSystemTailscale || OperatingSystem.IsWindows() &&
                BundledTailscale.ReadWindowsService(TailscaleMode.System) != WindowsServiceState.Running)
                return Array.Empty<TailscaleProfile>();

            try { return ParseTailscaleProfiles(RunTailscale(TailscaleMode.System, 8000, "switch", "--list", "--json")); }
            catch (InvalidOperationException error) when (error.Message.Contains("flag provided but not defined", StringComparison.Ordinal)
                && error.Message.Contains("json", StringComparison.Ordinal))
            {
                return ParseLegacyTailscaleProfiles(RunTailscale(TailscaleMode.System, 8000, "switch", "--list"));
            }
        }

        internal static void SwitchSystemTailscaleProfile(string profileId)
        {
            ValidateProfileId(profileId);
            try { RunTailscale(TailscaleMode.System, 12000, "switch", "--", profileId); }
            catch (Exception error) when (error is InvalidOperationException or TimeoutException)
            {
                // The CLI may report a timeout after changing the profile while the new tailnet starts.
                if (!ReadSystemTailscaleProfiles().Any(profile => profile.Id == profileId && profile.IsCurrent)) throw;
            }
        }

        internal static TailscaleProfile[] ParseTailscaleProfiles(string json)
        {
            using var document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Array)
                throw new InvalidOperationException("无法读取系统 Tailscale 返回的账号列表。");
            var profiles = new List<TailscaleProfile>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var item in document.RootElement.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.Object) continue;
                var id = ReadString(item, "id");
                var login = ReadString(item, "account");
                if (!ValidProfileId(id) || string.IsNullOrWhiteSpace(login) || !seen.Add(id)) continue;
                profiles.Add(new(id, ReadString(item, "nickname"), login, ReadString(item, "tailnet"),
                    item.TryGetProperty("selected", out var selected) && selected.ValueKind == JsonValueKind.True));
            }
            return profiles.ToArray();

            static string ReadString(JsonElement item, string name) =>
                item.TryGetProperty(name, out var data) && data.ValueKind == JsonValueKind.String ? data.GetString() : null;
        }

        private static bool ValidProfileId(string id) => id is { Length: > 0 and <= 128 } &&
            char.IsAsciiLetterOrDigit(id[0]) && id.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.');

        internal static TailscaleProfile[] ParseLegacyTailscaleProfiles(string output)
        {
            // Older clients print an aligned ID / Tailnet / Account table and mark the
            // selected account with an asterisk. Use the header to preserve spaces in names.
            using var reader = new StringReader(output);
            var header = reader.ReadLine() ?? "";
            var tailnetColumn = header.IndexOf("Tailnet", StringComparison.Ordinal);
            var accountColumn = header.IndexOf("Account", StringComparison.Ordinal);
            if (!header.StartsWith("ID", StringComparison.Ordinal) || tailnetColumn < 2 || accountColumn <= tailnetColumn)
                throw new InvalidOperationException("当前系统 Tailscale 的账号列表格式无法识别。");
            var profiles = new List<TailscaleProfile>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            for (var line = reader.ReadLine(); line != null; line = reader.ReadLine())
            {
                if (line.Length <= accountColumn) continue;
                var id = line[..tailnetColumn].Trim();
                var tailnet = line[tailnetColumn..accountColumn].Trim();
                var account = line[accountColumn..].Trim();
                var selected = account.EndsWith('*');
                if (selected) account = account[..^1].TrimEnd();
                if (ValidProfileId(id) && account.Length != 0 && seen.Add(id))
                    profiles.Add(new(id, null, account, tailnet, selected));
            }
            return profiles.ToArray();
        }

        private static void ValidateProfileId(string id)
        {
            if (!ValidProfileId(id)) throw new InvalidOperationException("系统 Tailscale 账号标识无效。");
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

        private static IPAddress PickAddress(object value, bool ipv6Only = false)
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
                    if (ipv6Only) continue;
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
                    AlternateAddress = address.AddressFamily == AddressFamily.InterNetwork
                        ? PickAddress(Field(peer, "TailscaleIPs"), ipv6Only: true) : null,
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

        public static bool SupportsHost => OperatingSystem.IsWindows() || BundledSunshine.IsAvailable;

        public static HostStatus ReadHost()
        {
            if (BundledSunshine.IsRunning)
                return new HostStatus { Managed = true, Installed = true, SettingsAvailable = BundledSunshine.IsReady() };
            if (BundledSunshine.IsAvailable)
                return new HostStatus { Managed = true };
            if (!OperatingSystem.IsWindows()) return new HostStatus();
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

        public static Task StartSunshine()
        {
            return BundledSunshine.StartAsync();
        }

        internal static string FindMoonlight()
        {
            var mac = OperatingSystem.IsMacOS();
            var integrated = mac
                ? Path.Combine(AppContext.BaseDirectory, "AuroraStream")
                : Path.Combine(AppContext.BaseDirectory, "Components", "Moonlight", "AuroraStream.exe");
            if (File.Exists(integrated)) return integrated;
            var bundle = mac ? FindMacApplication("Moonlight") : null;
            var path = mac ? bundle == null ? null : Path.Combine(bundle, "Contents", "MacOS", "Moonlight")
                : FindProgram(@"Moonlight Game Streaming\Moonlight.exe", "Moonlight.exe");
            if (!mac)
            {
                var oldIntegrated = Path.Combine(AppContext.BaseDirectory, "Streamer", "AuroraStream.exe");
                if (File.Exists(oldIntegrated)) return oldIntegrated;
                var separate = Path.Combine(AppContext.BaseDirectory, "Moonlight", "Moonlight.exe");
                if (File.Exists(separate)) path = separate;
            }
            if (path == null || !File.Exists(path)) throw new MoonlightNotFoundException();
            return path;
        }

        internal static Process LaunchMoonlight(string path, StreamHost route, bool pair, string embeddedSession = null)
        {
            var arguments = MoonlightArguments(route, pair);
            if (!pair)
            {
                var bundled = Path.GetFileNameWithoutExtension(path) == "AuroraStream";
                var useViewerDefault = bundled && (OperatingSystem.IsMacOS() || embeddedSession != null);
                arguments = arguments.Concat(StreamPreferences.Arguments(useViewerDefault, useBundledDefaults: bundled)).ToArray();
            }
            return StartMoonlight(path, arguments, redirect: false, embeddedSession, route);
        }

        internal static Process StartMoonlight(string path, string[] arguments, bool redirect, string embeddedSession = null,
            StreamHost streamRoute = null)
        {
            var options = new ProcessStartInfo(path)
            {
                WorkingDirectory = Path.GetDirectoryName(path),
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = redirect,
                RedirectStandardError = redirect
            };
            if (OperatingSystem.IsWindows())
            {
                options.Environment.Remove("AURORA_EMBEDDED_SESSION");
                if (embeddedSession != null) options.Environment["AURORA_EMBEDDED_SESSION"] = embeddedSession;
            }
            options.Environment.Remove("AURORA_GAMESTREAM_RELAY_BASE");
            options.Environment.Remove("AURORA_GAMESTREAM_HOST_UUID");
            if (streamRoute?.ViaEmbeddedTailnet == true)
            {
                if (Path.GetFileNameWithoutExtension(path) != "AuroraStream" ||
                    !IPAddress.Loopback.Equals(streamRoute.Endpoint.Address) || string.IsNullOrWhiteSpace(streamRoute.Id))
                    throw new InvalidOperationException("内置 Tailscale 需要随 Aurora 打包的串流组件。");
                options.Environment["AURORA_GAMESTREAM_RELAY_BASE"] = streamRoute.Endpoint.Port.ToString(System.Globalization.CultureInfo.InvariantCulture);
                options.Environment["AURORA_GAMESTREAM_HOST_UUID"] = streamRoute.Id;
            }
            foreach (var argument in StreamRuntimeArguments(path, arguments)) options.ArgumentList.Add(argument);
            return Process.Start(options);
        }

        internal static string[] StreamRuntimeArguments(string path, string[] arguments) =>
            Path.GetFileNameWithoutExtension(path) == "AuroraStream" &&
            !(arguments.Length == 1 && arguments[0] is "--aurora-embedded-info" or "--aurora-gamestream-relay-info")
                ? new[] { "--aurora-runtime" }.Concat(arguments).ToArray()
                : arguments;

        internal static async Task RequireRelayRuntime(string path, CancellationToken token)
        {
            if (Path.GetFileNameWithoutExtension(path) == "AuroraStream")
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                timeout.CancelAfter(TimeSpan.FromSeconds(4));
                var (exit, output) = await RunMoonlight(path, new[] { "--aurora-gamestream-relay-info" }, timeout.Token);
                if (exit == 0 && output.Trim() == "aurora-gamestream-relay/1") return;
            }
            throw new InvalidOperationException("内置 Tailscale 需要新版 Aurora 串流组件，请使用完整的集成包。");
        }

        internal static async Task<(int ExitCode, string Output)> ListMoonlight(string runtime, EmbeddedTailnet.StreamLease stream,
            CancellationToken token)
        {
            var route = stream.Route;
            if (route.ViaEmbeddedTailnet) await RequireRelayRuntime(runtime, token);
            return await RunMoonlight(runtime, new[] { "list", route.Endpoint.ToString() }, token, route);
        }

        internal static async Task<(bool Reachable, byte[] Preview)> ReadWallpaper(
            StreamHost route, string revision, CancellationToken cancellation)
        {
            var path = FindMoonlight();
            if (Path.GetFileNameWithoutExtension(path) != "AuroraStream") return default;
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            timeout.CancelAfter(TimeSpan.FromSeconds(5));
            try
            {
                using var stream = await EmbeddedTailnet.OpenStream(route, timeout.Token);
                var (exit, output) = await RunMoonlight(path, new[] { "wallpaper", stream.Route.Endpoint.ToString(), route.Id,
                    revision.Length == 0 ? "-" : revision }, timeout.Token, stream.Route);
                if (exit != 0) return default;
                var payload = output.Trim();
                if (payload == "=") return (true, null);
                if (payload.Length == 0 || payload.Length > (DesktopWallpaper.MaxPreviewBytes + 2) / 3 * 4) return default;
                var preview = Convert.FromBase64String(payload);
                return preview.Length <= DesktopWallpaper.MaxPreviewBytes ? (true, preview) : default;
            }
            catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { return default; }
        }

        internal static async Task<string> FindBackgroundMoonlight(CancellationToken token)
        {
            var path = FindMoonlight();
            if (Path.GetFileNameWithoutExtension(path) == "AuroraStream")
            {
                // Moonlight displays help in a modal Windows dialog unless pairing is in background mode.
                var (exit, output) = await RunMoonlight(path, new[] { "pair", "--aurora-background", "--help" }, token);
                if (exit == 0 && output.Contains("--aurora-background", StringComparison.Ordinal) &&
                    output.Contains("--aurora-pair-id", StringComparison.Ordinal))
                    return path;
            }
            throw new InvalidOperationException("本机串流组件还不支持后台配对。首次连接需要带后台配对功能的 Aurora 串流组件。");
        }

        internal static async Task RequestDevicePower(StreamHost route, DevicePowerAction action, CancellationToken token)
        {
            var command = action switch
            {
                DevicePowerAction.Restart => "restart",
                DevicePowerAction.Shutdown => "shutdown",
                _ => throw new ArgumentOutOfRangeException(nameof(action))
            };
            var path = FindMoonlight();
            if (Path.GetFileNameWithoutExtension(path) != "AuroraStream")
                throw new InvalidOperationException("远程电源操作需要 Aurora 内置的串流组件。");

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(12));
            if (route.ViaEmbeddedTailnet) await RequireRelayRuntime(path, timeout.Token);
            using var stream = await EmbeddedTailnet.OpenStream(route, timeout.Token);
            try
            {
                var (exit, output) = await RunMoonlight(path,
                    new[] { "power", stream.Route.Endpoint.ToString(), route.Id, command }, timeout.Token, stream.Route);
                if (exit == 0 && output.Trim() == "accepted") return;
                if (exit == 3) throw new InvalidOperationException("请先连接这台设备，完成配对后再进行电源操作。");
            }
            catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
            throw new InvalidOperationException("未收到被控端确认，请先检查设备状态和系统权限。");
        }

        internal static async Task<SessionPrivacyState> ChangeSessionPrivacy(string runtime, StreamHost route,
            SessionPrivacyCommand command, CancellationToken token)
        {
            var action = command switch
            {
                SessionPrivacyCommand.Status => "status",
                SessionPrivacyCommand.Enable => "on",
                SessionPrivacyCommand.Disable => "off",
                _ => throw new ArgumentOutOfRangeException(nameof(command))
            };
            // The active stream owns this route and its relay lease.
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(8));
            var (exit, output) = await RunMoonlight(runtime,
                new[] { "privacy", route.Endpoint.ToString(), route.Id, action }, timeout.Token, route);
            if (exit == 0)
            {
                switch (output.Trim())
                {
                    case "off": return SessionPrivacyState.Off;
                    case "on": return SessionPrivacyState.On;
                    case "unavailable": return SessionPrivacyState.Unavailable;
                }
            }
            throw new InvalidOperationException("未收到被控端的隐私屏保状态。");
        }

        internal static async Task<(int ExitCode, string Output)> RunMoonlight(string path, string[] arguments, CancellationToken token,
            StreamHost streamRoute = null)
        {
            using var process = StartMoonlight(path, arguments, redirect: true, streamRoute: streamRoute);
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
