#Requires -Version 7.3

param(
    [ValidateSet('arm64', 'x64')]
    [string] $Architecture = 'arm64',
    [string] $Rcodesign,
    [string] $Qmake,
    [string] $TailscaleDirectory,
    [switch] $WithoutStreamer,
    [switch] $WithoutHost
)

$ErrorActionPreference = 'Stop'
if (-not $IsMacOS -and -not $Rcodesign) {
    throw 'Build on macOS or pass -Rcodesign with the path to the rcodesign executable.'
}
if (-not $IsMacOS -and -not $WithoutStreamer) {
    throw 'The integrated macOS streaming component requires a native macOS build. Use -WithoutStreamer only for shell compatibility tests.'
}
$project = Join-Path $PSScriptRoot 'Aurora.csproj'
$repository = Split-Path -Parent $PSScriptRoot
$variant = if ($WithoutStreamer) { 'shell' } elseif ($WithoutHost) { 'controller' } else { 'integrated' }
$output = Join-Path $repository "dist/macos-$Architecture-$variant"
$bundle = Join-Path $output 'Aurora.app'
$contents = Join-Path $bundle 'Contents'
$executableDirectory = Join-Path $contents 'MacOS'
$zip = Join-Path $repository "dist/Aurora-macOS-$Architecture-$variant.zip"
$env:DOTNET_CLI_HOME = Join-Path $PSScriptRoot 'obj/cli'
$env:NUGET_PACKAGES = Join-Path $PSScriptRoot 'obj/packages'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'

if ($TailscaleDirectory) {
    $TailscaleDirectory = (Resolve-Path -LiteralPath $TailscaleDirectory).Path
    foreach ($file in @('AuroraTailnet', 'licenses/SOURCE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $TailscaleDirectory $file) -PathType Leaf)) {
            throw "The Tailscale build is missing $file."
        }
    }
}

if (Test-Path -LiteralPath $bundle) {
    $previous = Join-Path $output ("Aurora-" + [Guid]::NewGuid().ToString('N') + ".app")
    Move-Item -LiteralPath $bundle -Destination $previous
}
dotnet publish $project -c Release -r "osx-$Architecture" --self-contained true -p:RestoreLockedMode=true -p:PublishSingleFile=true -p:EnableSingleFileAnalyzer=false -p:DebugType=None -o $executableDirectory
if ($LASTEXITCODE -ne 0) { throw "macOS publish failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Info.plist') -Destination $contents
$resources = Join-Path $contents 'Resources'
$null = New-Item -ItemType Directory -Force -Path $resources
Copy-Item -LiteralPath (Join-Path $repository 'THIRD-PARTY-NOTICES.txt'), (Join-Path $repository 'LICENSE') -Destination $resources
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Assets/Aurora.icns') -Destination $resources
if ($TailscaleDirectory) {
    $tailnetRuntime = Join-Path $executableDirectory 'AuroraTailnet'
    Copy-Item -LiteralPath (Join-Path $TailscaleDirectory 'AuroraTailnet') -Destination $tailnetRuntime
    $tailnetResources = Join-Path $resources 'Tailscale'
    $null = New-Item -ItemType Directory -Force -Path $tailnetResources
    Copy-Item -LiteralPath (Join-Path $TailscaleDirectory 'licenses') -Destination $tailnetResources -Recurse
    if ($IsMacOS) {
        & /bin/chmod +x $tailnetRuntime
        if ($LASTEXITCODE -ne 0) { throw 'Could not make AuroraTailnet executable.' }
        & /usr/bin/codesign --force --sign - $tailnetRuntime
        if ($LASTEXITCODE -ne 0) { throw 'Signing AuroraTailnet failed.' }
    }
}
if (-not $WithoutStreamer) {
    $builder = Join-Path $PSScriptRoot 'upstream/moonlight-qt/Build-Mac-Streamer.sh'
    & /bin/bash $builder $Architecture $contents $Qmake
    if ($LASTEXITCODE -ne 0) { throw 'The Aurora streaming component could not be built.' }
    & /usr/bin/codesign --force --sign - (Join-Path $executableDirectory 'AuroraStream')
    if ($LASTEXITCODE -ne 0) { throw 'Signing AuroraStream failed.' }
    if (-not $WithoutHost) {
        $hostBuilder = Join-Path $PSScriptRoot 'upstream/sunshine/Build-Mac-Sunshine.sh'
        & /bin/bash $hostBuilder $Architecture $contents
        if ($LASTEXITCODE -ne 0) { throw 'The Aurora Sunshine host could not be built.' }
    }
}

if ($IsMacOS) {
    & /usr/bin/codesign --force --deep --sign - $bundle
    if ($LASTEXITCODE -ne 0) { throw 'Ad-hoc signing failed.' }
    & /usr/bin/codesign --verify --deep --strict $bundle
    if ($LASTEXITCODE -ne 0) { throw 'Signature verification failed.' }
    & /usr/bin/ditto -c -k --keepParent $bundle $zip
    if ($LASTEXITCODE -ne 0) { throw 'Archive creation failed.' }
}
else {
    $signer = (Resolve-Path -LiteralPath $Rcodesign).Path
    & $signer sign $bundle
    if ($LASTEXITCODE -ne 0) { throw 'Ad-hoc signing failed.' }
    $signature = & $signer extract code-directory (Join-Path $executableDirectory 'Aurora')
    if ($LASTEXITCODE -ne 0 -or ($signature -join "`n") -notmatch '\bADHOC\b') { throw 'The executable has no ad-hoc signature.' }
    python (Join-Path $PSScriptRoot 'Pack-Mac.py') $bundle $zip
    if ($LASTEXITCODE -ne 0) { throw 'Archive creation failed.' }
}
Get-Item -LiteralPath $zip
