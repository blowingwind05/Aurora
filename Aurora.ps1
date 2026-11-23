#Requires -Version 7.3

[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('Devices', 'Doctor', 'Pair', 'Apps', 'Stream')]
    [string] $Command,

    [Parameter(Position = 1)]
    [string] $Target,

    [string] $AppName = 'Desktop',
    [string] $TailscaleExe,
    [string] $MoonlightExe
)

$ErrorActionPreference = 'Stop'

try {
    if (-not $IsWindows) {
        throw 'This Aurora prototype supports Windows only.'
    }

    . (Join-Path $PSScriptRoot 'cli/Tailscale.ps1')
    . (Join-Path $PSScriptRoot 'cli/Moonlight.ps1')

    $needsTarget = $Command -in 'Pair', 'Apps', 'Stream'
    if ($needsTarget -and [string]::IsNullOrWhiteSpace($Target)) {
        throw 'Choose an exact device from Aurora.ps1 Devices and pass its Id, name, DNS name or address.'
    }
    if (-not $needsTarget -and $PSBoundParameters.ContainsKey('Target')) {
        throw 'Only Pair, Apps, and Stream accept a device target.'
    }
    if ($Command -ne 'Stream' -and $PSBoundParameters.ContainsKey('AppName')) {
        throw 'Only Stream accepts -AppName.'
    }
    if ($Command -eq 'Stream' -and [string]::IsNullOrWhiteSpace($AppName)) {
        throw 'Pass a non-empty -AppName or omit it to stream Desktop.'
    }

    $tailscaleOptions = @{}
    if ($PSBoundParameters.ContainsKey('TailscaleExe')) {
        $tailscaleOptions.Executable = $TailscaleExe
    }
    $moonlightOptions = @{}
    if ($PSBoundParameters.ContainsKey('MoonlightExe')) {
        $moonlightOptions.Executable = $MoonlightExe
    }

    if ($Command -eq 'Doctor') {
        $checks = @(
            try {
                $status = Get-AuroraTailscaleStatus @tailscaleOptions
                $peers = @(Get-AuroraTailscalePeers -Status $status)
                $onlineWindowsCount = @($peers | Where-Object { $_.Online -and $_.OperatingSystem -eq 'windows' }).Count
                [pscustomobject]@{ Component = 'Tailscale'; Ready = $true; Detail = "Running; $onlineWindowsCount online same-account Windows peer(s)." }
            }
            catch {
                [pscustomobject]@{ Component = 'Tailscale'; Ready = $false; Detail = $_.Exception.Message }
            }
            try {
                $moonlight = Get-AuroraMoonlightExecutable @moonlightOptions
                [pscustomobject]@{ Component = 'Moonlight'; Ready = $true; Detail = $moonlight }
            }
            catch {
                [pscustomobject]@{ Component = 'Moonlight'; Ready = $false; Detail = $_.Exception.Message }
            }
        )
        $checks | Format-Table -Wrap -AutoSize | Out-Host
        if ($checks.Ready -contains $false) {
            exit 1
        }
        return
    }

    $status = Get-AuroraTailscaleStatus @tailscaleOptions
    $peers = @(Get-AuroraTailscalePeers -Status $status)
    if ($Command -eq 'Devices') {
        if ($peers.Count -eq 0) {
            Write-Information 'No same-account devices are visible in Tailscale.' -InformationAction Continue
        }
        $peers
        return
    }

    $peer = Resolve-AuroraTailscalePeer -Peers $peers -Target $Target
    if ($peer.OperatingSystem -ne 'windows') {
        throw 'Select a Windows peer. Other host platforms are outside the prototype scope.'
    }
    if (-not $peer.Online) {
        throw 'The selected device is offline in Tailscale. Bring it online and try again.'
    }

    $moonlight = Get-AuroraMoonlightExecutable @moonlightOptions
    $operation = if ($Command -eq 'Apps') { 'List' } else { $Command }
    Invoke-AuroraMoonlight -Executable $moonlight -Operation $operation -Address $peer.Address -AppName $AppName
}
catch {
    [Console]::Error.WriteLine("Aurora: $($_.Exception.Message)")
    exit 1
}
