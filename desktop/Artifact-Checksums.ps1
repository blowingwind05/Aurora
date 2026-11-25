#Requires -Version 5.1

param(
    [Parameter(Mandatory)][string] $Path,
    [switch] $Verify
)

$ErrorActionPreference = 'Stop'
$artifact = Get-Item -LiteralPath $Path -Force
if ($artifact.Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Specify a regular artifact file or directory.'
}

if ($artifact.PSIsContainer) {
    $manifest = Join-Path $artifact.FullName 'SHA256SUMS.txt'
    $entries = @(Get-ChildItem -LiteralPath $artifact.FullName -Recurse -Force)
    if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw 'Archive the package before hashing because it contains filesystem links.'
    }
    $paths = [string[]]@($entries | Where-Object { -not $_.PSIsContainer -and $_.FullName -cne $manifest } |
        ForEach-Object FullName)
    $prefixLength = $artifact.FullName.TrimEnd([IO.Path]::DirectorySeparatorChar).Length + 1
} else {
    $manifest = $artifact.FullName + '.sha256'
    $paths = [string[]]@($artifact.FullName)
    $prefixLength = $artifact.FullName.Length - $artifact.Name.Length
}

if ($paths.Count -eq 0) { throw 'There are no package files to checksum.' }
[Array]::Sort($paths, [StringComparer]::Ordinal)
$lines = foreach ($file in $paths) {
    $relative = $file.Substring($prefixLength).Replace([IO.Path]::DirectorySeparatorChar, '/')
    if ($relative.Contains("`r") -or $relative.Contains("`n") -or $relative.Contains('\')) {
        throw 'The package contains a filename that cannot be written to a SHA-256 manifest.'
    }
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relative"
}

if ($Verify) {
    $expected = [IO.File]::ReadAllLines($manifest)
    $actual = @($lines)
    if ($expected.Length -ne $actual.Length) { throw 'The package file count differs from its SHA-256 manifest.' }
    for ($i = 0; $i -lt $expected.Length; $i++) {
        if ($expected[$i] -cne $actual[$i]) {
            throw "SHA-256 manifest mismatch at entry $($i + 1): $($actual[$i].Substring(66))"
        }
    }
    Write-Output "SHA-256 verified: $($artifact.Name)"
} else {
    [IO.File]::WriteAllText($manifest, ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
    Get-Item -LiteralPath $manifest
}
