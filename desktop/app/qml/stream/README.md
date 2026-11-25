# Qt Quick 串流顶栏

将 `StreamSessionChrome` 放在串流窗口客户区的画面层之上，尺寸覆盖整个客户区。窗口化时图像层从 `contentTop` 开始。全屏时 `contentTop` 为 0，工具栏和菜单覆盖画面，图像层保持原位。设计和尺寸见[独立串流窗口](../../../../docs/visual-design/stream-session.md)及[圆角规范](../../../../docs/visual-design/visual-guidelines.md#圆角规范)。

```qml
import "qml/stream" as Stream

Stream.StreamSessionChrome {
    id: chrome
    anchors.fill: parent
    deviceName: session.deviceName
    phase: session.phase
    displays: session.displays
    activeDisplayId: session.activeDisplayId
    onDisplayRequested: function(displayId) { session.requestDisplay(displayId) }
}
```

使用支持自定义控件的 Quick Controls 样式，例如 Qt 5.15 与 Qt 6.6 都提供的 Fusion。平台窗口负责系统窗控、拖动、窗口外框和原生视频层的叠放。窗口化使用 `leadingWindowInset` 和 `trailingWindowInset` 提供系统窗控占用的逻辑像素。macOS 默认在右侧显示品牌图标。窗口宿主同步 `maximized`、`fullScreen` 及 `reduceMotion`。全屏时上缘悬停可唤出顶栏，宿主也可绑定 `revealControls` 或调用 `focusControls()`。宿主观察 `panelOpen` 和 `controlsVisible` 来协调本地鼠标释放、焦点和按键转发。串流画面存放在独立原生子窗口时，宿主需要为浮层提供适合该平台的叠放方式。

## 状态与操作

| 输入 | 约定 |
| --- | --- |
| `phase` | `Connecting`、`Connected`、`Failed`。首帧可显示后才传入 `Connected`。 |
| `networkQuality` | `Normal`、`Poor`、`Bad`，使用串流端已经判定的网络质量。 |
| `connectionMetrics` | 已格式化的字符串。键为 `latency`（往返时间）、`resolution`、`frameRate`、`bitrate`、`networkLoss`、`codec`、`decodeTime`。可以省略任何未提供的指标。 |
| `displays` | 数组，每项有唯一且稳定的字符串 `id`、完整 `name` 和布尔值 `ready`。可选 `shortName`、`kind`、`resolution`、`refreshRate`、`scale`，详情值为已格式化的字符串。宿主仅为本次会话创建且可移除的项设置 `removable: true`，移除等待确认时设置 `removalPending: true`。 |
| `activeDisplayId` | 已确认能显示的当前屏幕。宿主确认创建、切换、释放和关闭活动屏时的回退屏后发布快照。 |
| `canCreateDisplay` 等 | 宿主报告创建能力、`createDisplayUnavailableReason`、`creatingDisplay` 及 `displayActionError`。错误文案由宿主清除。`canCancel`、`canDisconnect`、`canSetFullScreen` 同样由宿主授予。 |

会话操作使用 `cancelRequested()`、`disconnectRequested()`、`displayRequested(string displayId)`、`createDisplayRequested()`、`removeDisplayRequested(string displayId)` 和 `fullScreenRequested(bool enabled)`。宿主接受创建或移除请求时应立即回报等待状态，并在运行时确认后更新显示器快照。处理函数也应拒绝重复或已过期的请求。

传入新的 JS 对象或数组发布快照，以触发 QML 更新。每个控制项的 `writable` 由宿主明确传入，`value` 表示已确认值。离散项还需传 `options: [{value, label, enabled}]`，其中 `enabled` 可省略。码率项传 `minimum`、`maximum` 和 `step`，单位均为 kbps。未提供的选项、数值和开关显示为不可用。菜单发出请求后继续显示宿主确认值，滑条拖动期间预览候选值，提交后回到确认值。

| 输入 | 请求信号 | 生效时机 |
| --- | --- | --- |
| `preferences.resolution`、`frameRate`、`bitrate`、`codec`、`hdr` | `resolutionRequested(string resolution)`、`frameRateRequested(int fps)`、`bitrateRequested(int kbps)`、`codecRequested(string codec)`、`hdrRequested(bool enabled)` | 由会话控制器确认即时调整或同窗口重协商 |
| `preferences.performanceOverlay` | `performanceOverlayRequested(bool enabled)` | 下次连接 |
| `preferences.audioConfiguration`、`remoteAudioMuted`、`muteOnFocusLoss` | `audioConfigurationRequested(string configuration)`、`remoteAudioMutedRequested(bool muted)`、`muteOnFocusLossRequested(bool muted)` | 下次连接 |
| `preferences.swapMouseButtons`、`reverseScroll` | `swapMouseButtonsRequested(bool enabled)`、`reverseScrollRequested(bool enabled)` | 下次连接 |
| `mouseMode` | `mouseModeRequested(string mode)`，宿主可提供 `desktop` 和 `relative` | 当前会话 |
| `privacyScreen` | `privacyModeRequested(string mode)` | 当前会话 |

宿主将启动参数写入现有偏好存储。如果映射到 `audio-on-host`，远端扬声器静音与该偏好布尔值方向相反。隐私保护在状态未知时传入 `null`，状态确认后传入 `{mode, writable}`。`mode` 为 `off`、`display-off` 或 `screensaver`。使用 `privacyScreenMessage` 说明等待或失败原因。仅在实际鼠标已捕获且宿主提供 `releaseMouseShortcut` 时才显示释放提示。

## 离线验收

在仓库根目录运行 Qt Quick 界面测试。

```powershell
pwsh -NoProfile -File ./tests/qt-ui/Replay.ps1
```

可用 Qt 5.15 的 `qmlscene tests/qt-ui/previews/Preview.qml` 浏览静态素材和演示数据。将窗口调整至 1000 × 620、760、759、490、489 和 400 × 280，查看页签圆脚、分隔线、留白、菜单对齐及滚动。用 Tab、Enter 和 Esc 操作。预览里的系统窗控区域按平台留白，系统行为由实际宿主提供。

`icons/` 中的七个 SVG 来自[串流原型](../../../../docs/visual-design/aurora-stream-preview.html)的内联图标。其余图标和品牌图沿用设计目录中的资产。
