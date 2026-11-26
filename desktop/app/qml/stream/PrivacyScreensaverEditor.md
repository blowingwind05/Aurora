# 串流隐私屏保编辑器

`PrivacyScreensaverEditor` 是可放入窗口、弹层或页面的 `FocusScope`。调用方提供尺寸、打开和关闭动作，并负责当前串流的设备身份和远端状态。编辑器只读取包内壁纸和本地预览 URL。

调用 `beginEdit(deviceName, savedStyle, defaultStyle, initialDraft)` 打开一次编辑。`initialDraft` 可省略，默认使用已确认的 `savedStyle`。`defaultStyle` 由设备能力决定。打开后可调用 `focusEditor()` 设置键盘焦点。处理 `closeRequested()` 后调用 `endEdit()`，待返回的旧请求会失效。

样式是 `{ id, kind, name, previewUrl }`。`id` 是调用方可与远端确认结果比较的稳定标识。`kind` 取 `preset`、`custom` 或 `default`。预设只需 `id` 和 `kind`，其余由包内目录解析。自定义样式的 `name` 通常是文件名。`previewUrl` 使用本地盘的 `file:///…` 或 Qt 资源 `qrc:/…`。无预览的已确认样式可以保留其 ID。

| 接口 | 调用方行为 |
| --- | --- |
| `importRequested(requestId)` | 打开平台文件选择器。选中图片后调用 `provideImport(requestId, style)`，本地预览可用后草稿才更新。取消或提前失败时调用 `finishImport(requestId, message)`，空消息表示取消。 |
| `saveRequested(requestId, style)` | 收到当前草稿快照后，由调用方打开本地素材、归一化并通过已认证的会话提交。 |
| `completeSave(requestId, outcome, message, confirmedStyle)` | 远端确认后传 `PrivacyScreensaverEditor.Succeeded`，必要时带远端确认的样式。明确拒绝或传输失败传 `Failed`。确认丢失时传 `Unconfirmed`，释放等待并禁止重复保存。 |
| `reconcileSaved(requestId, confirmedStyle)` | 重新读取同一设备确认的样式后调用。若远端回报不同标识，更新已确认记录并保留当前草稿供重试。 |

`available` 和 `disabledReason` 用于禁用编辑，`screensaverActive` 用于区分当前遮挡与下次生效，`importHint` 可显示被控端接受的格式和大小。恢复默认和大图预览只在本地编辑器内生效。调用方应只使用当前编辑器当前设备的请求结果。
