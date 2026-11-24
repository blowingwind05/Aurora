# SPDX-License-Identifier: GPL-3.0-only
#Requires -Version 7.3
param(
    [Parameter(Mandatory)] [string] $QtShell,
    [Parameter(Mandatory)] [string] $QtBin,
    [Parameter(Mandatory)] [string] $SdlDll,
    [string] $OfflinePackages
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$QtShell = (Resolve-Path -LiteralPath $QtShell).Path
$QtBin = (Resolve-Path -LiteralPath $QtBin).Path
$SdlDll = (Resolve-Path -LiteralPath $SdlDll).Path
$output = Join-Path $PSScriptRoot 'obj'
$settings = @{
    DOTNET_CLI_HOME = (Join-Path $output 'dotnet')
    DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
    DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = 'false'
    DOTNET_NOLOGO = 'true'
    DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    AVALONIA_TELEMETRY_OPTOUT = '1'
    AURORA_REPLAY_FAILURE_LOG = (Join-Path $output ('native-session-failures-' + [guid]::NewGuid().ToString('N') + '.log'))
    QT_QPA_PLATFORM = 'windows'
    QT_QUICK_BACKEND = 'software'
    QT_QUICK_CONTROLS_STYLE = 'Fusion'
    QT_FORCE_STDERR_LOGGING = '1'
    QML_DISABLE_DISK_CACHE = '1'
    PATH = "$QtBin;$(Split-Path -Parent $SdlDll);$env:PATH"
}
$original = @{}
try {
    foreach ($name in $settings.Keys) {
        $original[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, $settings[$name])
    }
    $buildArgs = @('build', (Join-Path $PSScriptRoot 'Replay.csproj'), '-c', 'Debug',
        '-p:RuntimeIdentifiers=win-x64', '--disable-build-servers')
    if ($OfflinePackages) {
        $cache = (Resolve-Path -LiteralPath $OfflinePackages).Path
        $isolatedPackages = Join-Path $output 'packages'
        $buildArgs += @("-p:RestorePackagesPath=$isolatedPackages", '-p:NuGetAudit=false', '--source', $cache)
    }
    & dotnet @buildArgs
    if ($LASTEXITCODE) { throw 'Windows session replay build failed.' }
    $replay = Join-Path $PSScriptRoot 'bin/Debug/net9.0/WindowsSessionReplay.exe'
    $log = Join-Path $output 'native-session-replay.log'
    & $replay $QtShell (Join-Path $PSScriptRoot 'WindowsSessionPopup.qml') 2>&1 |
        ForEach-Object { "$_" } | Tee-Object -FilePath $log
    if ($LASTEXITCODE) { throw "Windows session replay failed. See $log." }
    foreach ($latency in @('12 ms', '48 ms')) {
        if (!(Select-String -Quiet -SimpleMatch -LiteralPath $log -Pattern "SESSION_REPLAY_LATENCY $latency")) {
            throw "Missing rendered Qt latency $latency. See $log."
        }
    }
}
finally {
    foreach ($name in $original.Keys) {
        [Environment]::SetEnvironmentVariable($name, $original[$name])
    }
}
