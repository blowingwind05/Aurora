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

资源复用 `desktop/Assets` 的品牌与壁纸，以及设计目录中的图标。当前预览使用本地文件路径，正式打包时可在 `AuroraTheme.qml` 中映射到资源路径。主窗口暂由演示入口加载，真实设备和其他页面仍由现有桌面宿主提供。
