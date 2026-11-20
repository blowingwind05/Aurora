#Requires -Version 5.1

param(
    [string] $StreamerDirectory,
    [string] $MoonlightDirectory
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'Aurora.csproj'
$variant = if ($StreamerDirectory) { 'dist/Avalonia-0.8-integrated' } else { 'dist/Avalonia-0.8' }
$output = Join-Path (Split-Path -Parent $PSScriptRoot) $variant
if ($StreamerDirectory) {
    if ($MoonlightDirectory) { throw 'Specify only one streaming component.' }
    $StreamerDirectory = (Resolve-Path -LiteralPath $StreamerDirectory).Path
    if (-not (Test-Path -LiteralPath (Join-Path $StreamerDirectory 'AuroraStream.exe') -PathType Leaf)) {
        throw 'StreamerDirectory must contain the integrated AuroraStream.exe and its runtime files.'
    }
}
if ($MoonlightDirectory) {
    $MoonlightDirectory = (Resolve-Path -LiteralPath $MoonlightDirectory).Path
    if (-not (Test-Path -LiteralPath (Join-Path $MoonlightDirectory 'Moonlight.exe') -PathType Leaf)) {
        throw 'MoonlightDirectory must contain the built Aurora-compatible Moonlight.exe and its runtime files.'
    }
}
$env:DOTNET_CLI_HOME = Join-Path $PSScriptRoot 'obj/cli'
$env:NUGET_PACKAGES = Join-Path $PSScriptRoot 'obj/packages'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
dotnet publish $project -c Release -r win-x64 --self-contained true -p:RestoreLockedMode=true -o $output
if ($LASTEXITCODE -ne 0) { throw "Aurora build failed ($LASTEXITCODE)." }
if ($StreamerDirectory -or $MoonlightDirectory) {
    $target = if ($StreamerDirectory) { 'Streamer' } else { 'Moonlight' }
    $source = if ($StreamerDirectory) { $StreamerDirectory } else { $MoonlightDirectory }
    $bundled = Join-Path $output $target
    $null = New-Item -ItemType Directory -Force -Path $bundled
    Get-ChildItem -LiteralPath $source -Force |
        Where-Object Name -notin @('portable.dat', 'Moonlight Game Streaming Project') |
        Copy-Item -Destination $bundled -Recurse -Force
}
Get-Item -LiteralPath (Join-Path $output 'Aurora.exe')
