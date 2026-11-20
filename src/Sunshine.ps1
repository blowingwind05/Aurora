function Get-AuroraSunshineStatus {
    $service = Get-Service -Name SunshineService -ErrorAction SilentlyContinue
    $process = Get-Process -Name sunshine -ErrorAction SilentlyContinue | Select-Object -First 1
    $installed = $null -ne $service -or $null -ne $process
    if (-not $installed) {
        foreach ($directory in $env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) {
            if ($directory -and (Test-Path -LiteralPath (Join-Path $directory 'Sunshine\sunshine.exe') -PathType Leaf)) {
                $installed = $true
                break
            }
        }
    }

    $portOpen = $false
    if ($process) {
        foreach ($address in [System.Net.IPAddress]::Loopback, [System.Net.IPAddress]::IPv6Loopback) {
            $socket = [System.Net.Sockets.TcpClient]::new($address.AddressFamily)
            try {
                $connection = $socket.ConnectAsync($address, 47990)
                $portOpen = $connection.Wait(200) -and $socket.Connected
            }
            catch {
                $portOpen = $false
            }
            finally {
                $socket.Dispose()
            }
            if ($portOpen) {
                break
            }
        }
    }

    [pscustomobject]@{
        Installed = $installed
        SettingsAvailable = $portOpen
    }
}

function Start-AuroraSunshineInstall {
    $winget = Get-Command -Name 'winget.exe' -CommandType Application -TotalCount 1 -ErrorAction SilentlyContinue
    if (-not $winget) {
        throw '找不到 Windows 应用安装程序（WinGet）。请更新 Microsoft Store 里的「应用安装程序」，或者用官方发布页安装。'
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new($winget.Path)
    $startInfo.UseShellExecute = $true
    foreach ($argument in 'install', '--id', 'LizardByte.Sunshine', '--exact', '--source', 'winget', '--interactive') {
        $startInfo.ArgumentList.Add($argument)
    }
    return [System.Diagnostics.Process]::Start($startInfo)
}
