# Aurora Qt Quick 主界面

主窗口、侧栏和设备页复用[完整视觉方案](../../docs/visual-design/README.md)。界面在 Windows 上可用 Qt 5.15 的 `qmlscene` 打开 `demo/Demo.qml` 查看，数据来自演示模型。应用宿主接入时加载 `qml/AuroraMainWindow.qml`，绑定设备与账户状态，随后显示窗口。

| QML 接口 | 宿主 |
| --- | --- |
| `deviceModel` | 传入 ListModel 或 QAbstractListModel。角色为 `deviceId`、`displayName`、`online`、`canConnect`、`lanAddress`、`tailscaleAddress`、`wallpaperUrl`。ID 为稳定设备标识。地址与壁纸可为空串。 |
| `deviceAccess` | 使用 `loading`、`signedOut` 或 `ready`。账户显示文字由 `accountName`、`accountSubtitle` 提供。 |
| `refreshRequested()` | 调用设备发现，回传 `refreshing`、`refreshFeedback` 和 `refreshFailed`。 |
| `connectRequested(deviceId)` | 用设备 ID 查询当前可用路由并发起串流。连接处理中绑定 `connectionPending`。 |
| `navigationRequested(destination)` | 同步 `currentPage` 并提供其他页面的 `destinationPage`。 |
| `deviceToolRequested(deviceId, action)` | 通过 `enabledDeviceTools` 按设备开放文件、观看、终端和端口映射。重启和关机需接现有宿主确认流程。 |

资源复用 `desktop/Assets` 的品牌与壁纸，以及设计目录中的图标。当前预览使用本地文件路径，正式打包时可在 `AuroraTheme.qml` 中映射到资源路径。演示入口和真实联调入口使用同一个主窗口。

## 启动真实主窗口联调

需要 .NET 9、CMake、C++17 工具链和 Qt Quick、Quick Controls 2 的开发与运行组件。当前验收环境为 Windows x64、MSVC 和 Qt 5.15.2。准备 Qt 运行时后，从已应用补丁的副本根目录运行。Windows 使用 x64 开发者 PowerShell。给 `CMAKE_PREFIX_PATH` 填写 Qt 安装前缀，Windows 的 `PATH` 中加入该 Qt 的 `bin`。下面的构建路径应位于副本或独立临时目录。

```powershell
cmake -S desktop/qt-shell -B build/qt-shell -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qtPrefix"
cmake --build build/qt-shell
$env:AVALONIA_TELEMETRY_OPTOUT = '1'
dotnet build desktop/Aurora.csproj -c Release -r win-x64 --self-contained false --disable-build-servers
$qtShell = (Resolve-Path build/qt-shell/AuroraQtShell.exe).Path
$live = (Resolve-Path desktop/qt-shell/bridge/Live.qml).Path
dotnet desktop/bin/Release/net9.0/win-x64/Aurora.dll --qt-shell $qtShell $live
```

在 macOS 上用匹配 .NET 目标架构的 Qt 和 CMake 默认生成器构建，将 RID 换成 `osx-arm64` 或 `osx-x64`，将子程序路径换成无 `.exe` 后缀的路径。QML 仍从源码目录加载，以复用现有资源路径。

`--qt-shell` 在 Aurora 首次启动时额外打开 Qt 窗口，同时保留 Avalonia 主窗。当前若已有 Aurora 实例，请从原窗口退出后再启动带参数的副本。Qt 中的账户入口打开既有的 Avalonia 账户页。关闭 Qt 窗口会结束联调通道，退出 Aurora 会结束 Qt 子进程。设备筛选、刷新文案、身份验证、选路及串流由现有 C# 流程负责。

子进程通过重定向的 stdin/stdout 交换 UTF-8 JSON 行。C# 写入完整状态快照，Qt 回写 `refresh`、`connect` 和 `account` 操作。连接消息带当前快照的 `deviceEpoch` 和 `deviceId`，以便 C# 拒绝账户切换前的设备操作。标准错误用于 Qt 诊断。

## 离线回放

回放使用实际 C# 管道类和实际 Qt 主窗口。它用固定账户和设备数据检查宽窗及窄窗、刷新等待与结果、带 ID 的连接信号、切换账户状态后清空设备、以及两端关闭时回收进程。在同一 PowerShell 会话中可于启动真实 Aurora 前执行。

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:QT_FORCE_STDERR_LOGGING = '1'
dotnet build tests/qt-shell/Replay.csproj --disable-build-servers
dotnet tests/qt-shell/bin/Debug/net9.0/Replay.dll $qtShell tests/qt-shell/Replay.qml
Remove-Item Env:QT_QPA_PLATFORM, Env:QT_QUICK_BACKEND, Env:QT_FORCE_STDERR_LOGGING
```

Windows 的 Qt 若提示缺少离屏字体目录，可在回放前给 `QT_QPA_FONTDIR` 指定本机字体目录。做真实账户验收时使用测试账号和可连接的测试设备，检查 Qt 与 Avalonia 的设备及账户状态一致，Qt 刷新走完既有发现流程，点击可连接卡片后打开目标设备的现有串流窗口。退出测试账户，确认 Qt 中的设备随即消失。
