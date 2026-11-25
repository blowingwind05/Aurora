# Aurora 桌面端

Windows 和 macOS 共用的 Qt Quick 界面和 C++ 桌面应用。发布入口见[原生打包说明](../README.md)。

| 目录 | 内容 |
| --- | --- |
| [src/main.cpp](src/main.cpp) | Qt 进程入口和应用组装 |
| [qml](qml) | `Main.qml`、设备卡、账户、设置与串流控制 |
| [src/ui](src/ui) | QML 状态桥接、串流窗口适配和界面资源 |
| [src/desktop](src/desktop) | 应用状态、设备连接、配对、宿主和壁纸协调 |
| [src/platform](src/platform) | 窗体、单实例、托盘、开机启动和操作系统适配 |
| [src](src) | 业务模块与可独立运行的测试 |

### QML 与 C++ 界面代码的分工

- `qml/` 负责页面布局、样式、动画、键盘和鼠标交互，以及将现有状态显示给用户。
- `src/ui/` 负责 QML 与应用的状态桥接、Qt 窗口生命周期，以及需要 C++ 支持的界面动作。`DesktopBridge` 接收 QML 动作并发布状态，`StreamSessionWindow` 创建和关闭 Qt 串流窗口。
- `src/desktop/` 负责应用决策，例如设备核验、选路、配对和会话控制。操作系统特有的窗口、托盘和进程行为归 `src/platform/`。

点击「连接」时，QML 提交设备 ID 和目录版本，`DesktopBridge` 转交动作，`NativeDesktop` 核验设备并发起串流。需要创建 Qt 串流外窗时，由 `StreamSessionWindow` 加载相应 QML。Sunshine 设置按管理会话和请求 ID 确认写入。

在配置 Qt 的开发环境中构建。Windows 使用 Visual Studio x64 开发者 PowerShell。

```powershell
cmake -S desktop -B desktop/obj/aurora-dev -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/obj/aurora-dev --config Release
```

运行 `Aurora --check-native` 时传入 `desktop/app/qml/Main.qml` 的绝对路径。它加载窗口并验证设置请求和退出。`Aurora --native` 可用相同路径调试界面。登录、发现和串流需要随包部署的 Moonlight、Sunshine 和 Tailscale 组件。

从根目录运行 `pwsh -NoProfile -File tests/qt-ui/Replay.ps1` 执行界面回归。模拟界面预览位于 [tests/qt-ui/previews](../../tests/qt-ui/previews)。会话、配对、设置和平台适配的离线测试入口见 `src/` 下的模块说明。[发布包扫描](../../tests/Check-NativeDesktopPackage.py)
