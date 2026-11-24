param(
    [Parameter(Mandatory)][string] $Assembly,
    [Parameter(Mandatory)][string] $Log,
    [Parameter(ValueFromRemainingArguments)][string[]] $ReplayArguments
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class AuroraReplayErrorMode
{
    [DllImport("kernel32.dll")]
    public static extern uint GetErrorMode();
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
"@
$prior = [AuroraReplayErrorMode]::GetErrorMode()
# Process-local only. The dotnet host and Qt child inherit the no-dialog error mode.
[AuroraReplayErrorMode]::SetErrorMode($prior -bor 0x8003) | Out-Null
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QML_DISABLE_DISK_CACHE = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
try {
    $PSNativeCommandUseErrorActionPreference = $false
    $ErrorActionPreference = 'Continue'
    & dotnet $Assembly @ReplayArguments 2>&1 | Tee-Object -FilePath $Log
    $result = $LASTEXITCODE
}
finally {
    [AuroraReplayErrorMode]::SetErrorMode($prior) | Out-Null
}
exit $result
