# 开发 Aurora

[项目首页](README.md) · [视觉与交互规范](docs/visual-design/README.md) · [串流运行时](docs/streaming.md)

## 开发环境

- 桌面端使用 .NET 9 SDK、Avalonia 和 PowerShell 7。用 Git 获取仓库，从仓库根目录执行下列命令。
- Windows 原生组件面向 x64，使用 Visual Studio x64 开发环境、CMake 和带 `qmake`、`windeployqt` 的 Qt 工具链。Moonlight 使用源码附带的 `jom`。Sunshine 使用 MSYS2 UCRT64、GCC 14 或更新版本和原生 x64 Node.js。制作安装器时准备 Inno Setup 6。
- macOS 原生构建使用与目标架构相同的 Mac、Xcode 工具、CMake 和带 `qmake`、`macdeployqt` 的 Qt 6。Sunshine 的 Homebrew 和 Python 依赖见[宿主构建说明](desktop/upstream/sunshine/README.md#macos)。
- 内置 Tailscale 节点的构建脚本使用 Go 1.27.4。Windows 目标还需要 GNU `windres` 和配套的预处理器。 [节点构建说明](desktop/upstream/tsnet/README.md)

调试主窗口、卡片或设置页时，可先运行桌面工程。把运行时改为当前机器的 `win-x64`、`osx-arm64` 或 `osx-x64`。

```powershell
dotnet run --project ./desktop/Aurora.csproj --runtime win-x64
```

需要验证登录、发现、配对、串流和被控权限时，请启动下面打包得到的 Aurora。

## 构建发布包

### Windows

在 Visual Studio x64 开发者 PowerShell 运行 Moonlight 构建，按 Sunshine 文档准备 MSYS2 依赖后再构建宿主。

```powershell
pwsh -File ./desktop/upstream/moonlight-qt/Build-Windows-Streamer.ps1
pwsh -File ./desktop/upstream/sunshine/Build-Windows-Sunshine.ps1 -Msys2Root C:\msys64
pwsh -File ./desktop/upstream/tsnet/Build-Tailnet.ps1 -Runtime win-x64 -OutputDirectory ./desktop/obj/tailnet-win -RunTests
```

把前两个脚本生成的 `Streamer` 和 Sunshine 部署目录填入下列变量。`Streamer` 包含 Qt 主窗口和它与串流组件共用的 Qt 运行库。打包脚本打印 `Aurora.exe` 的输出路径。`-Setup` 同时生成 `AuroraSetup.exe`，找不到 Inno Setup 时可传入 `-InnoSetupCompiler`。

```powershell
$streamer = 'C:\build\aurora\Streamer'
$sunshine = 'C:\build\aurora\Sunshine'
pwsh -File ./desktop/Build.ps1 -StreamerDirectory $streamer -SunshineDirectory $sunshine -TailscaleDirectory ./desktop/obj/tailnet-win -Setup
```

### macOS

在 Mac 上构建。Apple 芯片用 `arm64`，Intel 用 `x64`。构建节点和应用时选择相同的架构。

```powershell
pwsh -File ./desktop/upstream/tsnet/Build-Tailnet.ps1 -Runtime osx-arm64 -OutputDirectory ./desktop/obj/tailnet-mac -RunTests
pwsh -File ./desktop/Build-Mac.ps1 -Architecture arm64 -TailscaleDirectory ./desktop/obj/tailnet-mac
```

应用脚本调用 Moonlight 和 Sunshine 的原生构建，生成 `Aurora.app` 和对应 ZIP。需要选择另一套 Qt 时传 `-Qmake`。也可以在 GitHub Actions 的 **Build Aurora for macOS** 手动选择目标架构，从运行详情中的 **Artifacts** 取包。

编译检出、工具缓存和发布输出分别放在 Git 忽略的 `desktop/obj/` 和 `dist/`。构建后保留组件的 `licenses/` 和 `SOURCE.txt`。

## 实现入口

| 领域 | 代码 |
| --- | --- |
| 主窗口、页面切换和设备卡 | [Qt 主窗口](desktop/qt-shell/qml/AuroraMainWindow.qml)、[设备卡](desktop/qt-shell/qml/DeviceCard.qml)、[宿主连接](desktop/AuroraWindow.QtShell.cs) |
| 主题、控件和资产 | [Qt 主题](desktop/qt-shell/qml/AuroraTheme.qml)、[视觉方案](docs/visual-design/README.md)、[应用资产](desktop/Assets) |
| 设备发现与选路 | [Discovery.cs](desktop/Discovery.cs)、[TailscaleRuntime.cs](desktop/TailscaleRuntime.cs)、[EmbeddedTailnet.cs](desktop/EmbeddedTailnet.cs) |
| Tailscale 账户 | [TailscaleAccountSession.cs](desktop/TailscaleAccountSession.cs)、[Qt 账户页](desktop/qt-shell/qml/TailscaleAccountPage.qml) |
| 同账号配对 | [AutomaticPairing.cs](desktop/AutomaticPairing.cs) 与各组件的后台配对补丁 |
| 被控组件启动与设置 | [BundledSunshine.cs](desktop/BundledSunshine.cs)、[SunshineHostSession.cs](desktop/SunshineHostSession.cs)、[SunshineAdmin.cs](desktop/SunshineAdmin.cs)、[Qt 设置页](desktop/qt-shell/qml/SunshineSettings.qml) |
| 串流窗口与主控偏好 | [Windows 串流窗](desktop/Platform/Windows/StreamSessionWindow.cs)、[macOS 原生顶栏补丁](desktop/upstream/moonlight-qt/0013-aurora-macos-unified-session-toolbar.patch)、[StreamPreferences.cs](desktop/StreamPreferences.cs) |
| 桌面壁纸预览 | [DesktopWallpaper.cs](desktop/DesktopWallpaper.cs) 与组件的壁纸读取补丁 |

Windows 与 macOS 的正式包共用 Qt Quick 主窗口和 C# 宿主。Windows 将串流渲染挂进 Aurora 的会话窗，macOS 的窗口化会话使用串流组件的 AppKit 顶栏。调整串流界面时，核对目标平台的实现入口。

发现阶段合并同一宿主的局域网和 Tailscale 路径。首次自动配对通过当前 Tailscale 账号核验对端，串流组件继续验证 Sunshine 的配对证书。内置 Tailscale 通过应用内节点转发 Aurora 所需的连接；Sunshine 的管理 HTTPS 在本机回环地址，自定义管理请求也沿用证书校验。

串流偏好、允许被控的选择、配对凭据和壁纸缓存属于当前用户。测试这些流程时用临时设置或模拟宿主。调试日志和截图发布前检查地址、设备名、账号信息及凭据。

## 更新上游组件

固定版本、补丁顺序和部署脚本位于 [Moonlight](desktop/upstream/moonlight-qt/README.md)、[Sunshine](desktop/upstream/sunshine/README.md) 和 [tsnet](desktop/upstream/tsnet/README.md)。更新组件时，核对提交版本和子模块，验证补丁应用及单组件构建，再从 Aurora 包验证发现、首次配对和串流。移植上游代码时保留对应许可、来源和本地差异。

## 验证改动

Windows 下可先编译一个供离线测试使用的 `Aurora.dll`。

```powershell
dotnet publish ./desktop/Aurora.csproj -c Release -r win-x64 --self-contained false -p:RestoreLockedMode=true -o ./dist/development
$assembly = './dist/development/Aurora.dll'
pwsh -NoProfile -File ./tests/DesktopDiscovery.Tests.ps1 -AssemblyPath $assembly
pwsh -NoProfile -File ./tests/AutomaticPairing.Tests.ps1 -AssemblyPath $assembly -OfflineOnly
pwsh -NoProfile -File ./tests/StreamPreferences.Tests.ps1 -AssemblyPath $assembly
pwsh -NoProfile -File ./tests/ManagedHostPreference.Tests.ps1 -AssemblyPath $assembly
pwsh -NoProfile -File ./tests/SunshineOptions.Tests.ps1 -AssemblyPath $assembly
```

界面改动还可运行 [Windows 界面回归检查](tests/windows-visual/README.md)。原生组件改动用实际发布包验证，并在记录中写明平台、架构、测试方式和结果。

端到端验证覆盖同账号首次配对、已配对重连、局域网与跨网选路、断开恢复，以及退出后重开应用时的被控设置。macOS 权限相关改动需实测首次授权、授权后重启宿主和首次串流。界面验收覆盖宽窗、窄窗及键盘操作，见[视觉规范](docs/visual-design/README.md)。

### 旧命令行诊断

根目录的 `Aurora.ps1` 和 `cli/` 提供早期 Windows PowerShell 诊断工具。工具使用系统 Tailscale 和外部 Moonlight，可运行 `Doctor`、`Devices`、`Pair`、`Apps`、`Stream`。对应发现测试可用 `pwsh -NoProfile -File ./tests/Discovery.Tests.ps1` 执行。
