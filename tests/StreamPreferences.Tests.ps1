#Requires -Version 7.3

param([Parameter(Mandatory)][string] $AssemblyPath)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$type = $assembly.GetType('Aurora.StreamPreferenceStore', $true)
$instance = [Reflection.BindingFlags]'Instance,Public,NonPublic'
$methods = @{}
foreach ($name in 'Set', 'Get', 'SetBitrate', 'GetBitrate', 'SetToggle', 'Arguments', 'Reset') {
    $methods[$name] = $type.GetMethod($name, $instance)
}

function New-Store([string] $Path) {
    [Activator]::CreateInstance($type, $instance, $null, @($Path), $null)
}

function Assert-True([bool] $Condition, [string] $Reason) {
    if (!$Condition) { throw $Reason }
}

$fixture = [IO.Directory]::CreateTempSubdirectory('aurora-stream-preferences-')
$path = Join-Path $fixture.FullName 'stream-preferences.json'
$blocked = Join-Path $fixture.FullName 'blocked.json'
try {
    $store = New-Store $path
    Assert-True ($methods.GetBitrate.Invoke($store, $null) -eq 10000) 'A missing file must use the built-in default.'
    Assert-True (![IO.File]::Exists($path)) 'Reading defaults must not write the fixture.'
    Assert-True ($methods.Set.Invoke($store, @('resolution', '2560x1440'))) 'Resolution must save.'
    Assert-True ($methods.Set.Invoke($store, @('fps', '30'))) 'FPS must save.'
    Assert-True ($methods.GetBitrate.Invoke($store, $null) -eq 20000) 'Automatic bitrate must follow the selected resolution and FPS.'
    Assert-True ($methods.SetBitrate.Invoke($store, @(42500))) 'An explicit bitrate must save.'
    Assert-True ($methods.Set.Invoke($store, @('display-mode', 'borderless'))) 'The window mode must save.'
    Assert-True ($methods.Set.Invoke($store, @('video-decoder', 'software'))) 'The forced decoder must save.'
    Assert-True ($methods.SetToggle.Invoke($store, @('hdr', $true))) 'HDR must save.'
    Assert-True ($methods.SetToggle.Invoke($store, @('vsync', $true))) 'An explicit value equal to the default must save.'

    $reopened = New-Store $path
    Assert-True ($methods.GetBitrate.Invoke($reopened, $null) -eq 42500) 'A fresh store must read the saved bitrate.'
    Assert-True ($methods.Get.Invoke($reopened, @('resolution')) -ceq '2560x1440') 'A fresh store must read the saved resolution.'
    Assert-True ($methods.Get.Invoke($reopened, @('video-decoder')) -ceq 'software') 'HDR must not overwrite the saved decoder.'
    $arguments = $methods.Arguments.Invoke($reopened, @($true, $false)) -join '|'
    Assert-True ($arguments.Contains('--display-mode|borderless')) 'An explicit window mode must win over the viewer default.'
    Assert-True ($arguments.Contains('--video-decoder|auto') -and $arguments.Contains('--hdr')) 'HDR must force auto decoding only in CLI arguments.'
    Assert-True ($arguments.Contains('--vsync')) 'An explicit default-valued override must survive restart and reach standalone CLI arguments.'
    Assert-True ($methods.Reset.Invoke($reopened, $null)) 'Reset must save.'
    $reset = New-Store $path
    Assert-True ($methods.GetBitrate.Invoke($reset, $null) -eq 10000) 'Reset must survive reopening the store.'
    Assert-True (($methods.Arguments.Invoke($reset, @($false, $false)) -join '|') -ceq '--absolute-mouse') 'Reset must restore the original CLI defaults.'

    [IO.File]::WriteAllText($path, '{"resolution":"1920x1080","fps":60,"bitrate":"-1","keep-awake":"off","unknown":"on","hdr":"--untrusted","audio-config":["stereo"],"video-codec":"AV1"}')
    $partial = New-Store $path
    $arguments = $methods.Arguments.Invoke($partial, @($false, $false)) -join '|'
    Assert-True ($methods.GetBitrate.Invoke($partial, $null) -eq 20000) 'Invalid settings must fall back independently.'
    Assert-True ($arguments.Contains('--no-keep-awake') -and $arguments.Contains('--video-codec|AV1')) 'Valid settings must survive invalid neighbors.'
    Assert-True (!$arguments.Contains('--unknown') -and !$arguments.Contains('--hdr') -and !$arguments.Contains('--audio-config') -and !$arguments.Contains('--fps')) 'Unknown or invalid saved arguments must not reach the CLI.'

    foreach ($invalid in @('{broken', '[]', '{"fps":"\uD800"}', ('a' * 16385))) {
        [IO.File]::WriteAllText($path, $invalid)
        $fallback = New-Store $path
        Assert-True ($methods.GetBitrate.Invoke($fallback, $null) -eq 10000) 'Malformed, non-object, and oversized files must fall back.'
    }

    $null = [IO.Directory]::CreateDirectory($blocked)
    $failing = New-Store $blocked
    Assert-True (!$methods.Set.Invoke($failing, @('fps', '30'))) 'A write failure must return false.'
    Assert-True ($methods.Get.Invoke($failing, @('fps')) -ceq '30') 'A failed write must leave the selection usable in this instance.'
    Assert-True (!$methods.Reset.Invoke($failing, $null)) 'A reset write failure must return false.'
    Assert-True ($methods.Get.Invoke($failing, @('fps')) -ceq '60') 'A failed reset must restore in-memory defaults.'
    Assert-True (@($fixture.GetFiles('*.tmp')).Count -eq 0) 'Atomic saves must clean up temporary files on failure.'
}
finally {
    [IO.File]::Delete($path)
    if ([IO.Directory]::Exists($blocked)) { [IO.Directory]::Delete($blocked) }
    $fixture.Delete()
}

'Stream preferences persistence, defaults, validation, and failed writes passed with isolated settings.'
