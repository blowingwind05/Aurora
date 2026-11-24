#Requires -Version 7.3

param(
    [string] $QtExecutable,
    [ValidateSet('1','1.25','1.5')][string] $Scale = '1',
    [switch] $FailureProbe
)

$ErrorActionPreference = 'Stop'
$assembly = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'bin/Debug/net9.0/Replay.dll')).Path
$start = [Diagnostics.ProcessStartInfo]::new((Get-Command dotnet -CommandType Application -TotalCount 1).Source)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.WorkingDirectory = $PSScriptRoot
$start.ArgumentList.Add($assembly)
if (!$FailureProbe) {
    $start.ArgumentList.Add((Resolve-Path -LiteralPath $QtExecutable).Path)
    $start.ArgumentList.Add((Join-Path $PSScriptRoot 'Replay.qml'))
}
$start.Environment['QT_QPA_PLATFORM'] = 'offscreen'
$start.Environment['QT_QUICK_BACKEND'] = 'software'
$start.Environment['QT_QPA_FONTDIR'] = "$env:WINDIR/Fonts"
$start.Environment['QT_FORCE_STDERR_LOGGING'] = '1'
$start.Environment['QT_SCALE_FACTOR'] = $Scale

if (!('AuroraReplayProcessMode' -as [type])) {
    Add-Type @'
using System.Runtime.InteropServices;

public static class AuroraReplayProcessMode
{
    [DllImport("kernel32.dll")]
    public static extern uint GetErrorMode();

    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
'@
}

$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
try {
    # A child inherits this mode. Restore the invoking process as soon as the replay starts.
    $priorMode = [AuroraReplayProcessMode]::GetErrorMode()
    $null = [AuroraReplayProcessMode]::SetErrorMode($priorMode -bor 0x8003)
    try { $null = $process.Start() }
    finally { $null = [AuroraReplayProcessMode]::SetErrorMode($priorMode) }

    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(40000)) {
        $process.Kill($true)
        $process.WaitForExit()
        [Console]::Error.WriteLine('The isolated Tailscale replay timed out.')
        exit 124
    }
    [Console]::Out.Write($stdout.GetAwaiter().GetResult())
    [Console]::Error.Write($stderr.GetAwaiter().GetResult())
    exit $process.ExitCode
}
finally { $process.Dispose() }