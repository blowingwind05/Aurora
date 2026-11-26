# Aurora 的 Sunshine 宿主组件

极光会保存「允许本机被控」的选择，并在下次启动时尝试恢复内置宿主。退出极光时，当前被控进程和远控连接随之结束。

Windows 和 macOS 共用 [AuroraHost 入口](../runtime/README.md)，由它读取启动凭据和监听停止信号，再调用 Sunshine 的托管运行接口。

可从[上游源码阅读入口](../upstream/README.md)查看各平台的完整源码和固定提交。Windows 宿主的[码率状态机](../protocol/bitrate/HostControl.h)在 Aurora 仓库内维护，由构建送入 Sunshine 源码树。

## 版本口径

下文的 Windows 打包命令以主仓 `ede08495` 为可重放基线。它固定 Sunshine `1d1e55d` 和 Moonlight `f89e322`。是否已经发布，以发行渠道的记录、包内 `SOURCE.txt` 和校验值为准。本次可读资料未确认最新已发布 Windows 制品的版本或提交。当前 Windows 本地 VDD 候选仍待完整安装器验收。 [引擎固定提交与源码记录](../upstream/README.md)

## Windows

Windows 宿主从 [Sunshine v2026.914.233613](https://github.com/SunshineStream/Sunshine/tree/v2026.914.233613) 的 Aurora fork 锁定提交编译。极光复用 Sunshine 的采集、编码、输入及配对实现。托管入口由极光指定用户配置目录和退出事件，并将 HTTPS 管理端口限制在本机回环地址。

Windows x64 构建使用 [Sunshine 上游的 MSYS2 UCRT64 依赖](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/.github/workflows/ci-windows.yml)、Git 和 PowerShell 7。宿主编译和打包只包含采集所需的资源与极光调用的本机管理接口；托盘和设置界面由极光提供。

在 Visual Studio x64 开发终端运行。使用同一个 Qt SDK 构建和部署 AuroraStream 与桌面程序。安装器还需要 Inno Setup。以下命令在准备好依赖的临时 checkout 中运行。 [Windows CI](../../.github/workflows/build-windows.yml)

```powershell
$msys = 'C:\msys64'
$qmake = 'C:\Qt\6.6.3\msvc2019_64\bin\qmake.exe' # 改为实际安装路径
$stream = ./desktop/packaging/Build-Windows-Streamer.ps1 -Qmake $qmake |
    Where-Object { $_ -is [IO.DirectoryInfo] } | Select-Object -Last 1
$hostRuntime = ./desktop/packaging/Build-Windows-Sunshine.ps1 -Msys2Root $msys -WorkDirectory C:\aurora-native |
    Where-Object { $_ -is [IO.DirectoryInfo] } | Select-Object -Last 1
$tailnet = Join-Path $env:TEMP ('aurora-tailnet-' + [guid]::NewGuid())
./desktop/tailnet/Build-Tailnet.ps1 -Runtime win-x64 -OutputDirectory $tailnet -RunTests `
    -Windres (Join-Path $msys 'ucrt64/bin/windres.exe')
./desktop/packaging/Build-Windows.ps1 -StreamerDirectory $stream.FullName `
    -SunshineDirectory $hostRuntime.FullName -TailscaleDirectory $tailnet -Qmake $qmake -Setup

$portable = (Resolve-Path 'dist/Aurora-0.8-source-integrated').Path
$setup = 'dist/Aurora-0.8-source-integrated-setup/AuroraSetup.exe'
./desktop/packaging/Artifact-Checksums.ps1 -Path $portable -Verify
./desktop/packaging/Artifact-Checksums.ps1 -Path $setup -Verify
$zip = Join-Path (Resolve-Path 'dist').Path 'Aurora-Windows-x64-portable.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($portable, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
./desktop/packaging/Artifact-Checksums.ps1 -Path $zip
./desktop/packaging/Artifact-Checksums.ps1 -Path $zip -Verify
python tests/Check-NativeDesktopPackage.py $zip
```

Windows CI 另从便携树运行 `Aurora.exe --check-native`，限制 PATH 并清除 Qt 环境变量。包内的 `Components/Sunshine/licenses/SOURCE.txt` 用于核对 Sunshine 版本。校验和、包结构、启动探测和 Inno 编译各自验证制品的一部分。驱动安装、覆盖、显示布局恢复和卸载仍需在测试机上观察。

极光在「本机被控」启动宿主，配置和配对数据随当前 Windows 用户保存。内置版用 `48989` 作为串流基准端口，和独立安装的 Sunshine 并行运行。退出极光会停止内置宿主。极光用本地公开证书指纹校验 HTTPS 管理连接，管理端口及证书路径等参数在应用内显示为由极光管理的值。

每次启动内置宿主时，极光通过父子进程管道传递本次运行所需的随机管理凭据。Sunshine 使用上游逻辑保存凭据校验值，并在本机 HTTPS 接口继续验证凭据。设置通过极光管理。升级会保留原有 Sunshine 配置、配对和证书，只更新内置宿主私有目录中的管理凭据校验值。

远控 Windows 时，在主控端的「控制中心 → 安全 → 隐私保护」选择「关闭」「关闭显示器」或「隐私屏保」。Aurora 会自动选用 Windows 已识别的兼容虚拟输出，切换后验证仅该输出启用。选择「关闭」或结束会话时恢复原显示布局。没有可用虚拟输出时，界面会提示关屏未生效。

隐私屏保使用支持排除遮罩的采集方式，遮罩随会话结束移除。以旧版宿主配置预先启用的隐私设置仍按原配置生效。

常用设置和全部配置项会自动保存。手动重启被控组件后，新的配置才会用于之后的连接。

### Windows VDD 候选

`ede08495` 的关屏实现使用系统已识别的兼容虚拟输出，基线安装和便携打包流程没有自动部署 MttVDD 的步骤。当前并行工作树增加了 MttVDD `25.7.23` 随包支持、安装器准备待命输出及便携包的安装和移除入口。候选脚本接受 `-VirtualDisplayDriverDirectory` 指定同版本离线驱动文件，或用 `-SkipVirtualDisplayDriver` 跳过打包。候选路径校验驱动文件及发行者签名，安装器只为此次安装临时添加所需的发布者信任。计划在安装时保留物理屏布局，关屏时才启用待命输出，并在退出或断开后恢复。既有 SudoVDA 兼容适配器仍由宿主路径处理。该候选尚待完整安装器验证，以上命令以基线脚本参数为准。

Windows UIAccess 测试遮罩由独立开发脚本临时签署安装，普通安装包使用标准隐私屏保。 [UIAccess 设置和签署入口](../runtime/host/PrivacyScreensaver.md)

## macOS

macOS 使用同一个上游提交。构建脚本复用 Sunshine 的 macOS 应用打包和动态库部署，安装到 `Aurora.app/Contents/Helpers/AuroraHost.app`。宿主使用自己的应用标识和极光数据目录，与独立安装的 Sunshine 分开。串流基准端口为 `48989`，管理接口仅绑定 `127.0.0.1:48990`，极光验证本地公开证书的 SHA-256 指纹。

按 [上游 macOS 构建流程](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/.github/workflows/ci-macos.yml) 准备 Xcode、Git 和构建依赖。Homebrew 依赖为 `cmake pkgconf icu4c@78 openssl@3 opus`，构建使用的 Python 需要 Jinja2。由 `desktop/packaging/Build-Mac.ps1` 调用宿主构建，再与 Moonlight 一起封装。macOS 14.2 及以上可启用被控端。

首次使用在 macOS 系统设置中授予屏幕录制和辅助功能权限，音频和局域网权限按所用功能授权。极光管理宿主的启动与退出。使用内置 Tailscale 时，同账号设备可以自动配对。也可以在本机被控页面确认手动配对的 PIN。

标准构建的 `AURORA_MACOS_PRIVACY_PREVIEW` 默认为 `OFF`。测试 ScreenCaptureKit 隐私遮罩要显式启用预览并重建宿主。下面使用原生 arm64 和已配置的 Qt 6 SDK，Intel 测试机用 `x64`。终端应能找到具备 Jinja2 的 Python。 [macOS CI](../../.github/workflows/build-macos.yml) · [macOS 屏保限制](../runtime/host/PrivacyScreensaver.md)

```bash
arch=arm64
: "${QT_ROOT_DIR:?Set the native Qt SDK directory}"
tailnet="$(mktemp -d)"
pwsh -NoProfile -File ./desktop/tailnet/Build-Tailnet.ps1 \
  -Runtime "osx-$arch" -OutputDirectory "$tailnet" -RunTests
AURORA_MACOS_PRIVACY_PREVIEW=ON pwsh -NoProfile -File ./desktop/packaging/Build-Mac.ps1 \
  -Architecture "$arch" -Qmake "$QT_ROOT_DIR/bin/qmake" -TailscaleDirectory "$tailnet"

zip="dist/Aurora-macOS-$arch-integrated.zip"
(cd dist && shasum -a 256 -c "$(basename "$zip").sha256")
check="$(mktemp -d)"
ditto -x -k "$zip" "$check"
app="$check/Aurora.app"
host="$app/Contents/Helpers/AuroraHost.app"
codesign --verify --deep --strict "$app"
codesign --verify --deep --strict "$host"
python3 tests/Check-NativeDesktopPackage.py "$app"
python3 desktop/upstream/Prepare-EngineSource.py verify sunshine \
  "$host/Contents/Resources/licenses/SOURCE.txt"
python3 desktop/upstream/Prepare-EngineSource.py verify moonlight \
  "$app/Contents/Resources/ThirdParty/Moonlight/SOURCE.txt"
clean_env=(/usr/bin/env -i "HOME=$HOME" "TMPDIR=${TMPDIR:-/tmp}" PATH=/usr/bin:/bin QT_QPA_PLATFORM=cocoa)
test "$("${clean_env[@]}" "$host/Contents/MacOS/AuroraHost" --aurora-runtime-info)" = aurora-host/1
test "$("${clean_env[@]}" "$app/Contents/MacOS/AuroraStream" --aurora-runtime-info)" = aurora-stream/1
test "$("${clean_env[@]}" "$app/Contents/MacOS/AuroraStream" --aurora-gamestream-relay-info)" = aurora-gamestream-relay/1
"${clean_env[@]}" "$app/Contents/MacOS/Aurora" --check-native
```

构建脚本执行的本地 ad-hoc 签名用于包结构和嵌套程序签名检查。宿主遮罩、指针排除和切屏需另行做两机验收。
