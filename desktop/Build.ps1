#Requires -Version 7.3

param(
    [Parameter(Mandatory)][string] $StreamerDirectory,
    [Parameter(Mandatory)][string] $SunshineDirectory,
    [Parameter(Mandatory)][string] $TailscaleDirectory,
    [string] $Qmake = 'qmake',
    [string] $GamepadInstaller,
    [switch] $Setup,
    [string] $InnoSetupCompiler
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or
    -not (Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue)) {
    throw 'Run from the Visual Studio x64 developer shell.'
}

$StreamerDirectory = (Resolve-Path -LiteralPath $StreamerDirectory).Path
$SunshineDirectory = (Resolve-Path -LiteralPath $SunshineDirectory).Path
$TailscaleDirectory = (Resolve-Path -LiteralPath $TailscaleDirectory).Path
$python = (Get-Command python -CommandType Application -TotalCount 1).Source
$cmake = (Get-Command cmake -CommandType Application -TotalCount 1).Source

# Select the same Qt SDK that was used to deploy the streamer.
$qt = (Get-Command $Qmake -CommandType Application -TotalCount 1).Source
$qtVersion = & $qt -query QT_VERSION
if ($LASTEXITCODE -ne 0 -or $qtVersion -notmatch '^[56]\.') { throw 'Select a Qt 5 or Qt 6 qmake.' }
$qtMajor = $qtVersion.Split('.')[0]
$qtPrefix = & $qt -query QT_INSTALL_PREFIX
if ($LASTEXITCODE -ne 0 -or -not $qtPrefix) { throw 'The selected qmake did not return its installation prefix.' }
$qtPackage = Join-Path $qtPrefix "lib/cmake/Qt$qtMajor"
if (-not (Test-Path -LiteralPath (Join-Path $qtPackage "Qt${qtMajor}Config.cmake") -PathType Leaf)) {
    throw "Qt $qtMajor development files were not found under the selected qmake prefix: $qtPrefix"
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

$gamepadInstallerName = 'ViGEmBus_1.22.0_x64_x86_arm64.exe'
$gamepadInstallerUrl = "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/$gamepadInstallerName"
$gamepadInstallerSha256 = '89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A'
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

$repository = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repository 'dist/Aurora-0.8-source-integrated'
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath ($output + '-setup'))) {
    $output += '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
}
# Keep the build path short for MSVC's generated Qt object paths.
$build = Join-Path $PSScriptRoot ('obj/w-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
& $cmake -S $PSScriptRoot -B $build -G 'NMake Makefiles' '-DCMAKE_BUILD_TYPE=Release' `
    "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DQT_DIR=$qtPackage" "-DQt${qtMajor}_DIR=$qtPackage"
if ($LASTEXITCODE -ne 0) { throw 'Configuring the native Aurora launcher failed.' }
if ($Setup) {
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
    if ($cache -notmatch '(?m)^CMAKE_PROJECT_VERSION:[^=]+=(?<version>[0-9]+(?:\.[0-9]+){1,3})\r?$') {
        throw 'Could not read the Aurora project version for Inno Setup.'
    }
    $auroraVersion = $Matches.version
}

& $cmake --build $build --config Release --target Aurora
if ($LASTEXITCODE -ne 0) { throw 'Building the native Aurora launcher failed.' }

$assembler = Join-Path $PSScriptRoot 'Build-NativeDesktop.py'
& $python $assembler windows --launcher (Join-Path $build 'bin/Aurora.exe') `
    --streamer-directory $StreamerDirectory --sunshine-directory $SunshineDirectory `
    --tailscale-directory $TailscaleDirectory --output $output
if ($LASTEXITCODE -ne 0) { throw 'Assembling the native Aurora package failed.' }

$gamepadDirectory = Join-Path $output 'Components/ViGEmBus'
$null = New-Item -ItemType Directory -Force -Path $gamepadDirectory
$bundledInstaller = Join-Path $gamepadDirectory $gamepadInstallerName
Copy-Item -LiteralPath $GamepadInstaller -Destination $bundledInstaller
if ((Get-FileHash -LiteralPath $bundledInstaller -Algorithm SHA256).Hash -ne $gamepadInstallerSha256) {
    Remove-Item -LiteralPath $bundledInstaller
    throw 'The bundled ViGEmBus installer failed SHA-256 verification.'
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses/ViGEmBus.LICENSE.txt') -Destination (Join-Path $gamepadDirectory 'LICENSE.txt')

$checksums = Join-Path $PSScriptRoot 'Artifact-Checksums.ps1'
& $checksums -Path $output | Out-Null
if ($Setup) {
    $setupOutput = $output + '-setup'
    $icon = Join-Path $PSScriptRoot 'Assets/Aurora.ico'
    $script = Join-Path $PSScriptRoot 'AuroraSetup.iss'
    & $InnoSetupCompiler "/DAuroraSourceDir=$output" "/DAuroraIcon=$icon" "/DAuroraVersion=$auroraVersion" "/O$setupOutput" $script
    if ($LASTEXITCODE -ne 0) { throw "Aurora Setup compilation failed ($LASTEXITCODE)." }
    $setupPackage = Join-Path $setupOutput 'AuroraSetup.exe'
    & $checksums -Path $setupPackage | Out-Null
}
Get-Item -LiteralPath (Join-Path $output 'Aurora.exe')
if ($Setup) { Get-Item -LiteralPath $setupPackage }
