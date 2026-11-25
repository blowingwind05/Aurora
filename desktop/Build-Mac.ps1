#Requires -Version 7.3

param(
    [ValidateSet('arm64', 'x64')]
    [string] $Architecture = 'arm64',
    [string] $Qmake = 'qmake',
    [string] $TailscaleDirectory,
    [switch] $WithoutStreamer,
    [switch] $WithoutHost
)

$ErrorActionPreference = 'Stop'
if (-not $IsMacOS) { throw 'Build Aurora with the Qt 6 toolchain on macOS.' }

function Invoke-Native {
    $command, $arguments = $args
    & $command @arguments
    if ($LASTEXITCODE -ne 0) { throw "$command failed ($LASTEXITCODE)." }
}

if ($TailscaleDirectory) {
    $TailscaleDirectory = (Resolve-Path -LiteralPath $TailscaleDirectory).Path
    foreach ($file in @('AuroraTailnet', 'licenses/SOURCE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $TailscaleDirectory $file) -PathType Leaf)) {
            throw "The Tailscale build is missing $file."
        }
    }
}

$qmakePath = (Get-Command $Qmake -CommandType Application -TotalCount 1).Source
$repository = Split-Path -Parent $PSScriptRoot
$variant = if ($WithoutStreamer) { 'shell' } elseif ($WithoutHost) { 'controller' } else { 'integrated' }
$output = Join-Path $repository "dist/macos-$Architecture-$variant"
$bundle = Join-Path $output 'Aurora.app'
$zipName = "Aurora-macOS-$Architecture-$variant.zip"
$zip = Join-Path $repository "dist/$zipName"
$plist = Join-Path $PSScriptRoot 'Info.plist'
$work = Join-Path $output ('.build-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Force -Path $work

try {
    $stagedBundle = Join-Path $work 'Aurora.app'
    $stagedZip = Join-Path $work $zipName
    $assemble = @((Join-Path $PSScriptRoot 'Build-NativeDesktop.py'), 'macos',
        '--architecture', $Architecture, '--output', $stagedBundle)
    if ($TailscaleDirectory) {
        $assemble += @('--tailscale-directory', $TailscaleDirectory)
    }

    if ($WithoutStreamer) {
        $nativeArch = if ($Architecture -eq 'x64') { 'x86_64' } else { 'arm64' }
        $qtPrefix = Invoke-Native $qmakePath -query QT_INSTALL_PREFIX
        $qtBin = Invoke-Native $qmakePath -query QT_HOST_BINS
        $minimumOS = Invoke-Native /usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' $plist
        $jobs = Invoke-Native /usr/sbin/sysctl -n hw.logicalcpu
        $launcherBuild = Join-Path $work 'launcher'
        Invoke-Native cmake -S $PSScriptRoot -B $launcherBuild -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DCMAKE_OSX_ARCHITECTURES=$nativeArch" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$minimumOS"
        Invoke-Native cmake --build $launcherBuild --config Release --target Aurora --parallel $jobs
        $assemble += @('--without-streamer', '--qt-bin', $qtBin, '--launcher', (Join-Path $launcherBuild 'bin/Aurora'))
    }
    else {
        $streamerContents = Join-Path $work 'Streamer.app/Contents'
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $streamerContents 'MacOS')
        Copy-Item -LiteralPath $plist -Destination $streamerContents
        Invoke-Native /bin/bash (Join-Path $PSScriptRoot 'upstream/moonlight-qt/Build-Mac-Streamer.sh') `
            $Architecture $streamerContents $qmakePath
        $assemble += @('--streamer-contents', $streamerContents)
        if (-not $WithoutHost) {
            Invoke-Native /bin/bash (Join-Path $PSScriptRoot 'upstream/sunshine/Build-Mac-Sunshine.sh') `
                $Architecture $streamerContents
            $assemble += @('--host-app', (Join-Path $streamerContents 'Helpers/AuroraHost.app'))
        }
    }

    Invoke-Native python3 @assemble | Out-Host
    $contents = Join-Path $stagedBundle 'Contents'
    # Seal Qt and the loose executables before signing the outer app.
    $qtPaths = @('Frameworks', 'PlugIns', 'Resources/qml' | ForEach-Object { Join-Path $contents $_ })
    Invoke-Native /usr/bin/find @qtPaths -type f -name '*.dylib' -exec /usr/bin/codesign --force --sign '-' '{}' '+'
    Invoke-Native /usr/bin/find $qtPaths[0] -depth -type d -name '*.framework' -exec /usr/bin/codesign --force --sign '-' '{}' '+'
    if ($TailscaleDirectory) {
        $tailnet = Join-Path $contents 'MacOS/AuroraTailnet'
        Invoke-Native /bin/chmod +x $tailnet
        Invoke-Native /usr/bin/codesign --force --sign '-' $tailnet
    }
    if (-not $WithoutStreamer) {
        Invoke-Native /usr/bin/codesign --force --sign '-' (Join-Path $contents 'MacOS/AuroraStream')
        if (-not $WithoutHost) {
            # Keep the existing signature from the Sunshine component builder.
            Invoke-Native /usr/bin/codesign --verify --deep --strict (Join-Path $contents 'Helpers/AuroraHost.app')
        }
    }
    Invoke-Native /usr/bin/codesign --force --sign '-' $stagedBundle
    Invoke-Native /usr/bin/codesign --verify --deep --strict $stagedBundle
    Invoke-Native /usr/bin/ditto -c -k --sequesterRsrc --keepParent $stagedBundle $stagedZip
    & (Join-Path $PSScriptRoot 'Artifact-Checksums.ps1') -Path $stagedZip | Out-Null

    if (Test-Path -LiteralPath $bundle) {
        $previous = Join-Path $output ("Aurora-" + [Guid]::NewGuid().ToString('N') + ".app")
        Move-Item -LiteralPath $bundle -Destination $previous
    }
    Move-Item -LiteralPath $stagedBundle -Destination $bundle
    Move-Item -LiteralPath $stagedZip -Destination $zip -Force
    Move-Item -LiteralPath "$stagedZip.sha256" -Destination "$zip.sha256" -Force
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}

Get-Item -LiteralPath $zip
