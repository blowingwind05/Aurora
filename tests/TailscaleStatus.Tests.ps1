#Requires -Version 7.3

param([string] $AssemblyPath = (Join-Path $PSScriptRoot '../dist/Avalonia-0.6.2/Aurora.dll'))

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$parse = $assembly.GetType('Aurora.TailscaleStatus', $true).GetMethod('Parse')

foreach ($case in @(
    @{ Json = '{"BackendState":"Running","CurrentTailnet":{"Name":"test"}}'; State = 'Connected'; Tailnet = 'test' }
    @{ Json = '{"BackendState":"Stopped"}'; State = 'Stopped' }
    @{ Json = '{"BackendState":"NeedsMachineAuth"}'; State = 'NeedsApproval' }
    @{ Json = '{"BackendState":"NeedsLogin","AuthURL":"https://login.tailscale.com/a/test"}'; State = 'NeedsLogin'; Url = 'https://login.tailscale.com/a/test' }
    @{ Json = '{"BackendState":"NeedsLogin","AuthURL":"https://login.tailscale.com.example.test/a/test"}'; State = 'NeedsLogin' }
    @{ Json = '{"BackendState":"NeedsLogin","AuthURL":"http://login.tailscale.com/a/test"}'; State = 'NeedsLogin' }
    @{ Json = '{"BackendState":"Running","AuthURL":"https://login.tailscale.com/a/test"}'; State = 'Connected' }
    @{ Json = 'null'; State = 'Unknown' }
)) {
    $actual = $parse.Invoke($null, @($case.Json))
    if ($actual.State.ToString() -cne $case.State -or $actual.TailnetName -cne $case.Tailnet -or
        $actual.LoginUri?.AbsoluteUri -cne $case.Url) {
        throw "Unexpected result for expected state '$($case.State)'."
    }
}

'Tailscale setup state and login URL tests passed.'
