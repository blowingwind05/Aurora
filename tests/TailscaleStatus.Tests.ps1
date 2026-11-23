#Requires -Version 7.3

param([Parameter(Mandatory)][string] $AssemblyPath)

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

$upstream = $assembly.GetType('Aurora.Upstream', $true)
$binding = [Reflection.BindingFlags]'Static,NonPublic'
$parseProfiles = $upstream.GetMethod('ParseTailscaleProfiles', $binding)
$profiles = @($parseProfiles.Invoke($null, @( @'
[
  {"id":"a8b1","nickname":"Personal","account":"alex@example.test","tailnet":"home.example.test","selected":true},
  {"id":"db21","nickname":"Personal","account":"alex@example.test","tailnet":"work.example.test","selected":false},
  {"id":"db21","nickname":"Duplicate","account":"alex@example.test","selected":false},
  {"id":"--remove","account":"unexpected@example.test"},
  {"id":"empty","account":""}
]
'@ )))
if ($profiles.Count -ne 2 -or $profiles[0].Id -ne 'a8b1' -or !$profiles[0].IsCurrent -or
    $profiles[1].Id -ne 'db21' -or $profiles[1].IsCurrent -or $profiles[0].ToString() -eq $profiles[1].ToString()) {
    throw 'The system account list must identify and distinguish saved profiles by ID and tailnet.'
}

$parseLegacy = $upstream.GetMethod('ParseLegacyTailscaleProfiles', $binding)
$table = @'
ID    Tailnet            Account
a8b1  home.example.test  Personal device*
db21  work.example.test  alex@example.test
'@
$legacy = @($parseLegacy.Invoke($null, @($table)))
if ($legacy.Count -ne 2 -or !$legacy[0].IsCurrent -or $legacy[0].LoginName -ne 'Personal device' -or
    $legacy[1].Tailnet -ne 'work.example.test' -or $legacy[1].IsCurrent) {
    throw 'The legacy account table parser did not preserve account names, tailnets or selection.'
}

'Tailscale system profile list tests passed.'
