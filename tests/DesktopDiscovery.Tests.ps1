#Requires -Version 7.3

param([Parameter(Mandatory)][string] $AssemblyPath)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$discovery = $assembly.GetType('Aurora.Discovery', $true)
$deviceType = $assembly.GetType('Aurora.Device', $true)
$hostType = $assembly.GetType('Aurora.StreamHost', $true)
$presenceType = $assembly.GetType('Aurora.PresentDevice', $true)
$lanType = $assembly.GetType('Aurora.LanSnapshot', $true)
$tailnetType = $assembly.GetType('Aurora.TailnetSnapshot', $true)
$networkType = $assembly.GetType('Aurora.NetworkSnapshot', $true)
$staticMethods = [Reflection.BindingFlags]'Static,Public,NonPublic'
$routeConstructor = $discovery.GetMethod('ParseHost', $staticMethods)
$merge = $discovery.GetMethod('Merge', $staticMethods)

function Assert-True {
    param([bool] $Condition, [string] $Reason)
    if (-not $Condition) { throw $Reason }
}

function New-TestHost {
    param([string] $Address, [string] $Id, [int] $Port = 48989, [string] $EncoderStatus)
    $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Parse($Address), $Port)
    $encoder = if ($EncoderStatus) { "<AuroraEncoderStatus>$EncoderStatus</AuroraEncoderStatus>" }
    $xml = "<root status_code='200'><hostname>Studio</hostname><uniqueid>$Id</uniqueid><appversion>7</appversion><PairStatus>0</PairStatus>$encoder</root>"
    $routeConstructor.Invoke($null, @($endpoint, $xml))
}

function Get-Routes {
    param([object[]] $Lan, [object[]] $Tailnet, [object[]] $LocalPresence = @(),
        [object[]] $TailnetPresence = @(), [System.Collections.Generic.Dictionary[string,guid]] $Known = $null)
    $typedLan = [Array]::CreateInstance($hostType, $Lan.Count)
    $typedTailnet = [Array]::CreateInstance($hostType, $Tailnet.Count)
    $typedLocalPresence = [Array]::CreateInstance($presenceType, $LocalPresence.Count)
    $typedTailnetPresence = [Array]::CreateInstance($presenceType, $TailnetPresence.Count)
    $Lan.CopyTo($typedLan, 0)
    $Tailnet.CopyTo($typedTailnet, 0)
    $LocalPresence.CopyTo($typedLocalPresence, 0)
    $TailnetPresence.CopyTo($typedTailnetPresence, 0)
    if ($null -eq $Known) { $Known = [System.Collections.Generic.Dictionary[string,guid]]::new() }
    $arguments = [object[]]::new(3)
    $arguments[0] = $peers
    $arguments[1] = [Activator]::CreateInstance($lanType, @($null, $typedLan, $typedLocalPresence))
    $arguments[2] = [Activator]::CreateInstance($tailnetType, @($typedTailnet, $typedTailnetPresence, $Known))
    $merge.Invoke($null, $arguments)
}

function New-Presence {
    param([guid] $Identity, [string] $Address)
    [Activator]::CreateInstance($presenceType, @($Identity, [Net.IPAddress]::Parse($Address), 'Studio', 'windows', $true))
}

$tailnet = New-TestHost '100.64.4.8' 'host-a'
$lan = New-TestHost '192.168.12.8' 'host-a'
Assert-True ($lan.Id -eq 'host-a') 'Expected a valid GameStream host record.'
$invalid = $routeConstructor.Invoke($null, @($lan.Endpoint, "<root status_code='200'><hostname>Printer</hostname></root>"))
Assert-True ($null -eq $invalid) 'Unrelated HTTP responses must not become stream hosts.'

$peer = [Activator]::CreateInstance($deviceType, $true)
$peer.Id = 'tailscale-node'
$peer.Name = 'Studio'
$peer.Platform = 'windows'
$peer.Online = $true
$peer.Address = $tailnet.Endpoint.Address
$peer.AlternateAddress = [Net.IPAddress]::Parse('fd7a:115c:a1e0::8')
$snapshot = [Activator]::CreateInstance($networkType, $true)
$snapshot.Owner = 1
$snapshot.LocalId = 'self-node'
$snapshot.LocalAddress = [Net.IPAddress]::Parse('100.64.4.1')
$snapshot.Devices.Add($peer)
$lookup = $discovery.GetMethod('FindTailnetPeer', $staticMethods)
foreach ($address in @($peer.Address, $peer.Address.MapToIPv6(), $peer.AlternateAddress)) {
    Assert-True ($lookup.Invoke($null, @($snapshot, $address)).Id -eq $peer.Id) 'Both authenticated address families map to one node.'
}
Assert-True ($null -eq $lookup.Invoke($null, @($snapshot, [Net.IPAddress]::Loopback))) 'Loopback is not a tailnet identity.'
Assert-True ($null -eq $lookup.Invoke($null, @($snapshot, [Net.IPAddress]::Parse('100.64.4.99')))) 'Unknown peers must be rejected.'
$duplicatePeer = [Activator]::CreateInstance($deviceType, $true)
$duplicatePeer.Id = 'different-node'
$duplicatePeer.Address = $peer.AlternateAddress
$snapshot.Devices.Add($duplicatePeer)
Assert-True ($null -eq $lookup.Invoke($null, @($snapshot, $peer.AlternateAddress))) 'An ambiguous tailnet address must be rejected.'
$peers = [Array]::CreateInstance($deviceType, 1)
$peers.SetValue($peer, 0)

$identity = [guid]::NewGuid()
$known = [System.Collections.Generic.Dictionary[string,guid]]::new()
$known.Add($peer.Id, $identity)
$nearby = New-Presence $identity '192.168.12.8'
$remote = New-Presence $identity '100.64.4.8'
$recognized = @{ LocalPresence = @($nearby); TailnetPresence = @($remote); Known = $known }
$pendingFlag = $hostType.GetProperty('EncoderProbePending', [Reflection.BindingFlags]'Instance,NonPublic')
$probingHost = New-TestHost '192.168.12.8' 'host-a' -EncoderStatus 'probing'
$probingDevice = @(Get-Routes -Lan @($probingHost) -Tailnet @($tailnet) @recognized)
Assert-True ($probingDevice[0].CanConnect -and $pendingFlag.GetValue($probingDevice[0].Lan)) 'A host is discoverable and actionable while its initial encoder probe runs.'
foreach ($status in 'ready', 'failed', '') {
    $hostWithStatus = New-TestHost '192.168.12.8' 'host-a' -EncoderStatus $status
    Assert-True ($hostWithStatus -and !$pendingFlag.GetValue($hostWithStatus)) 'Completed or legacy hosts do not require an initial-probe wait.'
}
$combined = @(Get-Routes -Lan @($lan) -Tailnet @($tailnet) @recognized)
Assert-True ($combined.Count -eq 1 -and $combined[0].Lan -and $combined[0].Tailnet) 'One host ID on both routes should make one device.'
$embeddedHost = New-TestHost $peer.AlternateAddress.ToString() 'host-a'
$embeddedFlag = $hostType.GetProperty('ViaEmbeddedTailnet', [Reflection.BindingFlags]'Instance,NonPublic')
$embeddedFlag.SetValue($embeddedHost, $true)
$hasPrivacy = $hostType.GetProperty('SupportsSessionPrivacy', [Reflection.BindingFlags]'Instance,NonPublic')
$parseHost = $discovery.GetMethod('ParseHost', $staticMethods)
$privacyHost = $parseHost.Invoke($null, @($lan.Endpoint, '<root status_code="200"><PairStatus>1</PairStatus><appversion>7.1.0.0</appversion><hostname>Fixture</hostname><uniqueid>fixture</uniqueid><AuroraSessionPrivacy>1</AuroraSessionPrivacy></root>'))
Assert-True ($hasPrivacy.GetValue($privacyHost) -and !$hasPrivacy.GetValue($lan)) 'Only hosts that advertise live privacy should receive privacy requests.'
$embeddedPeer = New-Presence $identity $peer.AlternateAddress.ToString()
$dualStack = @(Get-Routes -Lan @($lan) -Tailnet @($embeddedHost) -LocalPresence @($nearby) -TailnetPresence @($embeddedPeer) -Known $known)
Assert-True ($dualStack.Count -eq 1 -and $dualStack[0].Online -and $dualStack[0].Lan -and
    $dualStack[0].Tailnet.Endpoint.Address.Equals($peer.AlternateAddress) -and $embeddedFlag.GetValue($dualStack[0].Tailnet)) `
    'An embedded IPv6 route remains associated with its known node and carries its real endpoint and transport flag.'
$alternate = New-Presence $identity '192.168.13.8'
$multiLan = @(Get-Routes -Lan @($lan) -Tailnet @($tailnet) -LocalPresence @($alternate, $nearby) -TailnetPresence @($remote) -Known $known)
Assert-True ($multiLan.Count -eq 1 -and $multiLan[0].LanAddress.Equals($lan.Endpoint.Address)) 'Multiple LAN addresses for one Aurora should retain the usable route.'
$stockLan = New-TestHost '192.168.12.8' 'stock-host' 47989
$stockTailnet = New-TestHost '100.64.4.8' 'stock-host' 47989
$coexisting = @(Get-Routes -Lan @($stockLan, $lan) -Tailnet @($stockTailnet, $tailnet) @recognized)
Assert-True ($coexisting.Count -eq 1 -and $coexisting[0].Lan.Id -eq 'host-a' -and $coexisting[0].Tailnet.Id -eq 'host-a') 'Aurora must choose its embedded host even when the independent Sunshine responds on the same addresses.'
$stockOnly = @(Get-Routes -Lan @($stockLan) -Tailnet @($stockTailnet) @recognized)
Assert-True ($stockOnly.Count -eq 1 -and $stockOnly[0].Online -and !$stockOnly[0].CanConnect) 'A running Aurora controller should appear without accepting an independent Sunshine.'
$other = New-Presence ([guid]::NewGuid()) '192.168.12.9'
$distinct = @(Get-Routes -Lan @((New-TestHost '192.168.12.9' 'host-b')) -Tailnet @($tailnet) -LocalPresence @($other) -TailnetPresence @($remote) -Known $known)
Assert-True ($distinct.Count -eq 2) 'Identical names must not merge distinct hosts.'
$unverified = @(Get-Routes -Lan @($lan) -Tailnet @($tailnet))
Assert-True ($unverified.Count -eq 0) 'An unrelated Tailnet peer or a stream port alone must not appear.'
$offline = @(Get-Routes -Lan @() -Tailnet @() -Known $known)
Assert-True ($offline.Count -eq 1 -and !$offline[0].Online -and !$offline[0].CanConnect) 'A recognized peer remains visible when Aurora is offline.'
$offlineTailnet = @(Get-Routes -Lan @($lan) -Tailnet @() -LocalPresence @($nearby) -Known $known)
Assert-True ($offlineTailnet.Count -eq 1 -and $offlineTailnet[0].Online -and $offlineTailnet[0].Lan) 'A recognized peer can still appear online over LAN.'
$peers = [Array]::CreateInstance($deviceType, 0)
$localOnly = @(Get-Routes -Lan @($lan) -Tailnet @() -LocalPresence @($nearby))
Assert-True ($localOnly.Count -eq 1 -and $localOnly[0].CanConnect) 'LAN must work without a Tailscale snapshot.'

$choose = $discovery.GetMethods($staticMethods) | Where-Object { $_.Name -eq 'ChooseRoute' -and $_.GetParameters().Length -eq 2 }
$probeType = $choose.GetParameters()[1].ParameterType
$taskFromResult = [Threading.Tasks.Task].GetMethods() | Where-Object Name -eq 'FromResult'
$taskFromResult = $taskFromResult.MakeGenericMethod($hostType)
$online = { param($endpoint)
    $value = if ($endpoint.Equals($lan.Endpoint)) { $lan } else { $tailnet }
    return ,$taskFromResult.Invoke($null, @($value))
}.GetNewClosure()
$lanUnavailable = { param($endpoint)
    $value = if ($endpoint.Equals($tailnet.Endpoint)) { $tailnet } else { $null }
    return ,$taskFromResult.Invoke($null, @($value))
}.GetNewClosure()
$route = $choose.Invoke($null, @($combined[0], [Management.Automation.LanguagePrimitives]::ConvertTo($online, $probeType))).GetAwaiter().GetResult()
Assert-True ($route.Endpoint.Equals($lan.Endpoint)) 'Connection should select a reachable LAN route first.'
$route = $choose.Invoke($null, @($combined[0], [Management.Automation.LanguagePrimitives]::ConvertTo($lanUnavailable, $probeType))).GetAwaiter().GetResult()
Assert-True ($route.Endpoint.Equals($tailnet.Endpoint)) 'Connection should fall back to the matching Tailscale route.'

$moonlightArgs = $assembly.GetType('Aurora.Upstream', $true).GetMethod('MoonlightArguments', $staticMethods)
$ipv6 = New-TestHost 'fd7a:115c:a1e0::123' 'host-v6'
$pairArguments = $moonlightArgs.Invoke($null, @($ipv6, $true))
$streamArguments = $moonlightArgs.Invoke($null, @($lan, $false))
Assert-True (($pairArguments -join '|') -ceq 'pair|[fd7a:115c:a1e0::123]:48989') 'Pairing must pass IPv6 and port as one Moonlight host argument.'
Assert-True (($streamArguments -join '|') -ceq 'stream|192.168.12.8:48989|Desktop') 'Streaming must pass the selected host and Desktop separately.'

$runtimeArgs = $assembly.GetType('Aurora.Upstream', $true).GetMethod('StreamRuntimeArguments', $staticMethods)
$integrated = $runtimeArgs.Invoke($null, @('/Applications/Aurora.app/Contents/MacOS/AuroraStream', $streamArguments))
$integratedWin = $runtimeArgs.Invoke($null, @('C:/Aurora/Streamer/AuroraStream.exe', $streamArguments))
$external = $runtimeArgs.Invoke($null, @('/Applications/Moonlight.app/Contents/MacOS/Moonlight', $streamArguments))
Assert-True (($integrated -join '|') -ceq '--aurora-runtime|stream|192.168.12.8:48989|Desktop') 'The integrated worker must receive the private entry point followed by the original arguments.'
Assert-True (($integratedWin -join '|') -ceq ($integrated -join '|')) 'Windows must use the same private runtime command as macOS.'
Assert-True (($external -join '|') -ceq ($streamArguments -join '|')) 'External Moonlight must keep the upstream argument format.'

$preferences = $assembly.GetType('Aurora.StreamPreferences', $true)
$preferenceArgs = $preferences.GetMethod('Arguments', $staticMethods)
$setToggle = $preferences.GetMethod('SetToggle', $staticMethods)
try {
    foreach ($windowDefaults in $true, $false) {
        $defaults = $preferenceArgs.Invoke($null, @($windowDefaults, $true))
        Assert-True ($defaults -ccontains '--vsync' -and $defaults -ccontains '--keep-awake' -and
            $defaults -ccontains '--absolute-mouse' -and $defaults -ccontains '--no-frame-pacing') `
            'Bundled Aurora playback must apply its defaults with and without the embedded window.'
    }
    $standalone = $preferenceArgs.Invoke($null, @($false, $false))
    Assert-True ($standalone -cnotcontains '--vsync' -and $standalone -cnotcontains '--keep-awake') `
        'Aurora playback defaults must not take over the independently installed Moonlight preferences.'
    $setToggle.Invoke($null, @('vsync', $false))
    $setToggle.Invoke($null, @('keep-awake', $false))
    $setToggle.Invoke($null, @('absolute-mouse', $false))
    $setToggle.Invoke($null, @('frame-pacing', $true))
    $custom = $preferenceArgs.Invoke($null, @($true, $true))
    Assert-True ($custom -ccontains '--no-vsync' -and $custom -ccontains '--no-keep-awake' -and
        $custom -ccontains '--no-absolute-mouse' -and $custom -ccontains '--no-frame-pacing') `
        'Explicit overrides must win, and frame pacing requires V-Sync.'
}
finally { $preferences.GetMethod('Reset', $staticMethods).Invoke($null, $null) }

'Desktop discovery, route, Moonlight arguments, and playback defaults tests passed.'
