function Get-AuroraTailscaleStatus {
    [CmdletBinding()]
    param([ValidateNotNullOrEmpty()][string] $Executable)

    $cli = $null
    if ($PSBoundParameters.ContainsKey('Executable')) {
        $cli = (Get-Command -Name $Executable -CommandType Application -TotalCount 1 -ErrorAction SilentlyContinue)?.Source
        if (-not $cli) {
            throw "Tailscale executable '$Executable' was not found. Pass a valid executable path to -Executable."
        }
    } else {
        foreach ($programFiles in $env:ProgramW6432, $env:ProgramFiles) {
            if ($programFiles) {
                $candidate = Join-Path $programFiles 'Tailscale\tailscale.exe'
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $cli = $candidate
                    break
                }
            }
        }
        if (-not $cli) {
            $cli = (Get-Command -Name tailscale.exe -CommandType Application -TotalCount 1 -ErrorAction SilentlyContinue)?.Source
        }
        if (-not $cli) {
            throw 'Tailscale CLI not found. Check the Windows installation or pass its executable path to -Executable.'
        }
    }

    $PSNativeCommandUseErrorActionPreference = $false
    try {
        $output = & $cli status --json 2>&1
        $exitCode = $LASTEXITCODE
    } catch {
        throw "Could not run '$cli status --json'. Verify the executable path and that your Windows session can run it. $($_.Exception.Message)"
    }
    if ($exitCode -ne 0) {
        $detail = ($output | Where-Object { $_ -is [System.Management.Automation.ErrorRecord] }) -join ' '
        throw "Tailscale status failed (exit $exitCode). Check that the Tailscale Windows service is running and retry from a Windows session authorized to access it. $detail"
    }

    try {
        $json = ($output | Where-Object { $_ -is [string] }) -join "`n"
        $status = ConvertFrom-Json -InputObject $json -NoEnumerate -ErrorAction Stop
    } catch {
        throw "Tailscale returned invalid status JSON. Verify that '$cli' is the official CLI and that 'tailscale status --json' works."
    }
    $state = if ($status -is [pscustomobject]) { $status.PSObject.Properties['BackendState']?.Value }
    if ($state -isnot [string] -or [string]::IsNullOrWhiteSpace($state)) {
        throw 'Tailscale returned invalid status: missing BackendState. Check the CLI version and retry.'
    }
    if ($state -cne 'Running') {
        throw "Tailscale is not Running (state: $state). Open Tailscale, sign in or request device approval if needed, and connect to your tailnet."
    }
    return $status
}

function Get-AuroraTailscalePeers {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object] $Status)

    $self = $Status.PSObject.Properties['Self']?.Value
    $selfUserId = if ($self -is [pscustomobject]) { $self.PSObject.Properties['UserID']?.Value }
    if (($selfUserId -isnot [long] -and $selfUserId -isnot [int]) -or $selfUserId -le 0) {
        throw 'Tailscale status does not contain a positive Self.UserID. Sign in with your Tailscale account and refresh discovery; check or update the CLI if it persists.'
    }
    if ($self.PSObject.Properties['Tags']?.Value) {
        throw 'The local Tailscale device is tagged. Same-user discovery requires an untagged device signed in with your Tailscale account.'
    }

    $peerMap = $Status.PSObject.Properties['Peer']?.Value
    if ($null -eq $peerMap) {
        return
    }
    if ($peerMap -isnot [pscustomobject]) {
        throw 'Tailscale returned an invalid Peer map. Check the CLI version and refresh discovery.'
    }

    $selfId = [string] $self.PSObject.Properties['ID']?.Value
    foreach ($entry in $peerMap.PSObject.Properties) {
        if ($entry.Value -isnot [pscustomobject]) {
            continue
        }
        $peer = $entry.Value.PSObject.Properties
        $peerUserId = $peer['UserID']?.Value
        $id = [string] $peer['ID']?.Value
        if (($peerUserId -isnot [long] -and $peerUserId -isnot [int]) -or $peerUserId -ne $selfUserId -or
            $peer['Tags']?.Value -or $peer['AltSharerUserID']?.Value -or $peer['ShareeNode']?.Value -or
            [string]::IsNullOrWhiteSpace($id) -or $id -ceq $selfId) {
            continue
        }

        $ipv4 = $null
        $ipv6 = $null
        foreach ($candidate in $peer['TailscaleIPs']?.Value) {
            $ip = $null
            if ($candidate -isnot [string] -or $candidate -like '*%*' -or
                -not [System.Net.IPAddress]::TryParse($candidate, [ref] $ip)) {
                continue
            }
            $address = $ip.ToString()
            if ($ip.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork) {
                $bytes = $ip.GetAddressBytes()
                if ($candidate -ceq $address -and $bytes[0] -eq 100 -and ($bytes[1] -band 192) -eq 64 -and
                    $address -notmatch '^100\.(?:100\.(?:0|100)|115\.(?:92|93))\.' -and $address -cne '100.101.102.103') {
                    $ipv4 = $address
                    break
                }
            } elseif (-not $ipv6 -and $address -clike 'fd7a:115c:a1e0:*' -and
                $address -cnotlike 'fd7a:115c:a1e0:b1a:*' -and $address -cne 'fd7a:115c:a1e0::53') {
                $ipv6 = $address
            }
        }
        if (-not ($ipv4 -or $ipv6)) {
            continue
        }

        $online = $peer['Online']?.Value
        [pscustomobject]@{
            Id              = $id
            Name            = [string] $peer['HostName']?.Value
            DnsName         = ([string] $peer['DNSName']?.Value) -replace '\.\z'
            Address         = $ipv4 ?? $ipv6
            Online          = $online -is [bool] -and $online
            OperatingSystem = [string] $peer['OS']?.Value
        }
    }
}

function Resolve-AuroraTailscalePeer {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowNull()][object[]] $Peers,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string] $Target
    )

    $dnsTarget = $Target -replace '\.\z'
    $match = $null
    foreach ($peer in $Peers) {
        if ($null -eq $peer) {
            continue
        }
        if ([string]::Equals($peer.Id, $Target, [StringComparison]::Ordinal) -or
            [string]::Equals($peer.Name, $Target, [StringComparison]::Ordinal) -or
            [string]::Equals($peer.Address, $Target, [StringComparison]::OrdinalIgnoreCase) -or
            ($dnsTarget -and [string]::Equals(($peer.DnsName -replace '\.\z'), $dnsTarget, [StringComparison]::OrdinalIgnoreCase))) {
            if ($null -ne $match) {
                throw "Tailscale target '$Target' is ambiguous. Use a unique device ID, DNS name, or Tailscale address from the device list."
            }
            $match = $peer
        }
    }
    if ($null -eq $match) {
        throw "Tailscale target '$Target' was not found. Refresh discovery and check that the device is untagged, signed in as your Tailscale user, and listed with a Tailscale IP."
    }
    return $match
}
