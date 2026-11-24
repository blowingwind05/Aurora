#Requires -Version 7.3

param([Parameter(Mandatory)][string] $AssemblyPath)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$flags = [Reflection.BindingFlags]'Static,NonPublic'
$catalog = $assembly.GetType('Aurora.SunshineOptionCatalog', $true)
$options = @($catalog.GetMethod('ReadCatalog', $flags).Invoke($null, $null))
if ($options.Count -ne 96 -or @($options.Key | Sort-Object -Unique).Count -ne 96) {
    throw 'Expected 96 distinct upstream configuration keys.'
}
$port = ($options | Where-Object Key -eq 'port').PSObject.BaseObject
$validate = $catalog.GetMethod('Validate', $flags)
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

$parsePrivacy = $admin.GetMethod('ParsePrivacySettings', $flags)
$virtualId = '{82ae1b01-31af-4430-88cd-09c56dbf7af2}'
$json = [Text.Json.JsonDocument]::Parse("{`"saved`":`"$virtualId`",`"active`":`"$virtualId`",`"displays`":[{`"id`":`"$virtualId`",`"name`":`"Test display`"}]}")
try {
    $legacy = $parsePrivacy.Invoke($null, @($json.RootElement))
    if (!$legacy.SupportsVirtualDisplay -or $legacy.SupportsScreensaver -or !$legacy.Saved.Exclusive -or
        $legacy.Saved.Id -cne $virtualId -or $legacy.Displays.Count -ne 1) {
        throw 'The privacy parser lost a previous virtual-display-only configuration.'
    }
}
finally { $json.Dispose() }

$json = [Text.Json.JsonDocument]::Parse('{"supports_virtual_display":false,"supports_screensaver":true,"saved_screensaver":true,"active_screensaver":false}')
try {
    $curtain = $parsePrivacy.Invoke($null, @($json.RootElement))
    if ($curtain.SupportsVirtualDisplay -or !$curtain.SupportsScreensaver -or !$curtain.Saved.Screensaver -or
        $curtain.Active.Screensaver -or $curtain.Displays.Count -ne 0) {
        throw 'The privacy parser must accept a screensaver-only host and distinguish saved and loaded settings.'
    }
}
finally { $json.Dispose() }

'Sunshine native catalog, validation, configuration merge, and privacy capabilities tests passed.'
