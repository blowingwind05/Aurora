#Requires -Version 7.3

param(
    [string] $Msys2Root = 'C:\msys64',
    [string] $SourceDirectory,
    [string] $WorkDirectory
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'Build AuroraHost on Windows x64 with MSYS2 UCRT64.'
}

$ucrt = Join-Path $Msys2Root 'ucrt64'
$ucrtBin = Join-Path $ucrt 'bin'
$gcc = Join-Path $ucrtBin 'gcc.exe'
$gxx = Join-Path $ucrtBin 'g++.exe'
$cmake = Join-Path $ucrtBin 'cmake.exe'
$ninja = Join-Path $ucrtBin 'ninja.exe'
$objdump = Join-Path $ucrtBin 'objdump.exe'
foreach ($tool in @($gcc, $gxx, $cmake, $ninja, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Missing Sunshine build dependency $tool. Use an MSYS2 UCRT64 installation with the upstream Windows build dependencies."
    }
}
if ([version](& $gcc -dumpfullversion) -lt [version]'14.0') {
    throw 'Sunshine requires GCC 14 or newer from MSYS2 UCRT64.'
}
$git = (Get-Command git.exe -CommandType Application -TotalCount 1).Source

function Invoke-BuildTool([string] $Tool, [string[]] $ToolArguments, [string] $Failure) {
    & $Tool @ToolArguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Failure (exit $LASTEXITCODE)." }
}

$desktop = Split-Path -Parent $PSScriptRoot
$workspace = if ($WorkDirectory) { [IO.Path]::GetFullPath($WorkDirectory) } else { Join-Path $desktop 'obj/native-windows' }
$work = Join-Path $workspace ('sunshine-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$source = Join-Path $work 'src'
$build = Join-Path $work 'build'
$stage = Join-Path $work 'stage'
$deploy = Join-Path $work 'Sunshine'
$null = New-Item -ItemType Directory -Force -Path $work


$engine = (Get-Content -LiteralPath (Join-Path $desktop 'upstream/EngineSources.json') -Raw | ConvertFrom-Json).sunshine
$sourceTool = Join-Path $desktop 'upstream/Prepare-EngineSource.py'
$sourceArgs = @('checkout', 'sunshine', 'windows', $source, '--git', $git)
if ($SourceDirectory) { $sourceArgs += @('--origin', $SourceDirectory) }
$virtualHid = Join-Path $source 'third-party/libvirtualhid'
$environment = @{
    PATH = "$ucrtBin;$(Join-Path $Msys2Root 'usr/bin');$env:PATH"
    BRANCH = $engine.upstreamRelease
    BUILD_VERSION = $engine.upstreamRelease
    COMMIT = $null
    GIT_TERMINAL_PROMPT = '0'
    LC_ALL = 'C'
}
$previousEnvironment = @{}
foreach ($name in $environment.Keys) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
    [Environment]::SetEnvironmentVariable($name, $environment[$name])
}
try {
    & python $sourceTool @sourceArgs
    if ($LASTEXITCODE -ne 0) { throw 'Preparing the Sunshine fork failed.' }
    $sourceRevision = & $git -C $source rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or -not $sourceRevision) { throw 'Reading the Sunshine source revision failed.' }
    $env:COMMIT = $sourceRevision.Trim()
    $entrySources = Get-ChildItem -LiteralPath (Join-Path $desktop 'runtime/host') -File -Filter 'AuroraHost*'
    Copy-Item -LiteralPath @($entrySources.FullName) -Destination (Join-Path $source 'src')
    Copy-Item -LiteralPath (Join-Path $desktop 'Assets/Aurora.ico') -Destination (Join-Path $source 'sunshine.ico') -Force
    $bitrateSource = Join-Path $desktop 'protocol/bitrate/HostControl.h'
    Copy-Item -LiteralPath $bitrateSource -Destination (Join-Path $source 'src/aurora_video_bitrate.h')
    $clientProofSource = Join-Path $desktop 'protocol/identity/ClientProof.h'
    Copy-Item -LiteralPath $clientProofSource -Destination (Join-Path $source 'src/aurora_client_proof.h')
    $sourceIndex = Join-Path $desktop 'obj/upstream-sources/sunshine/windows-x64'
    & python $sourceTool index $sourceIndex $source | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Publishing the Sunshine source index failed.' }

    Invoke-BuildTool $cmake @('-S', $source, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja", "-DCMAKE_C_COMPILER:FILEPATH=$gcc", "-DCMAKE_CXX_COMPILER:FILEPATH=$gxx",
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
        '-DBUILD_DOCS=OFF', '-DBUILD_TESTS=OFF', '-DSUNSHINE_ENABLE_TRAY=OFF',
        '-DAURORA_SUNSHINE_HOST=ON', '-DSUNSHINE_ASSETS_DIR=assets',
        '-DSUNSHINE_PUBLISHER_NAME=Aurora') 'Configuring AuroraHost failed'
    Invoke-BuildTool $cmake @('-E', 'env', '--unset=CODECOV_TOKEN', '--unset=GITHUB_REPOSITORY', $cmake,
        '--build', $build, '--parallel', [string][Math]::Min(8, [Environment]::ProcessorCount)) 'Compiling AuroraHost failed'
    Invoke-BuildTool $cmake @('--install', $build, '--strip', '--prefix', $stage) 'Staging the Sunshine runtime failed'

    $required = @('sunshine.exe', 'zlib1.dll', 'assets/apps.json', 'assets/box.png')
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $relative) -PathType Leaf)) {
            throw "The source-built Sunshine runtime is missing $relative."
        }
    }
    if (Test-Path -LiteralPath (Join-Path $stage 'assets/web')) {
        throw 'AuroraHost unexpectedly contains the standalone Sunshine web interface.'
    }
    $null = New-Item -ItemType Directory -Path $deploy
    $executable = Join-Path $deploy 'AuroraHost.exe'
    Copy-Item -LiteralPath (Join-Path $stage 'sunshine.exe') -Destination $executable
    Copy-Item -LiteralPath (Join-Path $stage 'zlib1.dll'), (Join-Path $stage 'assets') -Destination $deploy -Recurse

    foreach ($binary in @($executable, (Join-Path $deploy 'zlib1.dll'))) {
        $header = @(& $objdump -p $binary)
        if ($LASTEXITCODE -ne 0 -or -not ($header -match 'file format pei-x86-64')) {
            throw "Expected an x64 PE binary at $binary."
        }
        foreach ($line in $header) {
            if ($line -notmatch '^\s*DLL Name:\s*(\S+)\s*$') { continue }
            $dll = $Matches[1]
            if ($dll -match '^(api|ext)-ms-win-' -or
                (Test-Path -LiteralPath (Join-Path $deploy $dll) -PathType Leaf) -or
                (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $dll) -PathType Leaf)) { continue }
            throw "Missing runtime dependency $dll required by $binary."
        }
    }

    $runtimeInfo = & $executable --aurora-runtime-info
    if ($LASTEXITCODE -ne 0 -or $runtimeInfo -cne 'aurora-host/1') {
        throw 'The Aurora Host entry point is missing.'
    }

    $notices = Join-Path $deploy 'licenses'
    $null = New-Item -ItemType Directory -Path $notices
    Copy-Item -LiteralPath (@((Join-Path $source 'LICENSE'), $bitrateSource, $clientProofSource) + @($entrySources.FullName)) -Destination $notices
    Copy-Item -LiteralPath (Join-Path $virtualHid 'LICENSES/MIT.md') -Destination (Join-Path $notices 'libvirtualhid-MIT.md')
    Copy-Item -LiteralPath (Join-Path $ucrt 'share/licenses/zlib/LICENSE') -Destination (Join-Path $notices 'zlib-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $source 'AURORA-SOURCE.txt') -Destination (Join-Path $notices 'SOURCE.txt')
    Get-Item -LiteralPath $deploy
}
finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name])
    }
}
