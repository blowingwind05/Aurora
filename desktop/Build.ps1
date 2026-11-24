#Requires -Version 5.1

param(
    [string] $StreamerDirectory,
    [string] $MoonlightDirectory,
    [string] $SunshineDirectory,
    [string] $TailscaleDirectory,
    [string] $GamepadInstaller,
    [switch] $Setup,
    [string] $InnoSetupCompiler
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'Aurora.csproj'
$gamepadInstallerName = 'ViGEmBus_1.22.0_x64_x86_arm64.exe'
$gamepadInstallerUrl = "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/$gamepadInstallerName"
$gamepadInstallerSha256 = '89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A'
if (($Setup -or $GamepadInstaller) -and -not $SunshineDirectory) {
    throw 'Specify SunshineDirectory when building Setup or bundling ViGEmBus.'
}
if ($Setup) {
    if (-not $InnoSetupCompiler) {
        $compiler = Get-Command ISCC.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($compiler) { $InnoSetupCompiler = $compiler.Source }
        else {
            $InnoSetupCompiler = @(
                "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe"
                "$env:ProgramFiles/Inno Setup 6/ISCC.exe"
            ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        }
    }
    if (-not $InnoSetupCompiler -or -not (Test-Path -LiteralPath $InnoSetupCompiler -PathType Leaf)) {
        throw 'Inno Setup compiler not found. Pass -InnoSetupCompiler with the path to ISCC.exe.'
    }
    $InnoSetupCompiler = (Resolve-Path -LiteralPath $InnoSetupCompiler).Path
}
$variant = if ($SunshineDirectory -or $TailscaleDirectory) { 'dist/Avalonia-0.8-source-integrated' }
    elseif ($StreamerDirectory) { 'dist/Avalonia-0.8-integrated' } else { 'dist/Avalonia-0.8' }
$output = Join-Path (Split-Path -Parent $PSScriptRoot) $variant
if (($StreamerDirectory -or $SunshineDirectory -or $TailscaleDirectory) -and (Test-Path -LiteralPath $output)) {
    $output += '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
}
if ($StreamerDirectory) {
    if ($MoonlightDirectory) { throw 'Specify only one streaming component.' }
    $StreamerDirectory = (Resolve-Path -LiteralPath $StreamerDirectory).Path
    $requiredStreamerFiles = @('AuroraStream.exe', 'AuroraQtShell.exe', 'QtPreview/desktop/qt-shell/qml/qmldir',
        'QtPreview/desktop/qt-shell/bridge/Live.qml', 'QtPreview/desktop/qt-shell/bridge/WindowsSessionPopup.qml')
    foreach ($file in $requiredStreamerFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $StreamerDirectory $file) -PathType Leaf)) {
            throw "StreamerDirectory is missing $file. Rebuild it with Build-Windows-Streamer.ps1."
        }
    }
}
if ($MoonlightDirectory) {
    $MoonlightDirectory = (Resolve-Path -LiteralPath $MoonlightDirectory).Path
    if (-not (Test-Path -LiteralPath (Join-Path $MoonlightDirectory 'Moonlight.exe') -PathType Leaf)) {
        throw 'MoonlightDirectory must contain the built Aurora-compatible Moonlight.exe and its runtime files.'
    }
}
$components = @()
if ($SunshineDirectory) {
    $components += @{ Name = 'Sunshine'; Directory = $SunshineDirectory; Files = @('AuroraHost.exe', 'assets/apps.json', 'licenses/SOURCE.txt') }
}
if ($TailscaleDirectory) {
    $components += @{ Name = 'Tailscale'; Directory = $TailscaleDirectory; Files = @('AuroraTailnet.exe', 'licenses/SOURCE.txt') }
}
foreach ($component in $components) {
    $component.Directory = (Resolve-Path -LiteralPath $component.Directory).Path
    foreach ($file in $component.Files) {
        if (-not (Test-Path -LiteralPath (Join-Path $component.Directory $file) -PathType Leaf)) {
            throw "$($component.Name) source build is missing $file."
        }
    }
}
if ($SunshineDirectory) {
    if ($GamepadInstaller) {
        $GamepadInstaller = (Resolve-Path -LiteralPath $GamepadInstaller).Path
    } else {
        $GamepadInstaller = Join-Path $PSScriptRoot "obj/ViGEmBus/$gamepadInstallerName"
        if (-not (Test-Path -LiteralPath $GamepadInstaller -PathType Leaf)) {
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $GamepadInstaller) -Force
            $temporaryInstaller = $GamepadInstaller + '.' + [Guid]::NewGuid().ToString('N') + '.download'
            try {
                $ProgressPreference = 'SilentlyContinue'
                Invoke-WebRequest -UseBasicParsing -Uri $gamepadInstallerUrl -OutFile $temporaryInstaller -TimeoutSec 90
                if ((Get-FileHash -LiteralPath $temporaryInstaller -Algorithm SHA256).Hash -ne $gamepadInstallerSha256) {
                    throw 'The downloaded ViGEmBus installer failed SHA-256 verification.'
                }
                Move-Item -LiteralPath $temporaryInstaller -Destination $GamepadInstaller -Force
            } finally {
                if (Test-Path -LiteralPath $temporaryInstaller -PathType Leaf) {
                    Remove-Item -LiteralPath $temporaryInstaller
                }
            }
        }
    }
    if ((Get-FileHash -LiteralPath $GamepadInstaller -Algorithm SHA256).Hash -ne $gamepadInstallerSha256) {
        throw 'The ViGEmBus installer failed SHA-256 verification. Supply an unmodified official 1.22.0 installer.'
    }
}
$env:DOTNET_CLI_HOME = Join-Path $PSScriptRoot 'obj/cli'
if (-not $env:NUGET_PACKAGES) { $env:NUGET_PACKAGES = Join-Path $PSScriptRoot 'obj/packages' }
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$publishArguments = @($project, '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true', '-p:RestoreLockedMode=true', '-o', $output)
dotnet publish @publishArguments
if ($LASTEXITCODE -ne 0) { throw "Aurora build failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'LICENSE') -Destination $output -Force
if ($StreamerDirectory -or $MoonlightDirectory) {
    $target = if ($StreamerDirectory) { 'Components/Moonlight' } else { 'Moonlight' }
    $source = if ($StreamerDirectory) { $StreamerDirectory } else { $MoonlightDirectory }
    $bundled = Join-Path $output $target
    $null = New-Item -ItemType Directory -Force -Path $bundled
    $excluded = @('portable.dat', 'Moonlight Game Streaming Project')
    if ($StreamerDirectory) { $excluded += 'QtPreview' }
    Get-ChildItem -LiteralPath $source -Force |
        Where-Object Name -notin $excluded |
        Copy-Item -Destination $bundled -Recurse -Force
    if ($StreamerDirectory) {
        Copy-Item -LiteralPath (Join-Path $StreamerDirectory 'QtPreview') -Destination $output -Recurse
    }
}
foreach ($component in $components) {
    $target = Join-Path $output ('Components/' + $component.Name)
    $null = New-Item -ItemType Directory -Force -Path $target
    if ($component.Name -eq 'Tailscale') {
        Copy-Item -LiteralPath (Join-Path $component.Directory 'AuroraTailnet.exe'), (Join-Path $component.Directory 'licenses') -Destination $target -Recurse -Force
    } else {
        Get-ChildItem -LiteralPath $component.Directory -Force |
            Copy-Item -Destination $target -Recurse -Force
    }
}
if ($SunshineDirectory) {
    $gamepadDirectory = Join-Path $output 'Components/ViGEmBus'
    $null = New-Item -ItemType Directory -Force -Path $gamepadDirectory
    $bundledInstaller = Join-Path $gamepadDirectory $gamepadInstallerName
    Copy-Item -LiteralPath $GamepadInstaller -Destination $bundledInstaller
    if ((Get-FileHash -LiteralPath $bundledInstaller -Algorithm SHA256).Hash -ne $gamepadInstallerSha256) {
        Remove-Item -LiteralPath $bundledInstaller
        throw 'The bundled ViGEmBus installer failed SHA-256 verification.'
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses/ViGEmBus.LICENSE.txt') -Destination (Join-Path $gamepadDirectory 'LICENSE.txt')
}
Get-Item -LiteralPath (Join-Path $output 'Aurora.exe')
if ($Setup) {
    $setupOutput = $output + '-setup'
    $icon = Join-Path $PSScriptRoot 'Assets/Aurora.ico'
    $script = Join-Path $PSScriptRoot 'AuroraSetup.iss'
    & $InnoSetupCompiler "/DAuroraSourceDir=$output" "/DAuroraIcon=$icon" "/O$setupOutput" $script
    if ($LASTEXITCODE -ne 0) { throw "Aurora Setup compilation failed ($LASTEXITCODE)." }
    Get-Item -LiteralPath (Join-Path $setupOutput 'AuroraSetup.exe')
}
