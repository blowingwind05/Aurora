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

`--qt-shell` 在 Aurora 首次启动时以 Qt 窗口作为主窗口，Avalonia 宿主在后台运行。当前若已有 Aurora 实例，请从原窗口退出后再启动带参数的副本。「账户」「设置」和「本机被控」进入 Qt 页。当前账户授权和 Sunshine 高级配置调用既有页面，打开这些页面时先隐藏 Qt 窗口。点击「我的设备」回到 Qt。Qt 窗口沿用主程序的关闭偏好，退出 Aurora 时结束 Qt 子进程。设备筛选、刷新文案、身份验证、选路及串流由现有 C# 流程负责。

子进程通过重定向的 stdin/stdout 交换 UTF-8 JSON 行。C# 写入完整状态快照，Qt 回写 设备操作与导航请求。连接消息带当前快照的 `deviceEpoch` 和 `deviceId`，以便 C# 拒绝账户切换前的设备操作。标准错误用于 Qt 诊断。

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

## 通用设置预览

用 `qmlscene desktop/qt-shell/demo/SettingsDemo.qml` 打开通用设置。关闭到托盘或菜单栏、开机自启动和防止休眠从宿主读取已确认的值，操作通过 QML 信号传给宿主。独立预览使用内存数据，试验主窗口读取和保存这三项真实偏好。

## 本机被控的宿主接口与离线回放

主窗口的 [HostPage.qml](qml/HostPage.qml) 读取 C# 随设备快照发送的可选 `host` 对象。开关与状态来自宿主，托管服务就绪后才会显示连接地址。Tailscale 地址还要求当前运行方式已登录。未登录时仍可打开本机管理页和使用局域网地址。

| Qt 请求 | C# 处理 |
| --- | --- |
| `hostRefresh` | 运行已有检测流程并返回本页反馈。 |
| `hostEnable` / `hostDisable` | 走既有托管组件启停和偏好保存流程。关闭请求在 Qt 窗口确认后发出。 |
| `hostRetry` | 按已保存的被控偏好重试启动。 |
| `hostCopyLan` / `hostCopyTailnet` | 由宿主复制当前有效地址并回传结果。 |
| `hostOpenPermissions` / `hostContinuePermissions` | 打开已有的 macOS 隐私设置入口，继续时重新检测或重试启动。屏幕录制和辅助功能的授权由系统与 Aurora Host 处理。 |
| `hostOpenSettings` | 打开 Avalonia 的「本机被控」，沿用常用设置、重启确认、全部配置项及独立 Sunshine 管理入口。 |

使用同一 Qt 子程序运行模拟宿主。回放需要 QtTest 的 QML 模块。它在 Windows Qt 5.15.2 上使用 Qt 鼠标和键盘事件，并检查通过标准输入输出传递的快照和请求。

```powershell
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:QML_DISABLE_DISK_CACHE = '1'
dotnet build tests/qt-host-shell/HostReplay.csproj --disable-build-servers
dotnet tests/qt-host-shell/bin/Debug/net9.0/HostReplay.dll $qtShell tests/qt-host-shell/HostReplay.qml
```

回放检查 1000 和 320 px 布局、未登录时的本机地址、长 IPv6 地址换行、复制请求、重新检测、权限和设置入口、默认聚焦「取消」、Esc、显式取消、确认停止后的状态和启动失败后的重试。地址和服务反馈由回放进程提供，权限操作以收到预期请求为验收条件。
