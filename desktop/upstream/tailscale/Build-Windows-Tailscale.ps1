#Requires -Version 7.3

param(
    [Parameter(Mandatory)] [string] $SourceDirectory,
    [Parameter(Mandatory)] [string] $GoExecutable,
    [Parameter(Mandatory)] [string] $WintunZip,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [string] $BuildDirectory = (Join-Path $PSScriptRoot '../../obj/aurora-tailscale-build'),
    [string] $CachedModuleDownloads,
    [switch] $RunSetupTests
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Build the Windows Tailscale component on Windows.' }

$version = '1.104.7'
$commit = 'cbc74fe48ef1b1649f69a4d9a602319427e25102'
$wintunVersion = '0.14.1'
$wintunHash = '07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51'
$wintunDllHash = 'e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce'
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$GoExecutable = (Resolve-Path -LiteralPath $GoExecutable).Path
$WintunZip = (Resolve-Path -LiteralPath $WintunZip).Path
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$BuildDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)

if ((Get-FileHash -LiteralPath $WintunZip -Algorithm SHA256).Hash -ne $wintunHash) {
    throw "Expected the official pinned Wintun $wintunVersion archive."
}

$resolvedCommit = & git -C $SourceDirectory rev-parse --verify "$commit^{commit}"
if ($LASTEXITCODE -ne 0 -or $resolvedCommit -ne $commit) {
    throw "Tailscale commit $commit is missing from SourceDirectory."
}
$versionOutput = & $GoExecutable version
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch '^go version go1\.27\.4 windows/') {
    throw 'The pinned Tailscale source requires Go 1.27.4.'
}

$job = Join-Path $BuildDirectory ('build-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$source = Join-Path $job 'source'
$component = Join-Path $job 'component'
$licenses = Join-Path $component 'licenses'
$null = New-Item -ItemType Directory -Path $job, $licenses
# A local checkout preserves pinned Git objects and represents upstream symlinks safely on Windows.
& git -c credential.helper= clone --quiet --no-hardlinks --no-checkout -- $SourceDirectory $source
if ($LASTEXITCODE -ne 0) { throw 'Could not create the temporary Tailscale checkout.' }
& git -C $source -c core.longpaths=true -c core.symlinks=false -c core.autocrlf=false -c core.hooksPath=NUL checkout --detach --quiet $commit
if ($LASTEXITCODE -ne 0) { throw 'Could not check out the pinned Tailscale source.' }
$patchNames = @('0001-aurora-protected-windows-state.patch', '0002-aurora-private-windows-service.patch')
foreach ($patchName in $patchNames) {
    & git -C $source apply (Join-Path $PSScriptRoot $patchName)
    if ($LASTEXITCODE -ne 0) { throw "Could not apply $patchName." }
}

$setup = Join-Path $source 'cmd/aurora-tailnet-setup'
$payload = Join-Path $setup 'payload'
$null = New-Item -ItemType Directory -Path $payload
foreach ($name in @('setup_windows.go', 'switch_windows.go', 'parking_windows.go', 'switch_windows_test.go')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $setup
}
$driver = Join-Path $payload 'wintun.dll'
$zip = [IO.Compression.ZipFile]::OpenRead($WintunZip)
try {
    [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry('wintun/bin/amd64/wintun.dll'), $driver)
    [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry('wintun/LICENSE.txt'), (Join-Path $licenses 'Wintun.txt'))
} finally { $zip.Dispose() }
if ((Get-FileHash -LiteralPath $driver -Algorithm SHA256).Hash -ne $wintunDllHash) {
    throw 'The Wintun x64 DLL failed verification.'
}
$signature = Get-AuthenticodeSignature -LiteralPath $driver
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=WireGuard LLC,') {
    throw "The Wintun signature did not verify ($($signature.Status))."
}

$settings = @{
    CGO_ENABLED = '0'
    GOARCH = 'amd64'
    GOAMD64 = 'v1'
    GOOS = 'windows'
    GOENV = 'off'
    GOFLAGS = $null
    GOPATH = (Join-Path $BuildDirectory 'go-path')
    GOCACHE = (Join-Path $BuildDirectory 'go-cache')
    GOMODCACHE = (Join-Path $BuildDirectory 'go-path/pkg/mod')
    GOTOOLCHAIN = 'local'
    GOWORK = 'off'
    GOAUTH = 'off'
    GOEXPERIMENT = $null
    GOPROXY = 'https://proxy.golang.org'
    GOSUMDB = 'sum.golang.org'
    GOPRIVATE = $null
    GONOPROXY = $null
    GONOSUMDB = $null
    GOINSECURE = $null
}
if ($CachedModuleDownloads) {
    $settings.GOPROXY = ([Uri] (Resolve-Path -LiteralPath $CachedModuleDownloads).Path).AbsoluteUri
}
$previous = @{}
Push-Location $source
try {
    foreach ($setting in $settings.GetEnumerator()) {
        $previous[$setting.Key] = [Environment]::GetEnvironmentVariable($setting.Key)
        [Environment]::SetEnvironmentVariable($setting.Key, $setting.Value)
    }
    $flags = "-s -w -X tailscale.com/version.shortStamp=$version -X tailscale.com/version.longStamp=$version-t$($commit.Substring(0, 9)) -X tailscale.com/version.gitCommitStamp=$commit"
    foreach ($command in @('tailscale', 'tailscaled')) {
        & $GoExecutable build -trimpath -mod=readonly -buildvcs=false '-tags=ts_omit_clientupdate,ts_omit_flashappliance' "-ldflags=$flags" -o (Join-Path $payload "$command.exe") "./cmd/$command"
        if ($LASTEXITCODE -ne 0) { throw "Could not build $command." }
    }
    if ($RunSetupTests) {
        & $GoExecutable test -mod=readonly './cmd/aurora-tailnet-setup'
        if ($LASTEXITCODE -ne 0) { throw 'Aurora Tailscale switch tests failed.' }
    }
    & $GoExecutable build -trimpath -mod=readonly -buildvcs=false '-ldflags=-s -w -H=windowsgui' -o (Join-Path $component 'AuroraTailnetSetup.exe') './cmd/aurora-tailnet-setup'
    if ($LASTEXITCODE -ne 0) { throw 'Could not build AuroraTailnetSetup.exe.' }

    $moduleFormat = '{{with .Module}}{{if not .Main}}{{.Path}}{{"\t"}}{{.Version}}{{"\t"}}{{if .Replace}}{{.Replace.Dir}}{{else}}{{.Dir}}{{end}}{{end}}{{end}}'
    $moduleRows = & $GoExecutable list -deps -mod=readonly '-tags=ts_omit_clientupdate,ts_omit_flashappliance' -f $moduleFormat './cmd/tailscale' './cmd/tailscaled' './cmd/aurora-tailnet-setup'
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate Tailscale dependency notices.' }
    $goRoot = & $GoExecutable env GOROOT
    if ($LASTEXITCODE -ne 0) { throw 'Could not locate the Go license.' }
} finally {
    Pop-Location
    foreach ($setting in $previous.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($setting.Key, $setting.Value)
    }
}

Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $licenses 'Tailscale.txt')
Copy-Item -LiteralPath (Join-Path $goRoot 'LICENSE') -Destination (Join-Path $licenses 'Go.txt')
$noticeFile = [IO.StreamWriter]::new((Join-Path $licenses 'Tailscale-dependencies.txt'))
try {
    foreach ($row in ($moduleRows | Where-Object { $_ } | Sort-Object -Unique)) {
        $module, $moduleVersion, $modulePath = $row -split [char]9, 3
        $noticeFile.WriteLine("===== $module $moduleVersion =====")
        $notices = Get-ChildItem -LiteralPath $modulePath -File -Force |
            Where-Object Name -Match '^(LICENSE|COPYING|NOTICE|PATENTS)([._-].*)?$' |
            Sort-Object Name
        if (-not $notices) { $noticeFile.WriteLine('[No root-level license or notice file.]') }
        foreach ($notice in $notices) {
            $noticeFile.WriteLine("--- $($notice.Name) ---")
            $noticeFile.WriteLine([IO.File]::ReadAllText($notice.FullName))
        }
    }
} finally { $noticeFile.Dispose() }

@"
Tailscale $version
https://github.com/tailscale/tailscale/tree/$commit
Source changes: desktop/upstream/tailscale/0001-aurora-protected-windows-state.patch
                desktop/upstream/tailscale/0002-aurora-private-windows-service.patch
Build recipe: desktop/upstream/tailscale/Build-Windows-Tailscale.ps1
Toolchain: Go 1.27.4, windows/amd64 target

Wintun $wintunVersion, original x64 wintun.dll
https://www.wintun.net/builds/wintun-$wintunVersion.zip
Archive SHA-256: $wintunHash
DLL SHA-256: $wintunDllHash
License: Wintun.txt

The Aurora setup executable embeds the unchanged DLL alongside the CLI and daemon
built from the pinned source. Its only Wintun usage is via the published API. Aurora tracks updates to these binaries
as part of its own Windows release.
"@ | Set-Content -LiteralPath (Join-Path $licenses 'SOURCE.txt') -Encoding utf8

if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Use a new output directory to avoid packaging old files: $OutputDirectory"
}
$null = New-Item -ItemType Directory -Path (Split-Path -Parent $OutputDirectory) -Force
Move-Item -LiteralPath $component -Destination $OutputDirectory
Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'AuroraTailnetSetup.exe') -Algorithm SHA256
