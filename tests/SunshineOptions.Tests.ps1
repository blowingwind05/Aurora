#Requires -Version 7.3

param([string] $AssemblyPath = (Join-Path $PSScriptRoot '../dist/Avalonia-0.7/Aurora.dll'))

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$flags = [Reflection.BindingFlags]'Static,NonPublic'
$window = $assembly.GetType('Aurora.SunshineOptionsPage', $true)
$options = @($window.GetMethod('ReadCatalog', $flags).Invoke($null, $null))
if ($options.Count -ne 96 -or @($options.Key | Sort-Object -Unique).Count -ne 96) {
    throw 'Expected 96 distinct upstream configuration keys.'
}
$port = ($options | Where-Object Key -eq 'port').PSObject.BaseObject
$validate = $window.GetMethod('Validate', $flags)
$validate.Invoke($null, @($port, '47989'))
try {
    $validate.Invoke($null, @($port, '65535'))
    throw 'Invalid port was accepted.'
}
catch {
    if ($_.Exception.InnerException -isnot [InvalidOperationException]) { throw }
}

$admin = $assembly.GetType('Aurora.SunshineAdmin', $true)
$merge = $admin.GetMethod('MergeOptions', $flags)
$config = [Text.Json.JsonDocument]::Parse('{"status":true,"version":"test","port":"47989","sunshine_name":"Old","unknown_option":"keep"}')
try {
    $changes = [Collections.Generic.Dictionary[string,string]]::new()
    $changes.Add('sunshine_name', '')
    $changes.Add('stream_audio', 'disabled')
    $payload = $merge.Invoke($null, @($config.RootElement, $changes))
    if ($payload.ContainsKey('status') -or $payload.ContainsKey('version') -or $payload.ContainsKey('sunshine_name') -or
        $payload['stream_audio'] -cne 'disabled' -or $payload['unknown_option'].GetString() -cne 'keep') {
        throw 'Configuration merge discarded an unrelated option or retained output metadata.'
    }
}
finally { $config.Dispose() }

'Sunshine native catalog, validation, and configuration merge tests passed.'
