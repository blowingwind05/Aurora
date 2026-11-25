#Requires -Version 7.3

param(
    [Parameter(Mandatory)] [ValidateSet('win-x64', 'osx-arm64', 'osx-x64')] [string] $Runtime,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [string] $SourceDirectory,
    [string] $GoExecutable = 'go',
    [string] $Windres = 'windres',
    [string] $BuildDirectory = (Join-Path $PSScriptRoot '../obj/aurora-tsnet'),
    [string] $CachedModuleDownloads,
    [switch] $RunTests
)

$ErrorActionPreference = 'Stop'
$version = '1.104.7'
$commit = 'cbc74fe48ef1b1649f69a4d9a602319427e25102'
$go = (Get-Command $GoExecutable -CommandType Application -TotalCount 1).Source
if ((& $go version) -notmatch '^go version go1\.27\.4 ') { throw 'AuroraTailnet requires Go 1.27.4.' }

$windows = $Runtime -eq 'win-x64'
$targetOS = if ($windows) { 'windows' } else { 'darwin' }
$targetArch = if ($Runtime -eq 'osx-arm64') { 'arm64' } else { 'amd64' }
$targetName = if ($windows) { 'AuroraTailnet.exe' } else { 'AuroraTailnet' }
$BuildDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'The output directory already exists.' }
$job = Join-Path $BuildDirectory ('build-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$source = Join-Path $job 'src'
$licenses = Join-Path $job 'licenses'
$null = New-Item -ItemType Directory -Force -Path $job, $licenses

$origin = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { 'https://github.com/tailscale/tailscale.git' }
$hooks = Join-Path $job 'no-hooks'
$clone = @('-c', 'credential.helper=', '-c', "core.hooksPath=$hooks", 'clone', '--quiet', '--no-checkout')
if ($SourceDirectory) { $clone += @('--no-hardlinks') } else { $clone += @('--depth', '1', '--branch', "v$version") }
& git @clone -- $origin $source
if ($LASTEXITCODE -ne 0) { throw 'Could not fetch Tailscale source.' }
& git -C $source -c core.longpaths=true -c core.autocrlf=false -c "core.hooksPath=$hooks" checkout --detach --quiet $commit
if ($LASTEXITCODE -ne 0 -or (& git -C $source rev-parse HEAD) -cne $commit) { throw 'Unexpected Tailscale revision.' }

$package = './cmd/aurora-tailnet'
$packagePath = Join-Path $source $package
$null = New-Item -ItemType Directory -Path $packagePath
Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.go' -File | Copy-Item -Destination $packagePath
if ($windows) {
    $resourceCompiler = (Get-Command $Windres -CommandType Application -TotalCount 1).Source
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../Assets/Aurora.ico') -Destination (Join-Path $packagePath 'Aurora.ico')
    @'
1 ICON "Aurora.ico"
1 VERSIONINFO
FILEVERSION 0,8,0,0
PRODUCTVERSION 0,8,0,0
FILEOS 0x40004L
FILETYPE 1L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "FileDescription", "Aurora Tailnet"
            VALUE "ProductName", "Aurora"
            VALUE "OriginalFilename", "AuroraTailnet.exe"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0409, 1200
    END
END
'@ | Set-Content -LiteralPath (Join-Path $packagePath 'aurora.rc') -Encoding utf8NoBOM
    Push-Location -LiteralPath $packagePath
    try {
        $previousPath = $env:PATH
        $env:PATH = (Split-Path -Parent $resourceCompiler) + [IO.Path]::PathSeparator + $previousPath
        & $resourceCompiler -i aurora.rc -O coff -o aurora_windows_amd64.syso
        if ($LASTEXITCODE -ne 0) { throw 'Compiling the Aurora Windows icon failed.' }
    } finally {
        $env:PATH = $previousPath
        Pop-Location
    }
}

$settings = @{
    CGO_ENABLED = '0'; GOARCH = $targetArch; GOOS = $targetOS; GOAMD64 = 'v1'
    GOENV = 'off'; GOWORK = 'off'; GOTOOLCHAIN = 'local'; GOFLAGS = $null; GOEXPERIMENT = $null
    GOPROXY = 'https://proxy.golang.org'; GOSUMDB = 'sum.golang.org'; GOPRIVATE = $null
    GONOPROXY = $null; GONOSUMDB = $null; GOINSECURE = $null
    GOMODCACHE = (Join-Path $BuildDirectory 'modules'); GOCACHE = (Join-Path $BuildDirectory 'cache')
}
if ($CachedModuleDownloads) {
    $settings.GOPROXY = ([Uri] (Resolve-Path -LiteralPath $CachedModuleDownloads).Path).AbsoluteUri
}
$previous = @{}
$binary = Join-Path $job $targetName
# The embedded node does not own the system client's file-sharing integration or update lifecycle.
$features = '-tags=ts_omit_taildrop,ts_omit_drive,ts_omit_clientupdate,ts_omit_desktop_sessions'
Push-Location -LiteralPath $source
try {
    foreach ($entry in $settings.GetEnumerator()) {
        $previous[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key)
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
    }
    if ($RunTests) {
        & $go test -mod=readonly $features $package
        if ($LASTEXITCODE -ne 0) { throw 'AuroraTailnet tests failed.' }
    }
    $flags = "-s -w -X tailscale.com/version.shortStamp=$version -X tailscale.com/version.gitCommitStamp=$commit"
    if ($windows) { $flags += ' -H=windowsgui' }
    & $go build -trimpath -mod=readonly -buildvcs=false $features "-ldflags=$flags" -o $binary $package
    if ($LASTEXITCODE -ne 0) { throw 'Building AuroraTailnet failed.' }
    $format = '{{with .Module}}{{if not .Main}}{{.Path}}{{"\t"}}{{.Version}}{{"\t"}}{{if .Replace}}{{.Replace.Dir}}{{else}}{{.Dir}}{{end}}{{end}}{{end}}'
    $dependencies = & $go list -deps -mod=readonly $features -f $format $package
    if ($LASTEXITCODE -ne 0) { throw 'Listing Tailscale dependencies failed.' }
    $goRoot = & $go env GOROOT
    if ($LASTEXITCODE -ne 0) { throw 'Could not locate the Go license.' }
} finally {
    Pop-Location
    foreach ($entry in $previous.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
    }
}

Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $licenses 'Tailscale.txt')
Copy-Item -LiteralPath (Join-Path $goRoot 'LICENSE') -Destination (Join-Path $licenses 'Go.txt')
$notices = [IO.StreamWriter]::new((Join-Path $licenses 'Tailscale-dependencies.txt'))
try {
    foreach ($row in ($dependencies | Where-Object { $_ } | Sort-Object -Unique)) {
        $module, $revision, $path = $row -split [char]9, 3
        $notices.WriteLine("===== $module $revision =====")
        Get-ChildItem -LiteralPath $path -File -Force |
            Where-Object Name -Match '^(LICENSE|COPYING|NOTICE|PATENTS)([._-].*)?$' |
            Sort-Object Name | ForEach-Object {
                $notices.WriteLine("--- $($_.Name) ---")
                $notices.WriteLine([IO.File]::ReadAllText($_.FullName))
            }
    }
} finally { $notices.Dispose() }

@"
Tailscale $version ($commit)
https://github.com/tailscale/tailscale/tree/$commit
Aurora sources and build recipe: desktop/tailnet
Toolchain: Go 1.27.4 ($targetOS/$targetArch)
SHA-256: $((Get-FileHash -LiteralPath $binary).Hash)
"@ | Set-Content -LiteralPath (Join-Path $licenses 'SOURCE.txt') -Encoding utf8NoBOM
$null = New-Item -ItemType Directory -Path $OutputDirectory
Copy-Item -LiteralPath $binary, $licenses -Destination $OutputDirectory -Recurse
Get-Item -LiteralPath (Join-Path $OutputDirectory $targetName)
