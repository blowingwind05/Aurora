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

Windows Qt 便携包带有 Qt 运行时和 QML 入口，可直接双击 `Aurora.exe` 启动。Qt 子程序的窗口图标和可执行文件图标均使用 `desktop/Assets/Aurora.ico`。打包 QML 时复制 `docs/visual-design/assets/icons/` 及其许可文件，并带上 `desktop/Assets/panel-top-tight.svg`。以下命令用于从源码独立构建并试用 Qt 界面。

需要 .NET 9、CMake、C++17 工具链和 Qt Quick、Quick Controls 2、Network 的开发与运行组件。当前验收环境为 Windows x64、MSVC 和 Qt 5.15.2。准备 Qt 运行时后，从已应用补丁的副本根目录运行。Windows 使用 x64 开发者 PowerShell。给 `CMAKE_PREFIX_PATH` 填写 Qt 安装前缀，Windows 的 `PATH` 中加入该 Qt 的 `bin`。下面的构建路径应位于副本或独立临时目录。

```powershell
cmake -S desktop/qt-shell -B build/qt-shell -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qtPrefix"
cmake --build build/qt-shell
$env:AVALONIA_TELEMETRY_OPTOUT = '1'
dotnet build desktop/Aurora.csproj -c Release -r win-x64 --self-contained false --disable-build-servers
$qtShell = (Resolve-Path build/qt-shell/AuroraQtShell.exe).Path
$live = (Resolve-Path desktop/qt-shell/bridge/Live.qml).Path
dotnet desktop/bin/Release/net9.0/win-x64/Aurora.dll --qt-shell $qtShell $live
```

macOS 的完整构建由 `Build-Mac.ps1` 调用串流构建脚本。Qt 6 的 `qmake` 与 CMake 用于构建匹配目标架构的 `AuroraQtShell`，同一套 QML 和视觉素材放入 `Aurora.app/Contents/Resources/QtPreview`。应用启动时会找到包内的 Qt 界面。从源码联调时，RID 使用 `osx-arm64` 或 `osx-x64`，Qt 子程序路径使用无 `.exe` 后缀的版本。

`--qt-shell` 在 Aurora 首次启动时以 Qt 窗口作为主窗口，Avalonia 宿主在后台运行。当前若已有 Aurora 实例，请从原窗口退出后再启动带参数的副本。「账户」「设置」和「本机被控」进入 Qt 页。Sunshine 常用设置及全部配置项在 Qt 内显示。账号授权和系统权限按需打开对应的系统界面。Qt 窗口沿用主程序的关闭偏好，退出 Aurora 时结束 Qt 子进程。设备筛选、刷新文案、身份验证、选路及串流由现有 C# 流程负责。

子进程通过重定向的 stdin/stdout 交换 UTF-8 JSON 行。C# 写入完整状态快照，Qt 回写 设备操作与导航请求。连接消息带当前快照的 `deviceEpoch` 和 `deviceId`，以便 C# 拒绝账户切换前的设备操作。标准错误用于 Qt 诊断。

「更多」的项目链接和更新检查由 Qt 子进程处理。宿主快照中的 `applicationVersion` 提供当前程序集版本，Qt 负责版本展示和 GitHub 正式版本比较。离线测试位于 [tests/qt-shell-more](../../tests/qt-shell-more)。

## 离线回放

回放使用实际 C# 管道类和实际 Qt 主窗口。它用固定账户和设备数据检查宽窗及窄窗、刷新等待与结果、带 ID 的连接信号、切换账户状态后清空设备、以及两端关闭时回收进程。在同一 PowerShell 会话中可于启动真实 Aurora 前执行。

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:QT_FORCE_STDERR_LOGGING = '1'
dotnet build tests/qt-shell/Replay.csproj --disable-build-servers
pwsh -NoProfile -NonInteractive -File tests/qt-sunshine-shell/Run-IsolatedReplay.ps1 -Assembly tests/qt-shell/bin/Debug/net9.0/Replay.dll -Log devices-replay.txt $qtShell tests/qt-shell/Replay.qml
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
| `sunshineLoad` / `sunshineSave` | 读取 Sunshine 配置并在 Qt 内编辑、自动保存及回读。 |

使用同一 Qt 子程序运行模拟宿主。回放需要 QtTest 的 QML 模块。它在 Windows Qt 5.15.2 上使用 Qt 鼠标和键盘事件，并检查通过标准输入输出传递的快照和请求。

```powershell
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:QML_DISABLE_DISK_CACHE = '1'
dotnet build tests/qt-host-shell/HostReplay.csproj --disable-build-servers
pwsh -NoProfile -NonInteractive -File tests/qt-sunshine-shell/Run-IsolatedReplay.ps1 -Assembly tests/qt-host-shell/bin/Debug/net9.0/HostReplay.dll -Log host-replay.txt $qtShell tests/qt-host-shell/HostReplay.qml
```

回放检查宽窗和 320 px 布局、未登录时的本机地址、长 IPv6 地址换行、复制请求、重新检测、权限入口、默认聚焦「取消」、Esc、显式取消、确认停止后的状态和启动失败后的重试。地址和服务反馈由回放进程提供，权限操作以收到预期请求为验收条件。


## Sunshine 常用设置和全部配置项

Qt 在同一个「本机被控」工作区显示常用设置、远控显示和全部配置项。高级页使用现有 Sunshine 配置目录的 96 个字段，并标记由 Aurora 管理的只读项。文本停止输入约 650 ms 后自动提交，开关和下拉立即提交。C# 使用 Sunshine 的原保存和回读流程，失败的草稿保留在对应字段。离开、关闭窗口或关闭被控前先尝试保存，有失败项时在 Qt 确认是否放弃。重启 Sunshine 前会再确认。

快照通过 `host.sunshine` 发布设置、会话标识和保存回执。`sunshineLoad` 读取配置和诊断，`sunshineSave` 按会话标识和请求 ID 返回保存结果。`sunshineRestart`、`sunshineCertificate`、`sunshineLogin`、`sunshineForget`、`sunshineOriginal`、`sunshineReauthorize` 和 `sunshinePair` 调用已有 C# 服务链。密码和 PIN 只在请求中出现。Windows 管理员授权、macOS 隐私设置和用户主动打开的 Sunshine 原版设置仍交给系统。

### Windows 隔离回放

在临时副本中构建 Qt 子程序，设置 `$qtShell` 为该可执行文件的绝对路径，并将对应 Qt 的 `bin` 加入 `PATH`。回放脚本强制 Qt 离屏软件渲染，并在当前测试进程树禁用 Windows 错误弹窗。托管入口捕获异常、写 stderr 并返回非零。日志路径应指向本次验收的临时目录。

```powershell
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$env:AVALONIA_TELEMETRY_OPTOUT = '1'
dotnet build tests/qt-sunshine-shell/Replay.csproj -c Release -p:RuntimeIdentifiers= --disable-build-servers
$replay = 'tests/qt-sunshine-shell/bin/Release/net9.0/Aurora.QtSunshine.Replay.dll'
$runner = 'tests/qt-sunshine-shell/Run-IsolatedReplay.ps1'
pwsh -NoProfile -NonInteractive -File $runner -Assembly $replay -Log sunshine-replay.txt $qtShell tests/qt-sunshine-shell/SunshineReplay.qml
```

省略 `$qtShell` 和 QML 路径可单独检查 C# 的读取、写入、回读及显示状态。将参数换成 `--verify-failure` 可验证捕获异常和退出码 1。回放中的 HTTP 响应由内存处理器提供，Qt 与 C# 使用真实管道。证书授权、系统隐私权限和实际 Sunshine 服务启停属于实机验收范围。
