using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;
using System.Xml.Linq;
using Zeroconf;

namespace Aurora;

internal sealed record StreamHost(IPEndPoint Endpoint, string Id, string Name)
{
    internal bool ViaEmbeddedTailnet { get; init; }
    internal bool EncoderProbePending { get; init; }
    internal bool SupportsPower { get; init; }
    internal bool SupportsSessionPrivacy { get; init; }
}
internal sealed record LanSnapshot(IPAddress Address, IReadOnlyList<StreamHost> Hosts, IReadOnlyList<PresentDevice> Devices);
internal sealed record TailnetSnapshot(IReadOnlyList<StreamHost> Hosts, IReadOnlyList<PresentDevice> Devices,
    IReadOnlyDictionary<string, Guid> Known);

internal static class Discovery
{
    private const string Service = "_nvstream._tcp.local.";
    private static readonly HttpClient Http = new(new SocketsHttpHandler
    {
        UseProxy = false, AllowAutoRedirect = false, UseCookies = false
    })
    {
        Timeout = TimeSpan.FromMilliseconds(1100), MaxResponseContentBufferSize = 32768
    };

    public static async Task<LanSnapshot> ReadLan(AuroraPresence presence, CancellationToken cancellation, bool scan = true)
    {
        var links = NetworkInterface.GetAllNetworkInterfaces()
            .Where(adapter => adapter.OperationalStatus == OperationalStatus.Up &&
                adapter.NetworkInterfaceType is NetworkInterfaceType.Ethernet or NetworkInterfaceType.Wireless80211 or NetworkInterfaceType.GigabitEthernet)
            .Select(adapter => (Adapter: adapter, Properties: adapter.GetIPProperties()))
            .Select(link => (link.Adapter, Addresses: link.Properties.UnicastAddresses
                .Where(item => IsLan(item.Address) && item.IPv4Mask != null && !item.IPv4Mask.Equals(IPAddress.Any)).ToArray(),
                Gateway: link.Properties.GatewayAddresses.Any(gateway => gateway.Address.AddressFamily == AddressFamily.InterNetwork)))
            .Where(link => link.Addresses.Length != 0)
            .OrderByDescending(link => link.Gateway).ToArray();
        var local = links.SelectMany(link => link.Addresses)
            .Select(address => new LanSubnet(address.Address, address.IPv4Mask)).Distinct().ToArray();
        presence.UpdateLan(scan ? local : Array.Empty<LanSubnet>());
        if (!scan || local.Length == 0)
            return new(local.FirstOrDefault()?.Address, Array.Empty<StreamHost>(), Array.Empty<PresentDevice>());

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        var nearby = presence.FindLan(local, timeout.Token);
        // mDNS provides addresses for a direct presence check on networks that filter broadcast.
        var advertised = Array.Empty<IPAddress>();
        try
        {
            var services = await ZeroconfResolver.ResolveAsync(Service, scanTime: TimeSpan.FromMilliseconds(1200),
                retries: 1, cancellationToken: timeout.Token, netInterfacesToSendRequestOn: links.Select(link => link.Adapter).ToArray());
            advertised = services.SelectMany(item => item.IPAddresses)
                .Select(literal => IPAddress.TryParse(literal, out var address) ? address : null)
                .Where(address => address != null && !local.Any(link => link.Address.Equals(address)) &&
                    local.Any(link => SameSubnet(link.Address, link.Mask, address))).Distinct().ToArray();
        }
        catch (Exception error) when (error is SocketException or IOException or OperationCanceledException or InvalidOperationException)
        {
            Debug.WriteLine(error);
        }
        var peers = await nearby;
        var additional = await presence.FindPeers(advertised.Except(peers.Select(device => device.Address)), timeout.Token);
        peers = peers.Concat(additional).DistinctBy(device => device.Address).ToArray();
        return new(local[0].Address, await Probe(HostEndpoints(peers.Where(peer => peer.Hosting).Select(peer => peer.Address)),
            timeout.Token), peers);
    }

    private static bool IsLan(IPAddress address)
    {
        if (address.AddressFamily != AddressFamily.InterNetwork || IPAddress.IsLoopback(address)) return false;
        var bytes = address.GetAddressBytes();
        return bytes[0] is > 0 and < 224 && !(bytes[0] == 100 && (bytes[1] & 192) == 64);
    }

    internal static bool SameSubnet(IPAddress local, IPAddress mask, IPAddress remote)
    {
        if (!IsLan(remote) || local.Equals(remote)) return false;
        var source = local.GetAddressBytes();
        var netmask = mask.GetAddressBytes();
        var destination = remote.GetAddressBytes();
        return source.Length == 4 && netmask.Length == 4 &&
            Enumerable.Range(0, 4).All(index => (source[index] & netmask[index]) == (destination[index] & netmask[index])) &&
            Enumerable.Range(0, 4).Any(index => (destination[index] & ~netmask[index]) != (255 & ~netmask[index])) &&
            Enumerable.Range(0, 4).Any(index => (destination[index] & ~netmask[index]) != 0);
    }

    public static async Task<TailnetSnapshot> ReadTailnet(NetworkSnapshot network, AuroraPresence presence,
        CancellationToken cancellation)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(4));
        var embedded = network.Mode == TailscaleMode.Embedded;
        var addresses = network.Devices.Where(peer => peer.Online).SelectMany(TailnetAddresses);
        var devices = embedded
            ? await presence.FindEmbeddedPeers(network, addresses, timeout.Token).ConfigureAwait(false)
            : await presence.FindPeers(addresses, timeout.Token).ConfigureAwait(false);
        timeout.Token.ThrowIfCancellationRequested();
        var known = presence.Remember(network, devices);
        var routes = await Probe(HostEndpoints(devices.Where(peer => peer.Hosting).Select(peer => peer.Address)), timeout.Token, embedded);
        return new(routes, devices, known);
    }

    internal static IEnumerable<IPAddress> TailnetAddresses(Device peer)
    {
        if (peer.Address != null) yield return peer.Address;
        if (peer.AlternateAddress != null && !peer.AlternateAddress.Equals(peer.Address)) yield return peer.AlternateAddress;
    }

    internal static Device FindTailnetPeer(NetworkSnapshot network, IPAddress source)
    {
        if (network?.IsAuthenticated != true || source == null) return null;
        var address = NormalizeAddress(source);
        if (IPAddress.IsLoopback(address) || address.Equals(IPAddress.Any) || address.Equals(IPAddress.IPv6Any) ||
            address.Equals(NormalizeAddress(network.LocalAddress))) return null;
        var peers = network.Devices.Where(peer => TailnetAddresses(peer).Any(candidate => NormalizeAddress(candidate).Equals(address)))
            .Take(2).ToArray();
        return peers.Length == 1 ? peers[0] : null;
    }

    internal static IPAddress NormalizeAddress(IPAddress address) =>
        address.IsIPv4MappedToIPv6 ? address.MapToIPv4() : address;

    private static IEnumerable<IPEndPoint> HostEndpoints(IEnumerable<IPAddress> addresses) =>
        addresses.Select(address => new IPEndPoint(address, BundledSunshine.BasePort));

    private static async Task<IReadOnlyList<StreamHost>> Probe(IEnumerable<IPEndPoint> endpoints, CancellationToken token,
        bool embedded = false)
    {
        var found = new System.Collections.Concurrent.ConcurrentBag<StreamHost>();
        try
        {
            await Parallel.ForEachAsync(endpoints.Distinct(), new ParallelOptions { MaxDegreeOfParallelism = 8, CancellationToken = token },
                async (endpoint, cancellation) =>
                {
                    var host = await ReadHost(endpoint, cancellation, embedded);
                    if (host != null) found.Add(host);
                });
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        return found.OrderBy(host => host.Endpoint.ToString(), StringComparer.Ordinal).ToArray();
    }

    private static async Task<StreamHost> ReadHost(IPEndPoint endpoint, CancellationToken token, bool embedded,
        IPEndPoint target = null)
    {
        try
        {
            using var bridge = embedded && target == null
                ? await EmbeddedTailnet.OpenStream(new(endpoint, "", "") { ViaEmbeddedTailnet = true }, token) : null;
            target ??= bridge?.Route.Endpoint ?? endpoint;
            var uri = new UriBuilder("http", target.Address.ToString(), target.Port, "/serverinfo").Uri;
            using var request = new HttpRequestMessage(HttpMethod.Get, uri);
            if (embedded) request.Headers.ConnectionClose = true;
            using var response = await Http.SendAsync(request, token);
            if (!response.IsSuccessStatusCode) return null;
            var host = ParseHost(endpoint, await response.Content.ReadAsStringAsync(token));
            return host == null ? null : host with { ViaEmbeddedTailnet = embedded };
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException or XmlException or IOException or
            SocketException || embedded && error is InvalidOperationException or TimeoutException)
        {
            return null;
        }
    }

    internal static StreamHost ParseHost(IPEndPoint endpoint, string xml)
    {
        using var input = new StringReader(xml);
        using var reader = XmlReader.Create(input, new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, MaxCharactersInDocument = 32768 });
        var root = XDocument.Load(reader).Root;
        if (root?.Name != "root" || (string)root.Attribute("status_code") != "200" ||
            root.Element("PairStatus") == null || root.Element("appversion") == null) return null;
        var id = ((string)root.Element("uniqueid"))?.Trim();
        var name = ((string)root.Element("hostname"))?.Trim();
        return string.IsNullOrWhiteSpace(id) || id.Length > 128 || string.IsNullOrWhiteSpace(name)
            ? null : new(endpoint, id, name)
            {
                EncoderProbePending = (string)root.Element("AuroraEncoderStatus") == "probing",
                SupportsPower = (string)root.Element("AuroraPower") == "1",
                SupportsSessionPrivacy = (string)root.Element("AuroraSessionPrivacy") == "1"
            };
    }

    internal static List<Device> Merge(IEnumerable<Device> peers, LanSnapshot lan, TailnetSnapshot tailnet)
    {
        var lanRoutes = StreamRoutes(lan?.Hosts);
        var tailnetRoutes = StreamRoutes(tailnet?.Hosts);
        var local = (lan?.Devices ?? Array.Empty<PresentDevice>()).GroupBy(device => device.Id)
            .ToDictionary(group => group.Key, group => group.OrderByDescending(device => lanRoutes.ContainsKey(device.Address)).First());
        var liveTailnet = (tailnet?.Devices ?? Array.Empty<PresentDevice>()).Select(peer => peer.Address).ToHashSet();
        var devices = new List<Device>();
        foreach (var peer in peers)
        {
            if (tailnet == null || !tailnet.Known.TryGetValue(peer.Id, out var identity)) continue;
            var addresses = TailnetAddresses(peer).ToArray();
            var streamer = addresses.Select(address => tailnetRoutes.GetValueOrDefault(address)).FirstOrDefault(route => route != null);
            StreamHost near = null;
            IPAddress lanAddress = null;
            if (local.TryGetValue(identity, out var nearby))
            {
                lanRoutes.TryGetValue(nearby.Address, out var route);
                if (streamer == null || route == null || string.Equals(streamer.Id, route.Id, StringComparison.OrdinalIgnoreCase))
                {
                    near = route;
                    lanAddress = nearby.Address;
                    local.Remove(identity);
                }
            }
            devices.Add(new Device
            {
                Id = peer.Id, Name = peer.Name, Platform = peer.Platform, Address = peer.Address, AlternateAddress = peer.AlternateAddress,
                Online = addresses.Any(liveTailnet.Contains) || streamer != null || lanAddress != null,
                TailnetVisible = true, Tailnet = streamer, Lan = near, LanAddress = lanAddress
            });
        }
        devices.AddRange(local.Values.Select(peer => new Device
        {
            Id = $"lan:{peer.Id:N}",
            Name = peer.Name, Platform = peer.Platform, Address = peer.Address,
            LanAddress = peer.Address, Online = true, Lan = lanRoutes.GetValueOrDefault(peer.Address)
        }));
        return devices.OrderByDescending(device => device.CanConnect).ThenByDescending(device => device.Online)
            .ThenBy(device => device.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    private static Dictionary<IPAddress, StreamHost> StreamRoutes(IReadOnlyList<StreamHost> hosts) =>
        (hosts ?? Array.Empty<StreamHost>()).Where(host => host.Endpoint.Port == BundledSunshine.BasePort)
            .ToDictionary(host => host.Endpoint.Address);

    internal static async Task<StreamHost> ChooseRoute(Device device, Func<IPEndPoint, Task<StreamHost>> probe) =>
        (await TryRoutes(device, route => probe(route.Endpoint))).Route;

    private static async Task<(StreamHost Route, StreamHost Current)> TryRoutes(Device device, Func<StreamHost, Task<StreamHost>> probe)
    {
        foreach (var route in new[] { device.Lan, device.Tailnet })
        {
            if (route == null) continue;
            var host = await probe(route);
            if (host != null && string.Equals(host.Id, route.Id, StringComparison.OrdinalIgnoreCase)) return (route, host);
        }
        throw new InvalidOperationException("无法连接这台设备。请确认 Sunshine 正在运行，再刷新设备列表。");
    }

    private static async Task<(StreamHost Route, StreamHost Current)> ProbeRoutes(Device device, CancellationToken token, bool requirePower)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(4));
        return await TryRoutes(device, async route =>
        {
            var current = await ReadHost(route.Endpoint, timeout.Token, route.ViaEmbeddedTailnet);
            token.ThrowIfCancellationRequested();
            return requirePower && current?.SupportsPower != true ? null : current;
        });
    }

    internal static async Task<StreamHost> ChoosePowerRoute(Device device, CancellationToken token) =>
        (await ProbeRoutes(device, token, requirePower: true)).Route;

    public static async Task<StreamHost> ChooseRoute(Device device, Action onEncoderWait, CancellationToken token)
    {
        var selected = await ProbeRoutes(device, token, requirePower: false);

        if (selected.Current.EncoderProbePending)
        {
            onEncoderWait();
            await WaitForEncoder(selected.Route, token);
        }
        return selected.Route;
    }

    private static async Task WaitForEncoder(StreamHost route, CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        try
        {
            using var bridge = await EmbeddedTailnet.OpenStream(route, timeout.Token);
            while (true)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(300), timeout.Token);
                var current = await ReadHost(route.Endpoint, timeout.Token, route.ViaEmbeddedTailnet, bridge.Route.Endpoint);
                timeout.Token.ThrowIfCancellationRequested();
                if (current == null) continue;
                if (!string.Equals(route.Id, current.Id, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("被控设备已变化，请刷新设备列表后重试。");
                // An unsuccessful initial probe still allows Sunshine to prepare the selected display and probe again.
                if (!current.EncoderProbePending) return;
            }
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        {
            throw new TimeoutException("等待被控端的编码器检测超时，请检查被控电脑后重试。");
        }
    }
}
