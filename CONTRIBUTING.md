# 开发 Aurora

[项目首页](README.md) · [开发交接](docs/handoff.md) · [视觉与交互规范](docs/visual-design/README.md) · [串流运行时](docs/streaming.md)

## 开发环境

- 桌面应用使用 C++17、Qt Quick 和 CMake。构建脚本需要 PowerShell 7.3 与 Python 3.10 或更新版本。
- Windows x64 的 Aurora 和 Moonlight 使用 Visual Studio x64 开发者环境及 Qt 6.6.3 MSVC x64 SDK，需包含 `qmake` 和 `windeployqt`。Sunshine 单独使用 MSYS2 UCRT64 的 GCC 14 或更新版本、CMake 和 Ninja。安装器使用 Inno Setup 6。
- macOS 使用 Xcode 工具和 Qt 6.6.3，安装 `qtimageformats` 以部署 WebP 插件。两平台的 Sunshine 依赖见[宿主构建说明](desktop/packaging/Sunshine.md)。
- 内置 Tailscale 使用 Go 1.27.4。Windows 还需要 GNU `windres` 和预处理器。[节点构建说明](desktop/tailnet/README.md)

## 检出源码

默认从[交接基线](docs/handoff.md#默认基线)开工。在 Aurora 主仓根目录按 `.gitmodules` 和该提交的 gitlink 初始化 Moonlight、Sunshine。发布构建从主仓索引读取锁定提交，再按平台准备嵌套依赖。[子模块与构建副本](desktop/upstream/README.md)

## 构建发布包

### Windows

在 Visual Studio x64 开发者 PowerShell 中运行。按本机安装位置设置 `$qmake` 和 `$msys2`，并将前两个构建脚本输出的部署目录填入 `$streamer` 和 `$sunshine`。

```powershell
$qmake = 'C:\Qt\6.6.3\msvc2019_64\bin\qmake.exe'
$msys2 = 'C:\msys64'
pwsh -File ./desktop/packaging/Build-Windows-Streamer.ps1 -Qmake $qmake
pwsh -File ./desktop/packaging/Build-Windows-Sunshine.ps1 -Msys2Root $msys2
pwsh -File ./desktop/tailnet/Build-Tailnet.ps1 -Runtime win-x64 -OutputDirectory ./desktop/obj/tailnet-win -RunTests -Windres (Join-Path $msys2 'ucrt64/bin/windres.exe')
$streamer = 'C:\build\aurora\Streamer'
$sunshine = 'C:\build\aurora\Sunshine'
pwsh -File ./desktop/packaging/Build-Windows.ps1 -StreamerDirectory $streamer -SunshineDirectory $sunshine -TailscaleDirectory ./desktop/obj/tailnet-win -Qmake $qmake -Setup
```

`Build-Windows.ps1` 用与 Moonlight 相同的 Qt 编译主程序并组装便携包及校验文件。`-Setup` 同时生成安装器。

### macOS

以下以 Apple 芯片的原生环境为例。Intel 原生环境将 `arm64` 和 `osx-arm64` 分别换成 `x64` 和 `osx-x64`。让 `qmake` 指向上述 Qt 6 SDK，也可通过 `-Qmake` 指定完整路径。

```powershell
pwsh -File ./desktop/tailnet/Build-Tailnet.ps1 -Runtime osx-arm64 -OutputDirectory ./desktop/obj/tailnet-mac -RunTests
pwsh -File ./desktop/packaging/Build-Mac.ps1 -Architecture arm64 -TailscaleDirectory ./desktop/obj/tailnet-mac
```

应用脚本从源码编译 Aurora、Moonlight 和 Sunshine，再将节点、Qt 框架及许可文件装入 `Aurora.app`。GitHub Actions 提供双平台构建。编译缓存和发布包分别放在 `desktop/obj/` 和 `dist/`。

## 代码目录

| 目录 | 职责 |
| --- | --- |
| [desktop/app/src/main.cpp](desktop/app/src/main.cpp) | Qt 进程入口和应用组装 |
| [desktop/app/qml](desktop/app/qml) | `Main.qml`、界面、主题和会话控件 |
| [desktop/app/src/ui](desktop/app/src/ui) | QML 状态桥接和串流窗口适配 |
| [desktop/app/src/desktop](desktop/app/src/desktop) | 账号、设备、会话和被控功能的协调 |
| [desktop/app/src/platform](desktop/app/src/platform) | 进程单实例、托盘、开机启动和系统适配 |
| [desktop/app/src](desktop/app/src) | 串流、Sunshine、Tailscale、配对和壁纸模块 |
| [desktop/packaging](desktop/packaging) | Windows 和 macOS 发布包、安装器和校验 |
| [desktop/upstream](desktop/upstream) | Moonlight 和 Sunshine 子模块、版本锁定和构建源码准备 |
| [desktop/CMakeLists.txt](desktop/CMakeLists.txt) | 桌面程序的统一 CMake 构建入口 |
| [tests/qt-ui](tests/qt-ui) | Qt 界面回归和模拟预览 |

## 验证改动

```powershell
pwsh -NoProfile -File ./tests/qt-ui/Replay.ps1
python ./tests/Test-NativeDesktopAssembly.py
python ./tests/Check-NativeDesktopPackage.py ./dist/Aurora-0.8-source-integrated
```

串流、配对、账号、宿主和设备发现模块各自的 `README.md` 记录可独立运行的 CTest。`Aurora --check-native` 验证 QML 加载、进程内设置请求和退出路径。真机验收覆盖首次配对、重连、局域网与跨网选路、被控权限以及重开后的设置。上游更新时一并核对固定版本和随包许可证。
