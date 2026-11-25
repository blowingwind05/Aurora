# 串流运行时开发

[贡献指南](../CONTRIBUTING.md) · [串流窗口视觉规范](visual-design/stream-session.md)

## 当前实现

[建连入口](../desktop/app/src/desktop/NativeDesktopSessions.cpp) 核验设备身份、选择局域网或 Tailscale 路径、确认配对，再启动 [Moonlight 串流组件](../desktop/upstream/moonlight-qt)。macOS 的 Tailscale 路径由内置节点提供。Windows 可以选用内置节点或已安装的系统客户端。局域网连接沿用已建立的设备信任和 Sunshine 证书校验。

确认配对时，Aurora 调用 `Moonlight list`。启动串流时，Moonlight 再解析主机和目标应用。内置 Tailscale 的配对检查和串流共用所选路由的[转发租约](../desktop/app/src/session/SessionSupervisor.cpp)。[启动警告补丁](../desktop/upstream/moonlight-qt/0014-aurora-skip-cli-warning-delay.patch) 将警告写入日志并让 CLI 继续执行。

[串流偏好](../desktop/app/src/preferences/DesktopPreferences.cpp) 保存手动选择。默认的「跟随被控端」会在连接时使用已配对 Windows 或 macOS 宿主报告的采集尺寸。源尺寸有效时，运行时按实际宽高比建立视频流。手动指定尺寸优先。旧宿主缺少尺寸信息时沿用串流组件的偏好。Windows 把视频嵌入 [Aurora 会话窗](../desktop/app/qml/stream/NativeSessionWindow.qml)。窗口化时，Windows 和 macOS 都根据实际视频宽高比调整视口。macOS 的串流组件使用 [Aurora 原生连接窗口](../desktop/upstream/moonlight-qt/0017-aurora-macos-session-window.patch) 显示进度与失败，出画时交给 SDL 视频窗及其 AppKit 控制中心。原生连接入口在当前 Mac 补丁中省略了 QML 提示所需的 1.5 秒等待。窗口化时从连接状态打开详情。全屏时可从悬浮控制中心打开同一详情。面板使用当前会话的实测指标，缺失值隐藏。隐私保护由被控端确认是否支持和开启。

## Qt Quick 会话界面

Aurora 默认启用 `AURORA_QT_SESSION=1`。Windows 的 Aurora 会话窗和两端的串流组件共用 [QML 连接页](../desktop/app/qml/stream/ConnectionStage.qml)。Windows 在 Aurora 会话窗中嵌入串流画面。macOS 的 AuroraStream 管理连接窗口，并在出画时交给 SDL 视频窗及 AppKit 控制中心。

## 已配对设备的建连优化

- 让同一 Moonlight 进程验证已保存的主机证书，确认预期设备和目标应用，然后启动串流。通过进程状态区分未配对、证书变化、网络不可达和启动失败。未配对时进入首次授权流程。
- 保留当前转发租约的生命周期。账号或节点变化时取消连接，换路时释放旧租约。设备发现和首次授权继续按各自的操作使用转发。
- 分别记录点击、身份核验、选路、配对状态、宿主编码器就绪、RTSP 建连、首个视频包和首次呈现。Windows 当前的 `ready` 表示渲染器具备呈现条件。首次呈现计时从渲染端报告。按平台、网络路径、宿主冷启动和已就绪状态比较中位数与 P90。

## 会话内设置

| 操作 | 实现路径 |
| --- | --- |
| 全屏、鼠标模式、左右键互换、反向滚轮、本机静音和性能统计 | 将本机操作送到当前串流进程，复用窗口、输入和渲染处理。切换鼠标模式和按键映射时释放已经转发的按键。 |
| 分辨率、帧率、编码格式、HDR、声道和被控端扬声器 | 保存参数并在同一 Aurora 窗口内重新协商。原应用仍在运行时可用 Sunshine 的 `/resume`。重连清理阶段需保留原应用。当前宿主会在会话结束时释放隐私屏保，接入重连前须验证本地遮罩的连续性。 |
| 视频码率 | 由被控端报告活动编码器的重配置能力。发出码率请求后以实际应用的目标为准。当前编码器缺少该能力时可通过重连应用。 |

## 串流组件裁剪

[Windows 构建](../desktop/upstream/moonlight-qt/Build-Windows-Streamer.ps1) 与 [macOS 构建](../desktop/upstream/moonlight-qt/Build-Mac-Streamer.sh) 定义 `AURORA_STREAM_RUNTIME`。该宏限制启动入口，现有目标仍编译原版 QML 资源并部署 Qt Quick 相关文件。

将 Aurora 的启动、连接状态和错误处理接入原生会话入口后，可从运行时构建清单移除原版界面源文件及 `qml.qrc`。拆除启动代码和偏好类的 QML 引用后，移除 Qt Quick/QML 链接项及部署脚本的 `-qmldir` 扫描。依据编译依赖保留网络、证书、SDL、编解码、平台窗口和实际使用的资源。

验收时检查部署依赖，并从 Aurora 正常入口测试已配对连接、首次授权、重连、取消和失败重试。覆盖 Windows 与 macOS，以及局域网和内置 Tailscale。
