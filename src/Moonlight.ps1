function Get-AuroraMoonlightExecutable {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [ValidateNotNullOrEmpty()]
        [string]$Executable
    )

    if ($PSBoundParameters.ContainsKey('Executable')) {
        $file = Get-Item -LiteralPath $Executable -ErrorAction SilentlyContinue
        if ($file -isnot [System.IO.FileInfo] -or $file.Extension -ine '.exe') {
            throw "Moonlight executable is not an existing .exe file: $Executable"
        }
        return $file.FullName
    }

    foreach ($directory in @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($directory) {
            $candidate = Join-Path $directory 'Moonlight Game Streaming\Moonlight.exe'
            $file = Get-Item -LiteralPath $candidate -ErrorAction SilentlyContinue
            if ($file -is [System.IO.FileInfo]) {
                return $file.FullName
            }
        }
    }

    $command = Get-Command -Name 'Moonlight.exe' -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) {
        return $command.Path
    }

    throw 'Moonlight Qt was not found in Program Files or PATH. Pass -Executable with the path to an existing Moonlight.exe.'
}

function Invoke-AuroraMoonlight {
    [CmdletBinding()]
    [OutputType([string], [System.Diagnostics.Process])]
    param(
        [Parameter(Mandatory)]
        [string]$Executable,

        [Parameter(Mandatory)]
        [ValidateSet('Pair', 'List', 'Stream')]
        [string]$Operation,

        [Parameter(Mandatory)]
        [string]$Address,

        [string]$AppName = 'Desktop',

        [switch]$NoWait
    )

    if (-not [System.IO.Path]::IsPathFullyQualified($Executable) -or
        [System.IO.Path]::GetExtension($Executable) -ine '.exe') {
        throw 'Pass the full .exe path from Get-AuroraMoonlightExecutable to -Executable.'
    }

    $peerAddress = $null
    if ($Address -notmatch '\A[0-9a-f:.]+\z' -or
        -not [System.Net.IPAddress]::TryParse($Address, [ref]$peerAddress)) {
        throw 'Address must be an unscoped IPv4 or IPv6 literal without brackets or a port.'
    }

    $hostArgument = $peerAddress.ToString()
    if ($peerAddress.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6) {
        $hostArgument = '[{0}]:47989' -f $hostArgument
    } elseif ($Address -cne $hostArgument) {
        throw 'IPv4 addresses must use dotted-decimal notation without leading zeros.'
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new($Executable)
    $startInfo.UseShellExecute = $false
    $startInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($Executable)
    $startInfo.ArgumentList.Add($Operation.ToLowerInvariant())
    $startInfo.ArgumentList.Add($hostArgument)

    if ($Operation -eq 'Stream') {
        if ([string]::IsNullOrWhiteSpace($AppName) -or $AppName -match '\A-|[\x00\r\n]') {
            throw 'AppName must be a non-blank, single-line app name without NUL or a leading dash.'
        }
        $startInfo.ArgumentList.Add($AppName)
    }

    if ($Operation -eq 'List') {
        if ($NoWait) {
            throw 'List requires waiting for the application list.'
        }
        $startInfo.RedirectStandardOutput = $true
        $startInfo.StandardOutputEncoding = [System.Text.CodePagesEncodingProvider]::Instance.GetEncoding(0) ?? [System.Text.Encoding]::UTF8
    }

    $process = [System.Diagnostics.Process]::Start($startInfo)
    if ($NoWait) {
        return $process
    }
    try {
        if ($startInfo.RedirectStandardOutput) {
            while ($null -ne ($line = $process.StandardOutput.ReadLine())) {
                $line
            }
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "Moonlight $Operation exited with code $($process.ExitCode)."
        }
    } finally {
        $process.Dispose()
    }
}
