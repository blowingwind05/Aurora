#Requires -Version 7.3

param(
    [Parameter(Mandatory)] [string] $BuildDirectory,
    [Parameter(Mandatory)] [string] $DeployDirectory,
    [Parameter(Mandatory)] [string] $QtPrefix,
    [Parameter(Mandatory)] [ValidateSet('5', '6')] [string] $QtMajor
)

$ErrorActionPreference = 'Stop'
$desktop = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$repository = Split-Path -Parent $desktop
$shell = Join-Path $desktop 'qt-shell'
$qtPackage = Join-Path $QtPrefix "lib/cmake/Qt$QtMajor"
if (-not (Test-Path -LiteralPath (Join-Path $qtPackage "Qt$($QtMajor)Config.cmake") -PathType Leaf)) {
    throw "Qt $QtMajor development files were not found under the selected qmake prefix: $QtPrefix"
}

# Pin both package lookups in CMakeLists.txt to the qmake installation.
$bin = Join-Path $BuildDirectory 'bin'
cmake -S $shell -B $BuildDirectory -G 'NMake Makefiles' '-DCMAKE_BUILD_TYPE=Release' `
    "-DCMAKE_PREFIX_PATH=$QtPrefix" "-DQT_DIR=$qtPackage" "-DQt$($QtMajor)_DIR=$qtPackage" `
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$bin"
if ($LASTEXITCODE -ne 0) { throw 'Configuring AuroraQtShell failed.' }
cmake --build $BuildDirectory
if ($LASTEXITCODE -ne 0) { throw 'Compiling AuroraQtShell failed.' }
Copy-Item -LiteralPath (Join-Path $bin 'AuroraQtShell.exe') -Destination $DeployDirectory

# Keep the source layout so QML's relative imports and asset URLs survive packaging.
$preview = Join-Path $DeployDirectory 'QtPreview'
$previewShell = Join-Path $preview 'desktop/qt-shell'
$bridge = Join-Path $previewShell 'bridge'
$assets = Join-Path $preview 'desktop/Assets'
$design = Join-Path $preview 'docs/visual-design/assets'
$brand = Join-Path $design 'brand'
$null = New-Item -ItemType Directory -Force -Path $bridge, $assets, $brand
Copy-Item -LiteralPath (Join-Path $shell 'qml') -Destination $previewShell -Recurse
Copy-Item -LiteralPath (Join-Path $shell 'bridge/Live.qml') -Destination $bridge
foreach ($asset in @('Aurora.png', 'Aurora-Windows-32.png', 'tailscale-dot.svg', 'panel-top-tight.svg', 'Wallpapers')) {
    Copy-Item -LiteralPath (Join-Path $desktop "Assets/$asset") -Destination $assets -Recurse
}
$designSource = Join-Path $repository 'docs/visual-design/assets'
Copy-Item -LiteralPath (Join-Path $designSource 'icons') -Destination $design -Recurse
Copy-Item -LiteralPath (Join-Path $designSource 'brand/Aurora-transparent-512.png') -Destination $brand
