#Requires -Version 7.3

param([Parameter(Mandatory)][string] $AssemblyPath, [switch] $OfflineOnly)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$flags = [Reflection.BindingFlags]'Static,Public,NonPublic'
$pairing = $assembly.GetType('Aurora.AutomaticPairing', $true)
$requestType = $assembly.GetType('Aurora.SunshinePairing', $true)
$find = $pairing.GetMethod('FindPending', $flags)
$findEmbedded = $pairing.GetMethod('FindEmbeddedPending', $flags)
$read = $pairing.GetMethod('ReadProxySource', $flags)
$parsePending = $assembly.GetType('Aurora.SunshineAdmin', $true).GetMethod('ParsePendingPairing', $flags)
$sourcePort = $requestType.GetProperty('SourcePort', [Reflection.BindingFlags]'Instance,NonPublic')

function Find-Request($source, $marker, $requests) {
    $items = [Array]::CreateInstance($requestType, $requests.Count)
    for ($index = 0; $index -lt $requests.Count; $index++) {
        $items[$index] = [Activator]::CreateInstance($requestType, [object[]]$requests[$index])
    }
    $find.Invoke($null, @($items, [Net.IPAddress]::Parse($source), $marker))
}

$marker = '01234567890123456789012345678901'
$source = '100.72.4.5'
$matching = @('12345678901234567890123456789012', "aurora-$marker", "::ffff:$source")
$otherSource = @('22345678901234567890123456789012', "aurora-$marker", '100.72.4.6')
$oldRequest = @('32345678901234567890123456789012', 'roth', $source)
$duplicate = @('42345678901234567890123456789012', "aurora-$marker", $source)
if ((Find-Request $source $marker @($oldRequest, $otherSource, $matching)).Id -ne $matching[0]) {
    throw 'Only the current marker from the authenticated source may be approved.'
}
if ($null -ne (Find-Request $source $marker @($matching, $duplicate))) {
    throw 'Ambiguous requests must be rejected.'
}
if ($null -ne (Find-Request $source $marker @($oldRequest, $otherSource))) {
    throw 'Another peer or an unrelated Moonlight request must never be approved.'
}

function Read-Pending($id, $address, $portMember = '') {
    $json = [Text.Json.JsonDocument]::Parse("{`"id`":`"$id`",`"name`":`"aurora-$marker`",`"address`":`"$address`"$portMember}")
    try { $parsePending.Invoke($null, @($json.RootElement)) }
    finally { $json.Dispose() }
}

function Find-EmbeddedRequest($source, $requests) {
    $items = [Array]::CreateInstance($requestType, $requests.Count)
    $requests.CopyTo($items, 0)
    $findEmbedded.Invoke($null, @($items, [Net.IPAddress]::Parse($source), $marker))
}

$legacy = Read-Pending $matching[0] '127.0.0.1'
if ($legacy.Id -ne $matching[0] -or $null -ne $sourcePort.GetValue($legacy) -or
    $null -ne (Find-EmbeddedRequest '127.0.0.1' @($legacy))) {
    throw 'A legacy pending request must parse for manual pairing and be ineligible for embedded approval.'
}
foreach ($invalidPort in @('null', 'false', '0', '-1', '65536', '21.5', '"54321"')) {
    $request = Read-Pending $matching[0] '127.0.0.1' ",`"sourcePort`":$invalidPort"
    if ($null -ne $sourcePort.GetValue($request) -or $null -ne (Find-EmbeddedRequest '127.0.0.1' @($request))) {
        throw "Malformed source port $invalidPort made a request eligible."
    }
}
$observed = Read-Pending $matching[0] '::ffff:127.0.0.1' ',"sourcePort":54321'
$changedPort = Read-Pending $matching[0] '::ffff:127.0.0.1' ',"sourcePort":54322'
$lanRequest = Read-Pending $otherSource[0] '192.168.12.3' ',"sourcePort":54321'
$eligible = Find-EmbeddedRequest '127.0.0.1' @($observed, $lanRequest)
if ($eligible.Id -ne $matching[0] -or $sourcePort.GetValue($eligible) -ne 54321) {
    throw 'Embedded pairing must preserve the exact Sunshine-observed source port on the attested address.'
}
if ($null -ne (Find-EmbeddedRequest '127.0.0.1' @($lanRequest)) -or
    $null -ne (Find-EmbeddedRequest '192.168.12.3' @($lanRequest)) -or
    $null -ne (Find-EmbeddedRequest '127.2.0.5' @($observed)) -or
    $null -ne (Find-EmbeddedRequest '127.0.0.1' @($observed, $legacy)) -or $observed.Equals($changedPort)) {
    throw 'The refreshed request must keep the same source port, and mismatched or ambiguous requests must be rejected.'
}

$header = [byte[]](13, 10, 13, 10, 0, 13, 10, 81, 85, 73, 84, 10, 33, 17, 0, 12,
    100, 72, 4, 5, 100, 72, 4, 7, 0xC0, 0x01, 0xAB, 0x32)
$packet = [IO.MemoryStream]::new($header)
$decoded = $read.Invoke($null, @($packet, [Threading.CancellationToken]::None)).GetAwaiter().GetResult()
if ($decoded.Address.ToString() -ne $source -or $decoded.Port -ne 49153) { throw 'Wrong PROXY source.' }
$header[12] = 32
try {
    $read.Invoke($null, @([IO.MemoryStream]::new($header), [Threading.CancellationToken]::None)).GetAwaiter().GetResult()
    throw 'PROXY LOCAL must be rejected.'
}
catch {
    if ($_.Exception.GetBaseException() -isnot [IO.InvalidDataException]) { throw }
}

$embeddedRead = $pairing.GetMethod('ReadEmbeddedSources', $flags)
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public sealed class AuroraPairingAssertionStream : MemoryStream
{
    private readonly byte[] peer;
    private readonly byte[] alias;
    private readonly byte[] key;
    private readonly byte[] replay;
    public byte[] Response { get; private set; }

    public AuroraPairingAssertionStream(string peer, string alias, byte[] key, byte[] replay)
    {
        this.peer = IPAddress.Parse(peer).GetAddressBytes();
        this.alias = IPAddress.Parse(alias).GetAddressBytes();
        this.key = key;
        this.replay = replay;
    }

    public override ValueTask WriteAsync(ReadOnlyMemory<byte> challenge, CancellationToken token)
    {
        var frame = new byte[1 + peer.Length + alias.Length];
        frame[0] = peer.Length == 4 ? (byte)4 : (byte)6;
        peer.CopyTo(frame, 1);
        alias.CopyTo(frame, 1 + peer.Length);
        var label = Encoding.ASCII.GetBytes("AURORA/PAIR/HOST/1");
        var assertion = new byte[label.Length + challenge.Length + frame.Length];
        label.CopyTo(assertion, 0);
        challenge.CopyTo(assertion.AsMemory(label.Length));
        frame.CopyTo(assertion, label.Length + challenge.Length);
        var proof = HMACSHA256.HashData(key, assertion);
        Write(replay ?? frame);
        if (replay == null) Write(proof);
        Response = ToArray();
        Position = 0;
        return ValueTask.CompletedTask;
    }
}
'@

$key = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
$v6 = 'fd7a:115c:a1e0::5'
$alias = '127.0.0.1'
$legit = [AuroraPairingAssertionStream]::new($v6, $alias, $key, $null)
$asserted = $embeddedRead.Invoke($null, @($legit, $key, [Threading.CancellationToken]::None)).GetAwaiter().GetResult()
if ($asserted.Item1.ToString() -ne $v6 -or $asserted.Item2.ToString() -ne $alias) {
    throw 'An authenticated relay must preserve the IPv6 peer separately from its Sunshine source.'
}
foreach ($invalid in @(
    [AuroraPairingAssertionStream]::new($v6, $alias, [Security.Cryptography.RandomNumberGenerator]::GetBytes(32), $null),
    [AuroraPairingAssertionStream]::new($v6, $alias, $key, $legit.Response),
    [AuroraPairingAssertionStream]::new($v6, '192.168.12.3', $key, $null),
    [AuroraPairingAssertionStream]::new('127.0.0.1', $alias, $key, $null)
)) {
    try {
        $null = $embeddedRead.Invoke($null, @($invalid, $key, [Threading.CancellationToken]::None)).GetAwaiter().GetResult()
        throw 'Pairing accepted a forged proof, replayed proof, or conflated transport and peer addresses.'
    }
    catch {
        if ($_.Exception.GetBaseException() -isnot [IO.InvalidDataException]) { throw }
    }
}

function Test-Port($json) {
    $config = [Text.Json.JsonDocument]::Parse($json)
    try { $pairing.GetMethod('UsesPort', $flags).Invoke($null, @($config.RootElement)) }
    finally { $config.Dispose() }
}
if ((Test-Port '{"TCP":{"43825":{"TCPForward":"127.0.0.1:8888"}}}') -or
    !(Test-Port '{"Foreground":{"a":{"TCP":{"43826":{"TCPForward":"127.0.0.1:8888"}}}}}') -or
    !(Test-Port '{"AllowFunnel":{"example.ts.net:43826":true}}')) {
    throw 'Reserved port detection must cover foreground Serve and Funnel.'
}

if ($IsWindows -and !$OfflineOnly) {
    $loopback = $assembly.GetType('Aurora.TailscaleLoopback', $true)
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $client.Connect($listener.LocalEndpoint)
        $incoming = $listener.AcceptTcpClient()
        try {
            $owns = $loopback.GetMethod('HasLoopbackConnection', $flags)
            if (-not $owns.Invoke($null, @($PID, $incoming.Client.RemoteEndPoint.Port, $incoming.Client.LocalEndPoint.Port))) {
                throw 'Windows TCP ownership must be resolved for the exact source and destination tuple.'
            }
            $modeType = $assembly.GetType('Aurora.TailscaleMode', $true)
            foreach ($mode in @('System', 'Embedded')) {
                $claimed = $loopback.GetMethod('IsFromService', $flags).Invoke($null,
                    @([Enum]::Parse($modeType, $mode), $incoming.Client.RemoteEndPoint, $incoming.Client.LocalEndPoint,
                        [Threading.CancellationToken]::None))
                if ($claimed.GetAwaiter().GetResult()) {
                    throw "An ordinary local process must not be treated as the $mode Tailscale service."
                }
            }
        }
        finally { $incoming.Dispose() }
    }
    finally {
        $client.Dispose()
        $listener.Stop()
    }
    $child = $assembly.GetType('Aurora.WindowsChildProcess', $true)
    $options = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'))
    $options.UseShellExecute = $false
    $options.CreateNoWindow = $true
    foreach ($argument in @('-NoProfile', '-Command', 'Start-Sleep -Seconds 15')) { $options.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($options)
    try {
        $job = $child.GetMethod('StopWithParent', $flags).Invoke($null, @($process))
        $job.Dispose()
        if (-not $process.WaitForExit(3000)) { throw 'A managed Tailscale relay must stop when the job handle closes.' }
    }
    finally {
        if (-not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
}

'Automatic pairing request binding, proxy parsing, and relay isolation tests passed.'
