using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Aurora;

internal sealed record PresentDevice(Guid Id, IPAddress Address, string Name, string Platform, bool Hosting);
internal sealed record LanSubnet(IPAddress Address, IPAddress Mask)
{
    internal IPAddress BroadcastAddress
    {
        get
        {
            var address = Address.GetAddressBytes();
            var mask = Mask.GetAddressBytes();
            for (var i = 0; i < address.Length; i++) address[i] |= (byte)~mask[i];
            return new IPAddress(address);
        }
    }
}

internal sealed class AuroraPresence : IDisposable
{
    internal const int Port = 48988;
    // Requests and responses have equal sizes. Presence advertises no credentials or control endpoints.
    private const int PacketSize = 192;
    private const int KindOffset = 17;
    private const int NonceOffset = KindOffset + 1;
    private const int IdOffset = NonceOffset + 16;
    private const int PlatformOffset = IdOffset + 16;
    private const int HostingOffset = PlatformOffset + 1;
    private const int NameOffset = HostingOffset + 1;
    private static ReadOnlySpan<byte> Magic => "AURORA/PRESENCE/1"u8;
    private readonly Guid identity;
    private readonly string dataDirectory;
    private readonly UdpClient ipv4;
    private readonly UdpClient ipv6;
    private volatile LanSubnet[] subnets = Array.Empty<LanSubnet>();
    private volatile HashSet<IPAddress> tailnet = new();

    internal AuroraPresence(string dataDirectory = null)
    {
        var directory = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData,
            Environment.SpecialFolderOption.DoNotVerify);
        this.dataDirectory = dataDirectory ?? (directory.Length == 0 ? null : Path.Combine(directory, "Aurora"));
        identity = GetIdentity();
        ipv4 = Listen(IPAddress.Any);
        if (Socket.OSSupportsIPv6) ipv6 = Listen(IPAddress.IPv6Any);
    }

    internal void UpdateLan(LanSubnet[] links) => subnets = links;

    internal void UpdateTailnet(NetworkSnapshot network) =>
        tailnet = network?.IsAuthenticated == true && network.Mode == TailscaleMode.System
            ? network.Devices.SelectMany(Discovery.TailnetAddresses).Select(Discovery.NormalizeAddress).ToHashSet() : new();

    internal async Task<PresentDevice[]> FindLan(LanSubnet[] links, CancellationToken token)
    {
        var results = await Task.WhenAll(links.Select(link => Scan(link.Address, new[] { link.BroadcastAddress },
            candidate => Discovery.SameSubnet(link.Address, link.Mask, candidate), broadcast: true, token)));
        return results.SelectMany(result => result).DistinctBy(peer => peer.Address).ToArray();
    }

    internal async Task<PresentDevice[]> FindPeers(IEnumerable<IPAddress> addresses, CancellationToken token)
    {
        var checks = await Task.WhenAll(addresses.Distinct().GroupBy(address => address.AddressFamily)
            .Select(group =>
            {
                var targets = group.ToHashSet();
                var any = group.Key == AddressFamily.InterNetwork ? IPAddress.Any : IPAddress.IPv6Any;
                return Scan(any, targets, targets.Contains, broadcast: false, token);
            }));
        return checks.SelectMany(result => result).ToArray();
    }

    internal async Task<PresentDevice[]> FindEmbeddedPeers(NetworkSnapshot network, IEnumerable<IPAddress> addresses,
        CancellationToken token)
    {
        if (network?.Mode != TailscaleMode.Embedded || TailscaleRuntime.Mode != network.Mode ||
            TailscaleRuntime.Generation != network.Generation || TailscaleRuntime.IsTransitioning) return Array.Empty<PresentDevice>();
        var found = new ConcurrentBag<PresentDevice>();
        await Parallel.ForEachAsync(addresses.Distinct(), new ParallelOptions { MaxDegreeOfParallelism = 8, CancellationToken = token },
            async (address, cancellation) =>
            {
                var peer = Discovery.FindTailnetPeer(network, address);
                if (peer == null) return;
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
                timeout.CancelAfter(TimeSpan.FromMilliseconds(900));
                try
                {
                    var request = CreateRequest();
                    // The bridge exchanges on a connected tsnet UDP socket to this exact endpoint.
                    var reply = await EmbeddedTailnet.Request("presence", new { peerId = peer.Id, address = address.ToString(),
                        packet = request }, timeout.Token).ConfigureAwait(false);
                    var packet = reply.ValueKind == JsonValueKind.String ? reply.GetBytesFromBase64() : null;
                    var device = ReadResponse(packet, request, address);
                    if (device != null) found.Add(device);
                }
                catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { }
                catch (Exception error) when (error is SocketException or IOException or InvalidOperationException or FormatException)
                {
                    Debug.WriteLine(error);
                }
            }).ConfigureAwait(false);
        return found.ToArray();
    }

    // The node validates the tailnet sender and copies its nonce into this template.
    internal byte[] EmbeddedAnnouncement() => FillReply(new byte[PacketSize]);

    private async Task<PresentDevice[]> Scan(IPAddress source, IReadOnlyCollection<IPAddress> targets,
        Func<IPAddress, bool> accept, bool broadcast, CancellationToken token)
    {
        var found = new Dictionary<IPAddress, PresentDevice>();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromMilliseconds(900));
        try
        {
            var request = CreateRequest();
            using var client = new UdpClient(new IPEndPoint(source, 0));
            if (broadcast) client.EnableBroadcast = true;
            foreach (var target in targets)
            {
                try { await client.SendAsync(request, new IPEndPoint(target, Port), timeout.Token).ConfigureAwait(false); }
                catch (SocketException error) { Debug.WriteLine(error); }
            }
            while (found.Count < (broadcast ? 512 : targets.Count))
            {
                UdpReceiveResult result;
                try { result = await client.ReceiveAsync(timeout.Token).ConfigureAwait(false); }
                catch (SocketException error) when (error.SocketErrorCode == SocketError.ConnectionReset) { continue; }
                var address = Discovery.NormalizeAddress(result.RemoteEndPoint.Address);
                if (result.RemoteEndPoint.Port != Port || !accept(address)) continue;
                var device = ReadResponse(result.Buffer, request, address);
                if (device != null) found[address] = device;
            }
        }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested) { }
        catch (SocketException error) { Debug.WriteLine(error); }
        return found.Values.ToArray();
    }

    private UdpClient Listen(IPAddress address)
    {
        UdpClient client = null;
        try
        {
            client = new UdpClient(address.AddressFamily);
            client.ExclusiveAddressUse = true;
            if (address.AddressFamily == AddressFamily.InterNetworkV6) client.Client.DualMode = false;
            client.Client.Bind(new IPEndPoint(address, Port));
            _ = Reply(client);
            return client;
        }
        catch (SocketException error)
        {
            client?.Dispose();
            Debug.WriteLine(error);
            return null;
        }
    }

    private async Task Reply(UdpClient client)
    {
        try
        {
            while (true)
            {
                UdpReceiveResult result;
                try { result = await client.ReceiveAsync().ConfigureAwait(false); }
                catch (SocketException error) when (error.SocketErrorCode == SocketError.ConnectionReset) { continue; }
                var source = Discovery.NormalizeAddress(result.RemoteEndPoint.Address);
                if (!(tailnet.Contains(source) || subnets.Any(link => Discovery.SameSubnet(link.Address, link.Mask, source))))
                    continue;
                var packet = CreateReply(result.Buffer);
                if (packet == null) continue;
                try { await client.SendAsync(packet, result.RemoteEndPoint).ConfigureAwait(false); }
                catch (SocketException error) { Debug.WriteLine(error); }
            }
        }
        catch (ObjectDisposedException) { }
        catch (SocketException error) { Debug.WriteLine(error); }
    }

    private static bool Matches(byte[] packet, byte kind) =>
        packet?.Length == PacketSize && packet.AsSpan(0, KindOffset).SequenceEqual(Magic) && packet[KindOffset] == kind;

    private static byte[] CreateRequest()
    {
        var request = new byte[PacketSize];
        Magic.CopyTo(request);
        request[KindOffset] = 1;
        RandomNumberGenerator.Fill(request.AsSpan(NonceOffset, 16));
        return request;
    }

    private byte[] CreateReply(byte[] request) => Matches(request, 1) ? FillReply((byte[])request.Clone()) : null;

    private byte[] FillReply(byte[] packet)
    {
        Magic.CopyTo(packet);
        packet[KindOffset] = 2;
        identity.TryWriteBytes(packet.AsSpan(IdOffset, 16));
        packet[PlatformOffset] = OperatingSystem.IsWindows() ? (byte)1 : OperatingSystem.IsMacOS() ? (byte)2 : (byte)0;
        packet[HostingOffset] = BundledSunshine.IsRunning ? (byte)1 : (byte)0;
        var name = packet.AsSpan(NameOffset);
        name.Clear();
        Encoding.UTF8.GetEncoder().Convert(Environment.MachineName.AsSpan(), name, true, out _, out _, out _);
        return packet;
    }

    private PresentDevice ReadResponse(byte[] packet, byte[] request, IPAddress address)
    {
        if (!Matches(packet, 2) || !packet.AsSpan(NonceOffset, 16).SequenceEqual(request.AsSpan(NonceOffset, 16))) return null;
        var id = new Guid(packet.AsSpan(IdOffset, 16));
        if (id == identity || id == Guid.Empty) return null;
        var name = Encoding.UTF8.GetString(packet.AsSpan(NameOffset)).TrimEnd('\0').Trim();
        var platform = packet[PlatformOffset] switch { 1 => "windows", 2 => "macOS", _ => "" };
        return new(id, address, name.Length > 0 && !name.Any(char.IsControl) ? name : address.ToString(),
            platform, packet[HostingOffset] == 1);
    }

    internal IReadOnlyDictionary<string, Guid> Remember(NetworkSnapshot network, IReadOnlyList<PresentDevice> present)
    {
        var known = new Dictionary<string, Guid>(StringComparer.Ordinal);
        var live = present.ToDictionary(peer => peer.Address);
        var account = Digest(FormattableString.Invariant($"{network.Owner}/{network.LocalId}"));
        foreach (var peer in network.Devices)
        {
            var path = dataDirectory == null || string.IsNullOrEmpty(network.LocalId) ? null
                : Path.Combine(dataDirectory, "KnownDevices", account, Digest(peer.Id));
            var saved = ReadIdentity(path);
            var identities = Discovery.TailnetAddresses(peer).Select(address => live.GetValueOrDefault(address))
                .Where(device => device != null).Select(device => device.Id).Distinct().Take(2).ToArray();
            if (identities.Length == 1)
            {
                known[peer.Id] = identities[0];
                if (path != null && saved != identities[0]) WriteIdentity(path, identities[0], overwrite: true);
            }
            else if (identities.Length == 0 && saved != Guid.Empty) known[peer.Id] = saved;
        }
        return known;
    }

    private Guid GetIdentity()
    {
        var id = Guid.NewGuid();
        if (dataDirectory == null) return id;
        var path = Path.Combine(dataDirectory, "presence-id");
        var existing = ReadIdentity(path);
        if (existing != Guid.Empty) return existing;
        WriteIdentity(path, id, overwrite: false);
        existing = ReadIdentity(path);
        return existing == Guid.Empty ? id : existing;
    }

    private static Guid ReadIdentity(string path)
    {
        if (path == null) return Guid.Empty;
        try
        {
            return File.Exists(path) && new FileInfo(path).Length == 32 &&
                Guid.TryParseExact(File.ReadAllText(path), "N", out var identity) ? identity : Guid.Empty;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return Guid.Empty; }
    }

    private static void WriteIdentity(string path, Guid id, bool overwrite)
    {
        var directory = Path.GetDirectoryName(path);
        var temp = path + "." + Guid.NewGuid().ToString("N");
        try
        {
            if (OperatingSystem.IsMacOS())
                Directory.CreateDirectory(directory, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            else Directory.CreateDirectory(directory);
            File.WriteAllText(temp, id.ToString("N"));
            File.Move(temp, path, overwrite);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Debug.WriteLine(error); }
        finally
        {
            try { File.Delete(temp); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Debug.WriteLine(error); }
        }
    }

    private static string Digest(string text) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)));

    public void Dispose()
    {
        ipv4?.Dispose();
        ipv6?.Dispose();
    }
}
