using System;
using System.Collections.Generic;
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

internal sealed record StreamHost(IPEndPoint Endpoint, string Id, string Name);
internal sealed record LanSnapshot(IPAddress Address, IReadOnlyList<StreamHost> Hosts);

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

    public static async Task<LanSnapshot> ReadLan()
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
        if (links.Length == 0) return new(null, Array.Empty<StreamHost>());

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var advertisements = await ZeroconfResolver.ResolveAsync(Service, scanTime: TimeSpan.FromMilliseconds(1200),
            retries: 1, cancellationToken: timeout.Token, netInterfacesToSendRequestOn: links.Select(link => link.Adapter).ToArray());
        var local = links.SelectMany(link => link.Addresses).ToArray();
        var candidates = from advertisement in advertisements
                         from literal in advertisement.IPAddresses
                         let address = IPAddress.TryParse(literal, out var parsed) ? parsed : null
                         where address != null && !local.Any(item => item.Address.Equals(address)) &&
                             local.Any(item => SameSubnet(item.Address, item.IPv4Mask, address))
                         from service in advertisement.Services.Values
                         where string.Equals(service.Name.TrimEnd('.'), Service.TrimEnd('.'), StringComparison.OrdinalIgnoreCase) &&
                             service.Port is > 0 and <= 65535
                         select new IPEndPoint(address, service.Port);
        return new(local[0].Address, await Probe(candidates, timeout.Token));
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

    public static async Task<IReadOnlyList<StreamHost>> ReadTailnetHosts(NetworkSnapshot tailnet)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(4));
        return await Probe(tailnet.Devices.Where(peer => peer.Online).Select(peer => new IPEndPoint(peer.Address, 47989)), timeout.Token);
    }

    private static async Task<IReadOnlyList<StreamHost>> Probe(IEnumerable<IPEndPoint> endpoints, CancellationToken token)
    {
        var found = new System.Collections.Concurrent.ConcurrentBag<StreamHost>();
        try
        {
            await Parallel.ForEachAsync(endpoints.Distinct(), new ParallelOptions { MaxDegreeOfParallelism = 8, CancellationToken = token },
                async (endpoint, cancellation) =>
                {
                    var host = await ReadHost(endpoint, cancellation);
                    if (host != null) found.Add(host);
                });
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        return found.OrderBy(host => host.Endpoint.ToString(), StringComparer.Ordinal).ToArray();
    }

    private static async Task<StreamHost> ReadHost(IPEndPoint endpoint, CancellationToken token = default)
    {
        try
        {
            var uri = new UriBuilder("http", endpoint.Address.ToString(), endpoint.Port, "/serverinfo").Uri;
            using var response = await Http.GetAsync(uri, token);
            if (!response.IsSuccessStatusCode) return null;
            return ParseHost(endpoint, await response.Content.ReadAsStringAsync(token));
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException or XmlException or IOException)
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
            ? null : new(endpoint, id, name);
    }

    internal static List<Device> Merge(IEnumerable<Device> peers, IEnumerable<StreamHost> lan, IEnumerable<StreamHost> tailnet)
    {
        var local = lan.GroupBy(host => host.Id, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);
        var byEndpoint = tailnet.ToDictionary(host => host.Endpoint);
        var byIdentity = byEndpoint.Values.ToLookup(host => host.Id, StringComparer.OrdinalIgnoreCase);
        var devices = new List<Device>();
        foreach (var peer in peers)
        {
            byEndpoint.TryGetValue(new IPEndPoint(peer.Address, 47989), out var streamer);
            StreamHost near = null;
            if (streamer != null && byIdentity[streamer.Id].Count() == 1)
                local.Remove(streamer.Id, out near);
            devices.Add(new Device
            {
                Id = peer.Id, Name = peer.Name, Platform = peer.Platform, Address = peer.Address,
                Online = peer.Online, TailnetVisible = true, Tailnet = streamer, Lan = near
            });
        }
        devices.AddRange(local.Values.Select(host => new Device
        {
            Id = "lan:" + host.Id, Name = host.Name, Platform = "", Address = host.Endpoint.Address, Online = true, Lan = host
        }));
        return devices.OrderByDescending(device => device.CanConnect).ThenByDescending(device => device.Online)
            .ThenBy(device => device.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    internal static async Task<StreamHost> ChooseRoute(Device device, Func<IPEndPoint, Task<StreamHost>> probe)
    {
        foreach (var route in new[] { device.Lan, device.Tailnet })
        {
            if (route == null) continue;
            var host = await probe(route.Endpoint);
            if (host != null && string.Equals(host.Id, route.Id, StringComparison.OrdinalIgnoreCase)) return route;
        }
        throw new InvalidOperationException("无法连接这台设备。请确认 Sunshine 正在运行，再刷新设备列表。");
    }

    public static Task<StreamHost> ChooseRoute(Device device) => ChooseRoute(device, endpoint => ReadHost(endpoint));
}
