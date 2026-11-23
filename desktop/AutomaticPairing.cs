using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.Versioning;
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
    private const int BridgeKeySize = 32;
    private static readonly byte[] Request = "AURORA/PAIR/1\n"u8.ToArray();
    private const string SettingsKey = @"Software\Aurora\AutomaticPairing";
    private const string SettingsValue = "Account";
    private const string ApprovalsKey = SettingsKey + @"\ApprovedAccounts";
    private const UnixFileMode PrivateFile = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private const UnixFileMode PrivateDirectory = PrivateFile | UnixFileMode.UserExecute;

    public static bool IsConfigured
    {
        get
        {
            if (OperatingSystem.IsMacOS())
            {
                try { return HasMacApprovals(); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    Debug.WriteLine(error);
                    return false;
                }
            }
            if (!OperatingSystem.IsWindows()) return false;
            using var previous = Registry.CurrentUser.OpenSubKey(SettingsKey);
            using var approvals = Registry.CurrentUser.OpenSubKey(ApprovalsKey);
            return previous?.GetValue(SettingsValue) is string || approvals?.ValueCount > 0;
        }
    }

    private static string Scope(NetworkSnapshot network, SunshineAdmin admin) =>
        FormattableString.Invariant($"{network.Owner}/{network.LocalId}/{admin.CredentialScope}");

    private static string ApprovalId(NetworkSnapshot network, SunshineAdmin admin) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes($"{network.Mode}/{Scope(network, admin)}")));

    internal static bool SupportsHost(NetworkSnapshot network, SunshineAdmin admin) => network?.Mode switch
    {
        TailscaleMode.System => OperatingSystem.IsWindows(),
        TailscaleMode.Embedded => admin?.IsManaged == true && (OperatingSystem.IsWindows() || OperatingSystem.IsMacOS()),
        _ => false
    };

    public static bool IsConfiguredFor(NetworkSnapshot network, SunshineAdmin admin)
    {
        if (network?.IsAuthenticated != true || !SupportsHost(network, admin) || string.IsNullOrEmpty(admin?.CredentialScope))
            return false;
        if (OperatingSystem.IsMacOS()) return IsMacApproved(MacApprovalsDirectory(), ApprovalId(network, admin));
        if (!OperatingSystem.IsWindows()) return false;
        using var approvals = Registry.CurrentUser.OpenSubKey(ApprovalsKey);
        if (approvals?.GetValue(ApprovalId(network, admin)) is 1) return true;
        using var previous = Registry.CurrentUser.OpenSubKey(SettingsKey);
        return string.Equals(previous?.GetValue(SettingsValue) as string, Scope(network, admin), StringComparison.Ordinal);
    }

    internal static bool EnsureEnrollment(NetworkSnapshot network, SunshineAdmin admin, bool reauthenticated)
    {
        if (network?.IsAuthenticated != true || !SupportsHost(network, admin) || string.IsNullOrEmpty(admin?.CredentialScope))
            return false;
        if (IsConfiguredFor(network, admin)) return true;
        if (!admin.IsManaged) return false;
        if (OperatingSystem.IsMacOS())
        {
            if (!reauthenticated && HasMacApprovals()) return false;
            return SaveMacApproval(ApprovalId(network, admin));
        }
        if (!OperatingSystem.IsWindows() || !reauthenticated && IsConfigured) return false;
        using var approvals = Registry.CurrentUser.CreateSubKey(ApprovalsKey);
        approvals.SetValue(ApprovalId(network, admin), 1, RegistryValueKind.DWord);
        return true;
    }

    [SupportedOSPlatform("macos")]
    private static DirectoryInfo MacApprovalsDirectory(bool create = false)
    {
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData,
            Environment.SpecialFolderOption.DoNotVerify);
        if (string.IsNullOrEmpty(appData)) throw new IOException("找不到 Aurora 的用户设置目录。");
        var path = Path.Combine(appData, "Aurora", "AutomaticPairing", "ApprovedAccounts");
        var directory = create ? Directory.CreateDirectory(path, PrivateDirectory) : new DirectoryInfo(path);
        if (!directory.Exists) return null;
        if (directory.LinkTarget != null || (File.GetUnixFileMode(path) & ~PrivateDirectory) != 0)
            throw new IOException("Aurora 的自动配对授权目录不是私有目录。");
        return directory;
    }

    [SupportedOSPlatform("macos")]
    private static bool HasMacApprovals() => MacApprovalsDirectory()?.EnumerateFiles("*.approved")
        .Any(IsPrivateApproval) == true;

    [SupportedOSPlatform("macos")]
    private static bool IsPrivateApproval(FileInfo file) => file.Exists && file.LinkTarget == null && file.Length == 0 &&
        (File.GetUnixFileMode(file.FullName) & ~PrivateFile) == 0;

    [SupportedOSPlatform("macos")]
    private static bool IsMacApproved(DirectoryInfo directory, string id) => directory != null &&
        IsPrivateApproval(new FileInfo(Path.Combine(directory.FullName, id + ".approved")));

    [SupportedOSPlatform("macos")]
    private static bool SaveMacApproval(string id)
    {
        var directory = MacApprovalsDirectory(create: true);
        var path = Path.Combine(directory.FullName, id + ".approved");
        try
        {
            using var approval = new FileStream(path, new FileStreamOptions
            {
                Mode = FileMode.CreateNew, Access = FileAccess.Write, UnixCreateMode = PrivateFile
            });
            return true;
        }
        catch (IOException) when (IsMacApproved(directory, id)) { return true; }
    }

    internal static bool IsBackgroundPairing(string name) => name.Length == 39 &&
        name.StartsWith("aurora-", StringComparison.Ordinal) && name.Skip(7).All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');

    public static async Task Pair(Device device, string moonlight, CancellationToken token)
    {
        var route = device.Tailnet ?? throw new InvalidOperationException("首次自动配对需要两台设备登录同一 Tailscale 账号。");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(38));
        var current = await Task.Run(Upstream.ReadNetwork, timeout.Token).WaitAsync(timeout.Token);
        var peer = Discovery.FindTailnetPeer(current, route.Endpoint.Address);
        if (peer?.Id != device.Id || route.ViaEmbeddedTailnet != (current.Mode == TailscaleMode.Embedded))
            throw new InvalidOperationException("设备的 Tailscale 身份已变化，请刷新设备列表。");

        using var stream = await EmbeddedTailnet.OpenStream(route, timeout.Token);
        if (route.ViaEmbeddedTailnet)
        {
            await PairEmbedded(peer.Id, route.Endpoint.Address, moonlight, stream.Route, timeout, token);
            return;
        }
        using var relay = Upstream.StartTailscale(current, "nc", route.Endpoint.Address.ToString(), Port.ToString(CultureInfo.InvariantCulture));
        try { await ExchangePairing(relay.StandardInput.BaseStream, relay.StandardOutput.BaseStream, moonlight, stream.Route, timeout, token); }
        finally { Upstream.StopProcess(relay); }
    }

    private static async Task PairEmbedded(string peerId, IPAddress address, string moonlight, StreamHost streamRoute,
        CancellationTokenSource timeout, CancellationToken token)
    {
        var key = RandomNumberGenerator.GetBytes(BridgeKeySize);
        string leaseId = null;
        try
        {
            timeout.Token.ThrowIfCancellationRequested();
            // Consume the allocation reply even if the caller cancels, so the lease can be closed.
            var opened = await EmbeddedTailnet.Request("pair-open", new { peerId, address = address.ToString(), key });
            leaseId = opened.GetProperty("leaseId").GetString();
            timeout.Token.ThrowIfCancellationRequested();
            using var client = new TcpClient(AddressFamily.InterNetwork);
            await client.ConnectAsync(IPAddress.Loopback, opened.GetProperty("localPort").GetInt32(), timeout.Token);
            using var channel = client.GetStream();
            using (var hello = CancellationTokenSource.CreateLinkedTokenSource(timeout.Token))
            {
                hello.CancelAfter(TimeSpan.FromSeconds(3));
                var challenge = new byte[BridgeKeySize];
                await channel.ReadExactlyAsync(challenge, hello.Token);
                await channel.WriteAsync(BridgeProof("AURORA/PAIR/DIAL/1"u8, key, challenge), hello.Token);
            }
            await ExchangePairing(channel, channel, moonlight, streamRoute, timeout, token);
        }
        finally
        {
            CryptographicOperations.ZeroMemory(key);
            if (leaseId != null) await CloseEmbeddedLease(leaseId);
        }
    }

    private static async Task ExchangePairing(Stream write, Stream read, string moonlight, StreamHost streamRoute,
        CancellationTokenSource timeout, CancellationToken token)
    {
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
                throw new InvalidOperationException("无法联系对端 Aurora。请确认被控电脑的 Aurora 正在运行，且已允许同账号设备连接。");
            }
        }
        if (reply[0] != 1 || !reply.Skip(1).Take(32).All(IsLowerHex) || !reply.Skip(33).All(IsDigit))
            throw new InvalidOperationException("对端 Aurora 的配对协议不兼容。");
        var marker = Encoding.ASCII.GetString(reply, 1, 32);
        var pin = Encoding.ASCII.GetString(reply, 33, 4);
        try
        {
            var result = await Upstream.RunMoonlight(moonlight, new[]
            {
                "pair", "--aurora-background", "--aurora-pair-id", marker, "--pin", pin, streamRoute.Endpoint.ToString()
            }, timeout.Token, streamRoute);
            if (result.ExitCode != 0) throw new InvalidOperationException("串流组件未完成后台配对，请检查被控端状态后重试。");
            // The caller independently checks Moonlight's persisted trust before starting a stream.
        }
        finally { timeout.Cancel(); }
    }

    private static byte[] BridgeProof(ReadOnlySpan<byte> purpose, byte[] key, byte[] challenge, ReadOnlySpan<byte> payload = default)
    {
        Span<byte> message = stackalloc byte[purpose.Length + challenge.Length + payload.Length];
        purpose.CopyTo(message);
        challenge.CopyTo(message[purpose.Length..]);
        payload.CopyTo(message[(purpose.Length + challenge.Length)..]);
        return HMACSHA256.HashData(key, message);
    }

    private static async Task CloseEmbeddedLease(string leaseId)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        try { await EmbeddedTailnet.Request("close", new { leaseId }, timeout.Token); }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException or TimeoutException)
        {
            Debug.WriteLine(error);
        }
    }

    private static bool IsLowerHex(byte value) => IsDigit(value) || value is >= (byte)'a' and <= (byte)'f';
    private static bool IsDigit(byte value) => value is >= (byte)'0' and <= (byte)'9';

    internal static SunshinePairing FindPending(SunshinePairing[] requests, IPAddress source, string marker)
    {
        var expected = Discovery.NormalizeAddress(source);
        var matches = requests.Where(item => item.Name == "aurora-" + marker &&
            IPAddress.TryParse(item.Address, out var address) && Discovery.NormalizeAddress(address).Equals(expected))
            .Take(2).ToArray();
        return matches.Length == 1 ? matches[0] : null;
    }

    internal static SunshinePairing FindEmbeddedPending(SunshinePairing[] requests, IPAddress sunshineSource, string marker)
    {
        if (!IPAddress.IsLoopback(Discovery.NormalizeAddress(sunshineSource))) return null;
        var pending = FindPending(requests, sunshineSource, marker);
        return pending?.SourcePort is > 0 and <= IPEndPoint.MaxPort ? pending : null;
    }

    private static async Task<bool> VerifyEmbeddedSource(NetworkSnapshot network, string peerId, IPAddress address,
        int sourcePort, CancellationToken token)
    {
        var result = await EmbeddedTailnet.Request("pair-source", new
        {
            peerId, address = address.ToString(), sourcePort, owner = network.Owner, nodeId = network.LocalId
        }, token);
        return result.ValueKind == JsonValueKind.Object && result.TryGetProperty("verified", out var verified) &&
            verified.ValueKind == JsonValueKind.True;
    }

    private static async Task CheckPort(NetworkSnapshot network, CancellationToken token)
    {
        using var status = Upstream.StartTailscale(network, "serve", "status", "--json");
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
        return new IPEndPoint(Discovery.NormalizeAddress(new IPAddress(payload.AsSpan(0, addressLength))),
            BinaryPrimitives.ReadUInt16BigEndian(payload.AsSpan(addressLength * 2)));
    }

    // The private bridge proves the actual tsnet socket peer and the Sunshine-side address.
    // Approval also requires the exact live GameStream relay source port.
    // The nonce is local to this accepted connection, so an assertion cannot be replayed on another.
    internal static async Task<(IPAddress Peer, IPAddress SunshineSource)> ReadEmbeddedSources(Stream stream, byte[] key,
        CancellationToken token)
    {
        var challenge = RandomNumberGenerator.GetBytes(BridgeKeySize);
        await stream.WriteAsync(challenge, token);
        var family = new byte[1];
        await stream.ReadExactlyAsync(family, token);
        var peerLength = family[0] switch { 4 => 4, 6 => 16, _ => throw new InvalidDataException() };
        var payload = new byte[1 + peerLength + 4];
        payload[0] = family[0];
        await stream.ReadExactlyAsync(payload.AsMemory(1), token);
        var proof = new byte[BridgeKeySize];
        await stream.ReadExactlyAsync(proof, token);
        if (!CryptographicOperations.FixedTimeEquals(proof, BridgeProof("AURORA/PAIR/HOST/1"u8, key, challenge, payload)))
            throw new InvalidDataException();
        var peer = Discovery.NormalizeAddress(new IPAddress(payload.AsSpan(1, peerLength)));
        var sunshine = new IPAddress(payload.AsSpan(1 + peerLength, 4));
        if (!IPAddress.IsLoopback(sunshine) || IPAddress.IsLoopback(peer)) throw new InvalidDataException();
        return (peer, sunshine);
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
        private byte[] bridgeKey;
        private string embeddedLease;
        private Task starting;
        private Task lifetime;
        private bool disposed;

        public Host(SunshineAdmin admin, NetworkSnapshot network)
        {
            this.admin = admin;
            enabledNetwork = network;
        }

        public Task Start() => starting = StartCore();

        private async Task StartCore()
        {
            if (!SupportsHost(enabledNetwork, admin) || !IsConfiguredFor(enabledNetwork, admin))
                throw new InvalidOperationException("当前连接方式未获准启动自动配对。");
            await admin.Pending(shutdown.Token); // Fail early on incompatible Sunshine versions.
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            var embedded = enabledNetwork.Mode == TailscaleMode.Embedded;
            if (!embedded) await CheckPort(enabledNetwork, timeout.Token);
            timeout.Token.ThrowIfCancellationRequested();
            listener.Server.ExclusiveAddressUse = true;
            listener.Start(8);
            var local = (IPEndPoint)listener.LocalEndpoint;
            if (embedded)
            {
                bridgeKey = RandomNumberGenerator.GetBytes(BridgeKeySize);
                var opened = await EmbeddedTailnet.Request("pair-host", new
                {
                    localPort = local.Port, key = bridgeKey,
                    owner = enabledNetwork.Owner, nodeId = enabledNetwork.LocalId
                });
                embeddedLease = opened.GetProperty("leaseId").GetString();
                timeout.Token.ThrowIfCancellationRequested();
                lifetime = AcceptWorkers(shutdown.Token);
                return;
            }
            relay = Upstream.StartTailscale(enabledNetwork, "serve", "--tcp=" + Port, "--proxy-protocol=2", "tcp://" + local);
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
            network.Mode == enabledNetwork.Mode && network.Generation == enabledNetwork.Generation &&
            network.LocalAddress?.Equals(enabledNetwork.LocalAddress) == true;

        private async Task Listen(Process process, CancellationToken token)
        {
            var drained = process.StandardOutput.ReadToEndAsync(token);
            var exit = process.WaitForExitAsync(token);
            var workers = AcceptWorkers(token);
            try { await Task.WhenAny(exit, workers); }
            finally
            {
                shutdown.Cancel();
                listener.Stop();
                try { await workers; }
                finally
                {
                    try { await drained; }
                    catch (OperationCanceledException) { }
                }
            }
        }

        private Task AcceptWorkers(CancellationToken token) => Task.WhenAll(Enumerable.Range(0, 4).Select(_ => Accept(token)));

        private async Task Accept(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    using var client = await listener.AcceptTcpClientAsync(token);
                    await Handle(client, token);
                }
                catch (Exception error) when (error is IOException or SocketException or OperationCanceledException or InvalidDataException or
                    ObjectDisposedException or InvalidOperationException or FormatException or JsonException or TimeoutException or
                    UnauthorizedAccessException)
                {
                    if (token.IsCancellationRequested) return;
                }
            }
        }

        private async Task Handle(TcpClient client, CancellationToken token)
        {
            var embedded = enabledNetwork.Mode == TailscaleMode.Embedded;
            if (client.Client.RemoteEndPoint is not IPEndPoint socketSource || client.Client.LocalEndPoint is not IPEndPoint socketTarget ||
                (embedded ? !IPAddress.IsLoopback(socketSource.Address) :
                    !await TailscaleLoopback.IsFromService(enabledNetwork.Mode, socketSource, socketTarget, token))) return;
            using var stream = client.GetStream();
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(token);
            attempt.CancelAfter(TimeSpan.FromSeconds(31));
            var ct = attempt.Token;
            IPAddress source;
            IPAddress sunshineSource;
            using (var hello = CancellationTokenSource.CreateLinkedTokenSource(ct))
            {
                hello.CancelAfter(TimeSpan.FromSeconds(3));
                if (embedded) (source, sunshineSource) = await ReadEmbeddedSources(stream, bridgeKey, hello.Token);
                else source = sunshineSource = (await ReadProxySource(stream, hello.Token)).Address;
                var request = new byte[Request.Length];
                await stream.ReadExactlyAsync(request, hello.Token);
                if (!request.SequenceEqual(Request)) return;
            }
            var local = await Task.Run(Upstream.ReadNetwork, ct).WaitAsync(ct);
            if (!Matches(local)) return;
            var peer = FindAuthorizedPeer(local, source);
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
                        var requests = await admin.Pending(active);
                        pending = embedded ? FindEmbeddedPending(requests, sunshineSource, marker) :
                            FindPending(requests, sunshineSource, marker);
                        if (pending == null) await Task.Delay(250, active);
                    } while (pending == null);
                    var rechecked = await Task.Run(Upstream.ReadNetwork, active).WaitAsync(active);
                    if (FindAuthorizedPeer(rechecked, source)?.Id != peer.Id) return;
                    if (embedded)
                    {
                        // Refresh the immutable request ID and socket data over pinned Sunshine HTTPS before Go's live check.
                        var confirmed = FindEmbeddedPending(await admin.Pending(active), sunshineSource, marker);
                        if (confirmed != pending || !await VerifyEmbeddedSource(rechecked, peer.Id, source, pending.SourcePort.Value, active))
                            return;
                    }
                    if (TailscaleRuntime.IsTransitioning || TailscaleRuntime.Mode != rechecked.Mode ||
                        TailscaleRuntime.Generation != rechecked.Generation) return;
                    active.ThrowIfCancellationRequested();
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
            IsConfiguredFor(network, admin) ? Discovery.FindTailnetPeer(network, source) : null;

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
            _ = Release();
        }

        private async Task Release()
        {
            try
            {
                if (starting != null) await starting;
                if (lifetime != null) await lifetime;
            }
            catch (Exception error) { Debug.WriteLine(error); }
            finally
            {
                if (embeddedLease != null) await CloseEmbeddedLease(embeddedLease);
                if (bridgeKey != null) CryptographicOperations.ZeroMemory(bridgeKey);
                relay?.Dispose();
                pairing.Dispose();
                shutdown.Dispose();
            }
        }
    }
}
