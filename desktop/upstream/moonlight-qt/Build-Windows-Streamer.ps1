#Requires -Version 7.3

param(
    [string] $Qmake = 'qmake',
    [string] $SourceDirectory
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    throw 'Run from the Visual Studio x64 developer shell.'
}
$revision = 'e7a2d9f44d7b43fe7f6d2e0f08bf55b7cca48818'
$qt = (Get-Command $Qmake -CommandType Application).Source
$qtBin = Split-Path $qt -Parent
$deployQt = Join-Path $qtBin 'windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployQt)) { throw 'The selected Qt must provide windeployqt.' }
if (-not $env:VCToolsRedistDir) { throw 'The MSVC x64 app-local runtime is required.' }
$crt = Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Select-Object -First 1
if (-not $crt) { throw 'The MSVC x64 app-local runtime is required.' }

$desktop = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $desktop ('obj/native-windows/' + [Guid]::NewGuid().ToString('N'))
$build = Join-Path $work 'build'
$deploy = Join-Path $work 'Streamer'
$null = New-Item -ItemType Directory -Force -Path $build, $deploy
if (-not $SourceDirectory) {
    $SourceDirectory = Join-Path $work 'source'
    git clone --depth 1 --branch v6.0.0 --recurse-submodules https://github.com/moonlight-stream/moonlight-qt.git $SourceDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Fetching Moonlight sources failed.' }
}
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
if ((git -C $source rev-parse HEAD) -ne $revision -or $LASTEXITCODE -ne 0) { throw 'Unexpected Moonlight source revision.' }
git -C $source diff --quiet HEAD
if ($LASTEXITCODE -ne 0) { throw 'Use an unmodified Moonlight v6.0.0 source checkout.' }
$patches = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.patch' |
    Where-Object Name -NotLike '*-macos-*' | Sort-Object Name
foreach ($patch in $patches) {
    git -C $source apply $patch.FullName
    if ($LASTEXITCODE -ne 0) { throw "Cannot apply $($patch.Name) to the Moonlight source." }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'AuroraStreamEntry.h') -Destination (Join-Path $source 'app')
Copy-Item -LiteralPath (Join-Path $desktop 'Assets/Aurora.ico') -Destination (Join-Path $source 'app/moonlight.ico') -Force

$previousPath = $env:PATH
$env:PATH = "$qtBin;$previousPath"
try {
    Push-Location -LiteralPath $build
    try {
        & $qt (Join-Path $source 'moonlight-qt.pro') 'CONFIG+=no_batch' 'DEFINES+=AURORA_STREAM_RUNTIME'
        if ($LASTEXITCODE -ne 0) { throw 'qmake failed.' }
        & (Join-Path $source 'scripts/jom.exe') -j ([Math]::Min(8, [Environment]::ProcessorCount)) release
        if ($LASTEXITCODE -ne 0) { throw 'Compiling AuroraStream failed.' }
    }
    finally { Pop-Location }

    $executable = Join-Path $deploy 'AuroraStream.exe'
    Copy-Item -LiteralPath (Join-Path $build 'app/release/AuroraStream.exe') -Destination $executable
    Copy-Item -Path (Join-Path $source 'libs/windows/lib/x64/*.dll') -Destination $deploy
    Copy-Item -LiteralPath (Join-Path $build 'AntiHooking/release/AntiHooking.dll'),
        (Join-Path $source 'app/SDL_GameControllerDB/gamecontrollerdb.txt') -Destination $deploy
    $qtVersion = & $qt -query QT_VERSION
    if ($qtVersion -like '5.*') {
        Copy-Item -LiteralPath (Join-Path $source 'app/qt_qt5.conf') -Destination (Join-Path $deploy 'qt.conf')
    }
    & $deployQt --release --qmldir (Join-Path $source 'app/gui') --dir $deploy --no-opengl-sw --no-sql --no-compiler-runtime $executable
    if ($LASTEXITCODE -ne 0) { throw 'Deploying Qt failed.' }
    Get-ChildItem -LiteralPath $crt.FullName -File -Filter '*.dll' | Copy-Item -Destination $deploy
    $launch = [Diagnostics.ProcessStartInfo]::new($executable)
    $launch.UseShellExecute = $false
    $launch.RedirectStandardOutput = $true
    $launch.CreateNoWindow = $true
    $launch.ArgumentList.Add('--aurora-runtime-info')
    $probe = [Diagnostics.Process]::Start($launch)
    try {
        if (-not $probe.WaitForExit(8000)) {
            $probe.Kill()
            throw 'Checking the Aurora streaming entry point timed out.'
        }
        if ($probe.ExitCode -ne 0 -or $probe.StandardOutput.ReadToEnd().Trim() -cne 'aurora-stream/1') {
            throw 'The Aurora streaming entry point is missing.'
        }
    }
    finally { $probe.Dispose() }
}
finally { $env:PATH = $previousPath }

$notices = Join-Path $deploy 'licenses'
$null = New-Item -ItemType Directory -Path $notices
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $notices 'Moonlight-Qt.txt')
Copy-Item -LiteralPath (@($patches.FullName) + (Join-Path $PSScriptRoot 'AuroraStreamEntry.h')) -Destination $notices
@('https://github.com/moonlight-stream/moonlight-qt', $revision) + @(git -C $source submodule status --recursive) |
    Set-Content -LiteralPath (Join-Path $notices 'SOURCE.txt') -Encoding UTF8
Get-ChildItem -LiteralPath $source -Recurse -File -Include 'LICENSE*', 'COPYING*', 'NOTICE*' | ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($source, $_.FullName)
    $target = Join-Path $notices $relative
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)
    Copy-Item -LiteralPath $_.FullName -Destination $target
}
Get-ChildItem -Path (Join-Path $qtBin '../LICENSE*') -ErrorAction SilentlyContinue | Copy-Item -Destination $notices -Recurse
Get-Item -LiteralPath $deploy
