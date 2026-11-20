#Requires -Version 7.3

param([string] $AssemblyPath = (Join-Path $PSScriptRoot '../dist/Avalonia-0.4/Aurora.dll'))

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$discovery = $assembly.GetType('Aurora.Discovery', $true)
$deviceType = $assembly.GetType('Aurora.Device', $true)
$hostType = $assembly.GetType('Aurora.StreamHost', $true)
$staticMethods = [Reflection.BindingFlags]'Static,Public,NonPublic'
$routeConstructor = $discovery.GetMethod('ParseHost', $staticMethods)
$merge = $discovery.GetMethod('Merge', $staticMethods)

function Assert-True {
    param([bool] $Condition, [string] $Reason)
    if (-not $Condition) { throw $Reason }
}

function New-TestHost {
    param([string] $Address, [string] $Id)
    $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Parse($Address), 47989)
    $xml = "<root status_code='200'><hostname>Studio</hostname><uniqueid>$Id</uniqueid><appversion>7</appversion><PairStatus>0</PairStatus></root>"
    $routeConstructor.Invoke($null, @($endpoint, $xml))
}

function Get-Routes {
    param([object[]] $Lan, [object[]] $Tailnet)
    $typedLan = [Array]::CreateInstance($hostType, $Lan.Count)
    $typedTailnet = [Array]::CreateInstance($hostType, $Tailnet.Count)
    $Lan.CopyTo($typedLan, 0)
    $Tailnet.CopyTo($typedTailnet, 0)
    $arguments = [object[]]::new(3)
    $arguments[0] = $peers
    $arguments[1] = $typedLan
    $arguments[2] = $typedTailnet
    $merge.Invoke($null, $arguments)
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
$peers = [Array]::CreateInstance($deviceType, 1)
$peers.SetValue($peer, 0)

$combined = @(Get-Routes -Lan @($lan) -Tailnet @($tailnet))
Assert-True ($combined.Count -eq 1 -and $combined[0].Lan -and $combined[0].Tailnet) 'One host ID on both routes should make one device.'
$distinct = @(Get-Routes -Lan @((New-TestHost '192.168.12.9' 'host-b')) -Tailnet @($tailnet))
Assert-True ($distinct.Count -eq 2) 'Identical names must not merge distinct hosts.'
$unverified = @(Get-Routes -Lan @() -Tailnet @())
Assert-True ($unverified.Count -eq 1 -and -not $unverified[0].CanConnect) 'Tailnet online status alone must not enable streaming.'
$peers = [Array]::CreateInstance($deviceType, 0)
$localOnly = @(Get-Routes -Lan @($lan) -Tailnet @())
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
Assert-True (($pairArguments -join '|') -ceq 'pair|[fd7a:115c:a1e0::123]:47989') 'Pairing must pass IPv6 and port as one Moonlight host argument.'
Assert-True (($streamArguments -join '|') -ceq 'stream|192.168.12.8:47989|Desktop') 'Streaming must pass the selected host and Desktop separately.'

$runtimeArgs = $assembly.GetType('Aurora.Upstream', $true).GetMethod('StreamRuntimeArguments', $staticMethods)
$integrated = $runtimeArgs.Invoke($null, @('/Applications/Aurora.app/Contents/MacOS/AuroraStream', $streamArguments))
$integratedWin = $runtimeArgs.Invoke($null, @('C:/Aurora/Streamer/AuroraStream.exe', $streamArguments))
$external = $runtimeArgs.Invoke($null, @('/Applications/Moonlight.app/Contents/MacOS/Moonlight', $streamArguments))
Assert-True (($integrated -join '|') -ceq '--aurora-runtime|stream|192.168.12.8:47989|Desktop') 'The integrated worker must receive the private entry point followed by the original arguments.'
Assert-True (($integratedWin -join '|') -ceq ($integrated -join '|')) 'Windows must use the same private runtime command as macOS.'
Assert-True (($external -join '|') -ceq ($streamArguments -join '|')) 'External Moonlight must keep the upstream argument format.'

'Desktop discovery, route, and Moonlight argument tests passed.'
