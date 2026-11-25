# SPDX-License-Identifier: GPL-3.0-only
param(
    [string] $QmlTestRunner = 'qmltestrunner'
)

$ErrorActionPreference = 'Stop'
$runner = (Get-Command $QmlTestRunner -CommandType Application).Source
$settings = @{
    QT_QPA_PLATFORM = 'offscreen'
    QT_QUICK_BACKEND = 'software'
    QT_QUICK_CONTROLS_STYLE = 'Fusion'
    QML_DISABLE_DISK_CACHE = '1'
    QT_FORCE_STDERR_LOGGING = '1'
}
if ($env:OS -eq 'Windows_NT' -and !$env:QT_QPA_FONTDIR) {
    $settings.QT_QPA_FONTDIR = Join-Path $env:SystemRoot 'Fonts'
}
$original = @{}
try {
    foreach ($name in $settings.Keys) {
        $original[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process')
    }
    & $runner -input $PSScriptRoot -o '-,txt'
    $testExitCode = $LASTEXITCODE
}
finally {
    foreach ($name in $original.Keys) {
        [Environment]::SetEnvironmentVariable($name, $original[$name], 'Process')
    }
}
exit $testExitCode
