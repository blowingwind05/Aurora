# 开发 Aurora

[项目首页](README.md) · [视觉与交互规范](docs/visual-design/README.md) · [串流运行时](docs/streaming.md)

## 开发环境

- 桌面应用使用 C++17、Qt Quick 和 CMake。构建脚本需要 PowerShell 7.3 与 Python 3.10 或更新版本。
- Windows x64 使用 Visual Studio x64 开发者环境及带 `qmake` 和 `windeployqt` 的 Qt。安装器使用 Inno Setup 6。
- macOS 使用目标架构对应的 Xcode 工具和 Qt 6。Windows 与 macOS 的 Sunshine 依赖见[宿主构建说明](desktop/upstream/sunshine/README.md)。
- 内置 Tailscale 使用 Go 1.27.4。Windows 还需要 GNU `windres` 和预处理器。[节点构建说明](desktop/upstream/tsnet/README.md)

## 构建发布包

### Windows

在 Visual Studio x64 开发者 PowerShell 中运行，再将前两个脚本输出的部署目录填入变量。

```powershell
pwsh -File ./desktop/upstream/moonlight-qt/Build-Windows-Streamer.ps1
pwsh -File ./desktop/upstream/sunshine/Build-Windows-Sunshine.ps1 -Msys2Root C:\msys64
pwsh -File ./desktop/upstream/tsnet/Build-Tailnet.ps1 -Runtime win-x64 -OutputDirectory ./desktop/obj/tailnet-win -RunTests
$streamer = 'C:\build\aurora\Streamer'
$sunshine = 'C:\build\aurora\Sunshine'
pwsh -File ./desktop/Build.ps1 -StreamerDirectory $streamer -SunshineDirectory $sunshine -TailscaleDirectory ./desktop/obj/tailnet-win -Setup
```

`Build.ps1` 编译 Qt 主程序并组装便携包及校验文件。`-Setup` 同时生成安装器。传入 `-Qmake` 时选与 Moonlight 相同的 Qt 工具链。

### macOS

Apple 芯片用 `arm64`，Intel 用 `x64`，两个脚本的运行时架构须一致。

```powershell
pwsh -File ./desktop/upstream/tsnet/Build-Tailnet.ps1 -Runtime osx-arm64 -OutputDirectory ./desktop/obj/tailnet-mac -RunTests
pwsh -File ./desktop/Build-Mac.ps1 -Architecture arm64 -TailscaleDirectory ./desktop/obj/tailnet-mac
```

应用脚本从源码编译 Aurora、Moonlight 和 Sunshine，再将节点、Qt 框架及许可文件装入 `Aurora.app`。GitHub Actions 提供双平台构建。编译缓存和发布包分别放在 `desktop/obj/` 和 `dist/`。

## 代码目录

| 目录 | 职责 |
| --- | --- |
| [desktop/qt-shell/bridge](desktop/qt-shell/bridge) | Qt 入口和 QML 请求转发 |
| [desktop/qt-shell/qml](desktop/qt-shell/qml) | 界面、主题和会话控件 |
| [desktop/qt-shell/native/app](desktop/qt-shell/native/app) | 账号、设备、会话和被控功能的协调 |
| [desktop/qt-shell/native/platform](desktop/qt-shell/native/platform) | 进程单实例、托盘、开机启动和系统适配 |
| [desktop/qt-shell/native](desktop/qt-shell/native) | 串流、Sunshine、Tailscale、配对和壁纸模块 |
| [desktop/upstream](desktop/upstream) | 上游固定版本、补丁和部署脚本 |
| [desktop/native-package](desktop/native-package) | 原生构建与打包 |
| [tests/qt-ui](tests/qt-ui) | Qt 界面回归用例 |

## 验证改动

```powershell
pwsh -NoProfile -File ./tests/qt-ui/Replay.ps1
python ./tests/Test-NativeDesktopAssembly.py
python ./tests/Check-NativeDesktopPackage.py ./dist/Aurora-0.8-source-integrated
```

串流、配对、账号、宿主和设备发现模块各自的 `README.md` 记录可独立运行的 CTest。`Aurora --check-native` 验证 QML 加载、进程内设置请求和退出路径。真机验收覆盖首次配对、重连、局域网与跨网选路、被控权限以及重开后的设置。上游更新时一并核对固定版本和随包许可证。
