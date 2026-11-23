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

$repository = 'https://github.com/SunshineStream/Sunshine.git'
$tag = 'v2026.914.233613'
$revision = '63d35f702ee9e362e43263742981836ec0710384'
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
$node = (Get-Command node.exe -CommandType Application -TotalCount 1).Source
$nodeDir = Split-Path -Parent $node
$npm = Join-Path $nodeDir 'npm.cmd'
$nativeNode = & $node -p "process.arch === 'x64' && process.platform === 'win32' && process.config.variables.node_target_type !== 'shared_library'"
if ($nativeNode -ne 'true' -or -not (Test-Path -LiteralPath $npm -PathType Leaf)) {
    throw 'Sunshine requires native Windows x64 Node.js with npm.cmd for the Web UI build.'
}
$git = (Get-Command git.exe -CommandType Application -TotalCount 1).Source

function Invoke-BuildTool([string] $Tool, [string[]] $ToolArguments, [string] $Failure) {
    & $Tool @ToolArguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Failure (exit $LASTEXITCODE)." }
}

$desktop = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workspace = if ($WorkDirectory) { [IO.Path]::GetFullPath($WorkDirectory) } else { Join-Path $desktop 'obj/native-windows' }
$work = Join-Path $workspace ('sunshine-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$source = Join-Path $work 'src'
$build = Join-Path $work 'build'
$stage = Join-Path $work 'stage'
$deploy = Join-Path $work 'Sunshine'
$null = New-Item -ItemType Directory -Force -Path $work

$origin = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { $repository }
$patches = foreach ($name in @('0001-aurora-managed-windows-host.patch', '0003-aurora-wallpaper.patch',
    '0004-aurora-windows-display-privacy.patch', '0005-aurora-private-admin.patch',
    '0006-aurora-pair-source-port.patch',
    '0008-aurora-background-encoder-probe.patch', '0009-aurora-device-power.patch',
    '0010-aurora-windows-privacy-saver.patch', '0011-aurora-windows-session-privacy.patch')) {
    $target = Join-Path $work $name
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $target
    $target
}
$virtualHidPatchName = '0007-libvirtualhid-skip-missing-broker.patch'
$virtualHidPatch = Join-Path $work $virtualHidPatchName
Copy-Item -LiteralPath (Join-Path $PSScriptRoot $virtualHidPatchName) -Destination $virtualHidPatch
$virtualHid = Join-Path $source 'third-party/libvirtualhid'
$environment = @{
    PATH = "$ucrtBin;$nodeDir;$(Join-Path $Msys2Root 'usr/bin');$env:PATH"
    BRANCH = $tag
    BUILD_VERSION = $tag
    COMMIT = $revision
    GIT_TERMINAL_PROMPT = '0'
    LC_ALL = 'C'
    npm_config_cache = (Join-Path $work 'npm-cache')
    npm_config_userconfig = (Join-Path $work '.npmrc')
    npm_config_globalconfig = (Join-Path $work 'npm-global.conf')
}
$previousEnvironment = @{}
foreach ($name in $environment.Keys) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
    [Environment]::SetEnvironmentVariable($name, $environment[$name])
}
try {
    Invoke-BuildTool $git @('-c', 'core.longpaths=true', '-c', 'credential.helper=', 'clone', '--no-local', '--depth', '1',
        '--branch', $tag, '--', $origin, $source) 'Fetching Sunshine source failed'
    $head = & $git -C $source rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -cne $revision) { throw 'Unexpected Sunshine source revision.' }
    $submodules = @('-c', 'core.longpaths=true', '-c', 'credential.helper=', '-C', $source, 'submodule', 'update',
        '--init', '--depth', '1', '--jobs', '8')
    Invoke-BuildTool $git $submodules 'Fetching pinned Sunshine submodules failed'
    # Upstream downloads FFmpeg libraries from the build-deps tag. Its own FFmpeg
    # and Linux packaging checkouts are not used to compile Sunshine on Windows.
    $nested = @('Simple-Web-Server', 'ViGEmClient', 'glad', 'libdisplaydevice', 'libvirtualhid',
        'lizardbyte-common', 'moonlight-common-c', 'nvapi', 'tray') | ForEach-Object { "third-party/$_" }
    Invoke-BuildTool $git ($submodules + @('--recursive', '--') + $nested) 'Fetching pinned Windows dependencies failed'
    $modules = @(& $git -C $source submodule status --recursive)
    if ($LASTEXITCODE -ne 0 -or ($modules | Where-Object {
        $_ -notmatch '^ [0-9a-f]{40} ' -and
        $_ -notmatch '^-[0-9a-f]{40} (third-party/build-deps/|third-party/doxyconfig/|packaging/linux/)'
    })) {
        throw 'Sunshine submodules must match the source revision.'
    }
    foreach ($patch in $patches) {
        Invoke-BuildTool $git @('-C', $source, 'apply', '--', $patch) 'Applying the Aurora host patch failed'
    }
    Invoke-BuildTool $git @('-C', $virtualHid, 'apply', '--', $virtualHidPatch) 'Applying the libvirtualhid client patch failed'
    Copy-Item -LiteralPath (Join-Path $desktop 'Assets/Aurora.ico') -Destination (Join-Path $source 'sunshine.ico') -Force

    Invoke-BuildTool $cmake @('-S', $source, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja", "-DCMAKE_C_COMPILER:FILEPATH=$gcc", "-DCMAKE_CXX_COMPILER:FILEPATH=$gxx",
        "-DNPM:FILEPATH=$npm", '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
        '-DBUILD_DOCS=OFF', '-DBUILD_TESTS=OFF', '-DSUNSHINE_ENABLE_TRAY=OFF',
        '-DAURORA_SUNSHINE_HOST=ON', '-DSUNSHINE_ASSETS_DIR=assets',
        '-DSUNSHINE_PUBLISHER_NAME=Aurora') 'Configuring AuroraHost failed'
    Invoke-BuildTool $cmake @('-E', 'env', '--unset=CODECOV_TOKEN', '--unset=GITHUB_REPOSITORY', $cmake,
        '--build', $build, '--parallel', [string][Math]::Min(8, [Environment]::ProcessorCount)) 'Compiling AuroraHost failed'
    Invoke-BuildTool $cmake @('--install', $build, '--strip', '--prefix', $stage) 'Staging the Sunshine runtime failed'

    $required = @('sunshine.exe', 'zlib1.dll', 'assets/apps.json', 'assets/web/index.html', 'assets/web/welcome.html')
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $relative) -PathType Leaf)) {
            throw "The source-built Sunshine runtime is missing $relative."
        }
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

    $notices = Join-Path $deploy 'licenses'
    $null = New-Item -ItemType Directory -Path $notices
    Copy-Item -LiteralPath (@((Join-Path $source 'LICENSE'), $virtualHidPatch) + $patches) -Destination $notices
    Copy-Item -LiteralPath (Join-Path $virtualHid 'LICENSES/MIT.md') -Destination (Join-Path $notices 'libvirtualhid-MIT.md')
    Copy-Item -LiteralPath (Join-Path $ucrt 'share/licenses/zlib/LICENSE') -Destination (Join-Path $notices 'zlib-LICENSE.txt')
    @($repository, $tag, $revision) + $modules |
        Set-Content -LiteralPath (Join-Path $notices 'SOURCE.txt') -Encoding utf8
    Get-Item -LiteralPath $deploy
}
finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name])
    }
}
