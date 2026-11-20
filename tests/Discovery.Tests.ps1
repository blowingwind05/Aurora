#Requires -Version 7.3

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../src/Tailscale.ps1')

function Assert-Equal {
    param($Actual, $Expected, [string] $Because)

    if ($Actual -cne $Expected) {
        throw "${Because}: expected '$Expected', got '$Actual'."
    }
}

function Assert-Rejected {
    param([scriptblock] $Action, [string] $Message)

    try {
        $null = & $Action
    }
    catch {
        if ($_.Exception.Message -like "*$Message*") {
            return
        }
        throw
    }
    throw "Expected rejection containing '$Message'."
}

$status = @'
{
    "BackendState":"Running",
    "Self":{"ID":"current","UserID":42},
    "Peer":{
        "key1":{"ID":"desk","UserID":42,"HostName":"studio","DNSName":"studio.example.ts.net.","TailscaleIPs":["fd7a:115c:a1e0::123","100.64.2.1"],"Online":true,"OS":"windows"},
        "key2":{"ID":"lap","UserID":42,"HostName":"studio","DNSName":"studio-2.example.ts.net.","TailscaleIPs":["fd7a:115c:a1e0::124"],"Online":false,"OS":"windows"},
        "key3":{"ID":"other","UserID":7,"TailscaleIPs":["100.64.2.3"],"Online":true,"OS":"windows"},
        "key4":{"ID":"tagged","UserID":42,"Tags":["tag:server"],"TailscaleIPs":["100.64.2.4"],"OS":"windows"},
        "key5":{"ID":"shared","UserID":42,"AltSharerUserID":7,"TailscaleIPs":["100.64.2.5"],"OS":"windows"},
        "key6":{"ID":"invalid","UserID":42,"TailscaleIPs":["192.0.2.8","::1"],"OS":"windows"},
        "key7":{"ID":"current","UserID":42,"TailscaleIPs":["100.64.2.7"],"OS":"windows"}
    }
}
'@ | ConvertFrom-Json

$peers = @(Get-AuroraTailscalePeers -Status $status)
Assert-Equal $peers.Count 2 'Only same-user untagged and non-shared peers with a tailnet address'

$desk = Resolve-AuroraTailscalePeer -Peers $peers -Target 'STUDIO.EXAMPLE.TS.NET.'
Assert-Equal $desk.Id 'desk' 'DNS names are matched regardless of case and final dot'
Assert-Equal $desk.Address '100.64.2.1' 'Prefer the verified IPv4 address'
Assert-Equal $desk.Online $true 'Preserve online status'

$lap = Resolve-AuroraTailscalePeer -Peers $peers -Target 'lap'
Assert-Equal $lap.Address 'fd7a:115c:a1e0::124' 'Retain a Tailscale IPv6 fallback'
Assert-Equal $lap.Online $false 'Keep offline peers visible'

Assert-Rejected { Resolve-AuroraTailscalePeer -Peers $peers -Target 'studio' } 'ambiguous'
Assert-Rejected { Resolve-AuroraTailscalePeer -Peers $peers -Target 'invalid' } 'not found'
Assert-Rejected { Resolve-AuroraTailscalePeer -Peers @() -Target 'desk' } 'not found'
Assert-Rejected { Get-AuroraTailscalePeers -Status ([pscustomobject]@{}) } 'Self.UserID'
Assert-Equal @(Get-AuroraTailscalePeers -Status ([pscustomobject]@{ Self = [pscustomobject]@{ UserID = 42 }; Peer = $null })).Count 0 'No peer map is an empty list'

'Discovery tests passed.'
