using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal static class AutomaticPairing
{
    internal const int Port = 43826;
    private static readonly byte[] Request = "AURORA/PAIR/1\n"u8.ToArray();
    private const string SettingsKey = @"Software\Aurora\AutomaticPairing";
    private const string SettingsValue = "Account";

    public static bool IsConfigured => OperatingSystem.IsWindows() && ReadConfiguration() != null;
    private static string ReadConfiguration()
    {
        if (!OperatingSystem.IsWindows()) return null;
        using var key = Registry.CurrentUser.OpenSubKey(SettingsKey);
        return key?.GetValue(SettingsValue) as string;
    }

    private static string Scope(NetworkSnapshot network, SunshineAdmin admin) =>
        FormattableString.Invariant($"{network.Owner}/{network.LocalId}/{admin.CredentialScope}");

    public static bool IsConfiguredFor(NetworkSnapshot network, SunshineAdmin admin) =>
        network?.LocalAddress != null && !string.IsNullOrEmpty(network.LocalId) &&
        string.Equals(ReadConfiguration(), Scope(network, admin), StringComparison.Ordinal);

    public static void SetConfigured(NetworkSnapshot network, SunshineAdmin admin)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
        using var key = Registry.CurrentUser.CreateSubKey(SettingsKey);
        if (network == null) key.DeleteValue(SettingsValue, false);
        else
        {
            if (network.LocalAddress == null || string.IsNullOrEmpty(network.LocalId))
                throw new InvalidOperationException("请先在本机登录 Tailscale。");
            key.SetValue(SettingsValue, Scope(network, admin));
        }
    }

    public static async Task Pair(Device device, string moonlight, CancellationToken token)
    {
        var route = device.Tailnet ?? throw new InvalidOperationException("首次自动配对需要两台设备登录同一 Tailscale 账号。");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(38));
        var current = await Task.Run(Upstream.ReadNetwork, timeout.Token).WaitAsync(timeout.Token);
        if (!current.Devices.Any(peer => peer.Id == device.Id && peer.Address.Equals(route.Endpoint.Address)))
            throw new InvalidOperationException("设备的 Tailscale 身份已变化，请刷新设备列表。");

        using var relay = Upstream.StartTailscale("nc", route.Endpoint.Address.ToString(), Port.ToString(CultureInfo.InvariantCulture));
        try
        {
            var write = relay.StandardInput.BaseStream;
            var read = relay.StandardOutput.BaseStream;
            await write.WriteAsync(Request, timeout.Token);
            await write.FlushAsync(timeout.Token);
            var reply = new byte[37];
            using (var hello = CancellationTokenSource.CreateLinkedTokenSource(timeout.Token))
            {
                hello.CancelAfter(TimeSpan.FromSeconds(10));
                try
                {
                    await read.ReadExactlyAsync(reply.AsMemory(0, 1), hello.Token);
                    if (reply[0] == 0) throw new InvalidOperationException("被控端当前无法自动配对，请检查「本机被控」的状态。");
                    await read.ReadExactlyAsync(reply.AsMemory(1), hello.Token);
                }
                catch (Exception error) when (error is IOException || error is OperationCanceledException && !token.IsCancellationRequested)
                {
                    throw new InvalidOperationException("无法联系对端极光。请确认被控电脑的极光正在运行，且已允许同账号设备连接。");
                }
            }
            if (reply[0] != 1 || !reply.Skip(1).Take(32).All(IsLowerHex) || !reply.Skip(33).All(IsDigit))
                throw new InvalidOperationException("对端极光的配对协议不兼容。");
            var marker = Encoding.ASCII.GetString(reply, 1, 32);
            var pin = Encoding.ASCII.GetString(reply, 33, 4);
            var moonlightPair = Upstream.RunMoonlight(moonlight, new[]
            {
                "pair", "--aurora-background", "--aurora-pair-id", marker, "--pin", pin, route.Endpoint.ToString()
            }, timeout.Token);
            try
            {
                var result = await moonlightPair;
                if (result.ExitCode != 0) throw new InvalidOperationException("串流组件未完成后台配对，请检查被控端状态后重试。");
                // A concurrently paired client may finish before generating a new Sunshine request.
                // The caller independently checks Moonlight's persisted trust before starting a stream.
            }
            finally { timeout.Cancel(); }
        }
        finally { Upstream.StopProcess(relay); }
    }

    private static bool IsLowerHex(byte value) => IsDigit(value) || value is >= (byte)'a' and <= (byte)'f';
    private static bool IsDigit(byte value) => value is >= (byte)'0' and <= (byte)'9';

    internal static SunshinePairing FindPending(SunshinePairing[] requests, IPAddress source, string marker)
    {
        var matches = requests.Where(item => item.Name == "aurora-" + marker &&
            IPAddress.TryParse(item.Address, out var address) && Normalize(address).Equals(Normalize(source))).Take(2).ToArray();
        return matches.Length == 1 ? matches[0] : null;
    }

    private static IPAddress Normalize(IPAddress address) => address.IsIPv4MappedToIPv6 ? address.MapToIPv4() : address;

    private static async Task CheckPort(CancellationToken token)
    {
        using var status = Upstream.StartTailscale("serve", "status", "--json");
        status.StandardInput.Close();
        try
        {
            using var statusConfig = JsonDocument.Parse(await status.StandardOutput.ReadToEndAsync(token));
            await status.WaitForExitAsync(token);
            if (status.ExitCode != 0 || statusConfig.RootElement.ValueKind is not (JsonValueKind.Object or JsonValueKind.Null))
                throw new InvalidOperationException("读取 Tailscale 转发配置失败。");
            if (UsesPort(statusConfig.RootElement))
                throw new InvalidOperationException("Tailscale 的自动配对端口已被占用，请检查现有转发配置。");
        }
        finally { Upstream.StopProcess(status); }
    }

    internal static bool UsesPort(JsonElement config)
    {
        if (config.ValueKind != JsonValueKind.Object) return false;
        var port = Port.ToString(CultureInfo.InvariantCulture);
        if (config.TryGetProperty("TCP", out var tcp) && tcp.ValueKind == JsonValueKind.Object && tcp.TryGetProperty(port, out _))
            return true;
        if (config.TryGetProperty("AllowFunnel", out var funnel) && funnel.ValueKind == JsonValueKind.Object &&
            funnel.EnumerateObject().Any(entry => entry.Name.EndsWith(":" + port, StringComparison.Ordinal) &&
                entry.Value.ValueKind == JsonValueKind.True)) return true;
        return config.TryGetProperty("Foreground", out var sessions) && sessions.ValueKind == JsonValueKind.Object &&
            sessions.EnumerateObject().Any(session => UsesPort(session.Value));
    }

    internal static async Task<IPEndPoint> ReadProxySource(Stream stream, CancellationToken token)
    {
        var header = new byte[16];
        await stream.ReadExactlyAsync(header, token);
        var length = BinaryPrimitives.ReadUInt16BigEndian(header.AsSpan(14));
        var addressLength = header[13] switch { 0x11 => 4, 0x21 => 16, _ => 0 };
        if (!header.AsSpan(0, 12).SequenceEqual("\r\n\r\n\0\r\nQUIT\n"u8) ||
            header[12] != 0x21 || addressLength == 0 || length < addressLength * 2 + 4 || length > 512)
            throw new InvalidDataException();
        var payload = new byte[length];
        await stream.ReadExactlyAsync(payload, token);
        return new IPEndPoint(Normalize(new IPAddress(payload.AsSpan(0, addressLength))),
            BinaryPrimitives.ReadUInt16BigEndian(payload.AsSpan(addressLength * 2)));
    }

    internal sealed class Host : IDisposable
    {
        private readonly SunshineAdmin admin;
        private readonly NetworkSnapshot enabledNetwork;
        private readonly TcpListener listener = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource shutdown = new();
        private readonly SemaphoreSlim pairing = new(1);
        private Process relay;
        private SafeFileHandle relayJob;
        private Task lifetime;
        private bool disposed;

        public Host(SunshineAdmin admin, NetworkSnapshot network)
        {
            this.admin = admin;
            enabledNetwork = network;
        }

        public async Task Start()
        {
            await admin.Pending(shutdown.Token); // Fail early on incompatible Sunshine versions.
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            await CheckPort(timeout.Token);
            timeout.Token.ThrowIfCancellationRequested();
            listener.Server.ExclusiveAddressUse = true;
            listener.Start(8);
            var local = (IPEndPoint)listener.LocalEndpoint;
            relay = Upstream.StartTailscale("serve", "--tcp=" + Port, "--proxy-protocol=2", "tcp://" + local);
            relayJob = WindowsChildProcess.StopWithParent(relay);
            relay.StandardInput.Close();
            try
            {
                // The foreground command prints its endpoint only after the service accepts the configuration.
                var line = await relay.StandardOutput.ReadLineAsync(timeout.Token);
                if (line?.Trim() != "Available within your tailnet:" || relay.HasExited) throw new InvalidOperationException();
            }
            catch (Exception error) when (error is InvalidOperationException or OperationCanceledException or IOException)
            {
                throw new InvalidOperationException("无法启动 Tailscale 自动配对入口。请检查系统 Tailscale 版本和策略。");
            }
            lifetime = Listen(relay, shutdown.Token);
        }

        public Task Completion => lifetime;
        public bool Matches(NetworkSnapshot network) =>
            network?.Owner == enabledNetwork.Owner && network.LocalId == enabledNetwork.LocalId &&
            network.LocalAddress?.Equals(enabledNetwork.LocalAddress) == true;

        private async Task Listen(Process process, CancellationToken token)
        {
            var drained = process.StandardOutput.ReadToEndAsync(token);
            var exit = process.WaitForExitAsync(token);
            var workers = new Task[4];
            for (var index = 0; index < workers.Length; index++) workers[index] = Accept(token);
            try { await Task.WhenAny(exit, Task.WhenAll(workers)); }
            finally
            {
                shutdown.Cancel();
                listener.Stop();
                try { await Task.WhenAll(workers); }
                finally
                {
                    try { await drained; }
                    catch (OperationCanceledException) { }
                }
            }
        }

        private async Task Accept(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    using var client = await listener.AcceptTcpClientAsync(token);
                    await Handle(client, token);
                }
                catch (Exception error) when (error is IOException or SocketException or OperationCanceledException or
                    ObjectDisposedException or InvalidOperationException or FormatException or System.Text.Json.JsonException)
                {
                    if (token.IsCancellationRequested) return;
                }
            }
        }

        private async Task Handle(TcpClient client, CancellationToken token)
        {
            if (client.Client.RemoteEndPoint is not IPEndPoint socketSource || client.Client.LocalEndPoint is not IPEndPoint socketTarget ||
                !await TailscaleLoopback.IsFromService(socketSource, socketTarget, token)) return;
            using var stream = client.GetStream();
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(token);
            attempt.CancelAfter(TimeSpan.FromSeconds(31));
            var ct = attempt.Token;
            IPEndPoint source;
            using (var hello = CancellationTokenSource.CreateLinkedTokenSource(ct))
            {
                hello.CancelAfter(TimeSpan.FromSeconds(3));
                source = await ReadProxySource(stream, hello.Token);
                var request = new byte[Request.Length];
                await stream.ReadExactlyAsync(request, hello.Token);
                if (!request.SequenceEqual(Request)) return;
            }
            var local = await Task.Run(Upstream.ReadNetwork, ct).WaitAsync(ct);
            var peer = FindAuthorizedPeer(local, source.Address);
            if (peer == null || !await pairing.WaitAsync(0, ct))
            {
                await stream.WriteAsync(new byte[] { 0 }, ct);
                return;
            }
            try
            {
                var marker = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16));
                var pin = RandomNumberGenerator.GetInt32(10000).ToString("D4", CultureInfo.InvariantCulture);
                var reply = Encoding.ASCII.GetBytes("\u0001" + marker + pin);
                await stream.WriteAsync(reply, ct);
                using var disconnected = CancellationTokenSource.CreateLinkedTokenSource(ct);
                var read = DetectDisconnect(stream, disconnected);
                try
                {
                    var active = disconnected.Token;
                    SunshinePairing pending;
                    do
                    {
                        pending = FindPending(await admin.Pending(active), source.Address, marker);
                        if (pending == null) await Task.Delay(250, active);
                    } while (pending == null);
                    var rechecked = await Task.Run(Upstream.ReadNetwork, active).WaitAsync(active);
                    if (FindAuthorizedPeer(rechecked, source.Address)?.Id != peer.Id) return;
                    var label = new string(peer.Name.Where(character => !char.IsControl(character) && character != '#')
                        .Take(24).ToArray());
                    await admin.Pair(pending, pin, string.IsNullOrWhiteSpace(label) ? "Aurora" : label, active);
                }
                finally
                {
                    disconnected.Cancel();
                    await read;
                }
            }
            finally { pairing.Release(); }
        }

        private Device FindAuthorizedPeer(NetworkSnapshot network, IPAddress source) =>
            Matches(network) &&
            IsConfiguredFor(network, admin) ? network.Devices.SingleOrDefault(peer => peer.Address.Equals(source)) : null;

        private static async Task DetectDisconnect(Stream stream, CancellationTokenSource attempt)
        {
            try { await stream.ReadAtLeastAsync(new byte[1], 1, false, attempt.Token); }
            catch (Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
            finally { attempt.Cancel(); }
        }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            shutdown.Cancel();
            listener.Stop();
            relayJob?.Dispose();
            Upstream.StopProcess(relay);
            if (lifetime == null) Release();
            else _ = lifetime.ContinueWith(_ => Release(), TaskScheduler.Default);
        }

        private void Release()
        {
            relay?.Dispose();
            pairing.Dispose();
            shutdown.Dispose();
        }
    }
}
