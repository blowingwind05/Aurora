#Requires -Version 7.3

param([Parameter(Mandatory)][string] $AssemblyPath)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$flags = [Reflection.BindingFlags]'Static,Public,NonPublic'
$pairing = $assembly.GetType('Aurora.AutomaticPairing', $true)
$requestType = $assembly.GetType('Aurora.SunshinePairing', $true)
$find = $pairing.GetMethod('FindPending', $flags)
$read = $pairing.GetMethod('ReadProxySource', $flags)

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

if ($IsWindows) {
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
            $claimed = $loopback.GetMethod('IsFromService', $flags).Invoke($null,
                @($incoming.Client.RemoteEndPoint, $incoming.Client.LocalEndPoint, [Threading.CancellationToken]::None))
            if ($claimed.GetAwaiter().GetResult()) {
                throw 'An ordinary local process must not be treated as the Tailscale service.'
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
