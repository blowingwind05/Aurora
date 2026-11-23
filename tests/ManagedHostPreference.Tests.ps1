#Requires -Version 7.3
param([Parameter(Mandatory)][string] $AssemblyPath)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$store = $assembly.GetType('Aurora.ManagedHostPreference', $true)
$methods = [Reflection.BindingFlags]'Instance,NonPublic'
$enabled = $store.GetProperty('Enabled', $methods)
$set = $store.GetMethod('SetEnabled', $methods)
$allInstance = [Reflection.BindingFlags]'Instance,Public,NonPublic'
$directory = [IO.Directory]::CreateTempSubdirectory('aurora-host-consent-')
$marker = Join-Path $directory.FullName 'allow-managed-host'
function Open-Preference([string] $Path) {
    [Activator]::CreateInstance($store, $allInstance, $null, @($Path), $null)
}

try {
    if ($enabled.GetValue((Open-Preference $marker))) { throw 'A new install should not start a host.' }
    $set.Invoke((Open-Preference $marker), @($true))
    if (!$enabled.GetValue((Open-Preference $marker))) { throw 'Enabling the host was not saved.' }
    $set.Invoke((Open-Preference $marker), @($false))
    if ($enabled.GetValue((Open-Preference $marker))) { throw 'Disabling the host left an automatic start enabled.' }
    'Managed host consent persists across new settings instances.'
}
finally {
    [IO.File]::Delete($marker)
    $directory.Delete()
}
