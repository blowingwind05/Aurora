# Tailscale 运行方式与账号切换

[Windows 原型](aurora-preview.html) · [macOS 原型](aurora-macos-preview.html) · [原有账户交互](interaction.md#tailscale-账户)

在侧栏打开「Tailscale 账户」。macOS 通过 Aurora 内置的 Tailscale 节点连接。Windows 已安装系统 Tailscale 时，可选择使用系统客户端。原型中的账号、网络和地址均为示例。

## 页面结构

运行方式和当前账号分别显示。Windows 系统模式的账号选择会改变系统客户端的当前身份。macOS 账户页以应用内节点的登录和连接状态为准。

| 区域 | Windows | macOS |
| --- | --- | --- |
| 运行方式 | 检测到系统客户端时，下拉提供「内置 Tailscale」和「系统 Tailscale」，其余情况只提供内置选项 | 下拉仅提供「内置 Tailscale」 |
| 当前账号 | 系统模式的头像、姓名和邮箱区域可展开已保存账号。连接状态紧贴姓名，所属网络和地址在下方。 | 显示应用内节点的当前账号、所属网络和地址。 |

运行方式和当前账号沿页面内容线排成两张卡片，间距 **12 px**。卡片内左右留 **14 px**，运行方式字段宽至 **235 px**，常规宽度右对齐。登录后右上角只放「退出登录」。窄窗将运行方式字段移到标题下一行，退出登录缩为带提示的图标，姓名和连接状态空间不足时在身份区内换行。未登录或暂停时，卡片显示对应的登录或恢复按钮。

在账号头像和文字下方展开系统已保存账号。每项首行显示姓名，第二行显示邮箱和所属网络。当前项用侧栏选中灰和粗对勾，其他项悬停用浅蓝灰。长邮箱可换行，列表过长可滚动。展开时才显示提示「切换也会更改系统 Tailscale 的当前账号」。Esc 或点击外部收起菜单，键盘可用方向键浏览。没有可用账号时以登录入口为主，尚有已保存账号时仍允许改选。

## 切换状态

| 场景 | 行为 |
| --- | --- |
| 切换运行方式 | 确认框说明 Windows 上 Aurora 将使用的服务。内置节点和系统客户端各自保留登录状态。点「取消」回到原字段。 |
| 切换系统账号 | 选择另一个已保存账号。检测到正在进行的远控时先确认，否则立即开始。确认框展示目标邮箱和断开会话的风险。 |
| Windows 系统客户端未安装 | 下拉只提供内置选项，沿用内置模式的说明和登录入口。 |
| 正在切换 | 在对应卡片显示加载环及「正在切换」。运行方式、账号选择、刷新及连接入口暂缓操作，设备列表在确认新身份前不展示。开启减少动态效果时加载环静止。 |
| 切换成功 | 读取实际服务和账号后更新选中项、侧栏、账号信息和设备列表。窗口中央短暂显示「已切换至…」，复用检查更新的轻提示，约 3 秒后淡出，界面始终可继续操作。切回已有且有效的登录时直接恢复。 |
| 登录过期 | 显示「此账号需要重新登录」，聚焦该模式的登录入口。其他已保存账号仍可改选。 |
| 切换失败 | 能确认旧连接仍有效时保留旧选项与原账号，显示原因并提供「重试」。旧连接也失效或状态未确认时显示真实状态，设备保持锁定。 |

切换和退出登录使用独立的确认框。只有退出当前账号才清除相应登录。底层切换系统账号会改变 Tailscale 客户端的活动账号，已保存账号一般无需重新认证，密钥过期时需要重新登录。 [Tailscale 账号切换说明](https://tailscale.com/docs/features/client/fast-user-switching)

## 截图

- [Windows 内置模式](screenshots/tailscale-runtime/windows-builtin.png) · [Windows 系统模式](screenshots/tailscale-runtime/windows-system.png)
- [展开系统账号](screenshots/tailscale-runtime/account-menu.png) · [有会话时切换确认](screenshots/tailscale-runtime/switch-confirm.png) · [切换中](screenshots/tailscale-runtime/switch-loading.png) · [切换成功](screenshots/tailscale-runtime/switch-success.png) · [切换失败](screenshots/tailscale-runtime/switch-failure.png) · [窄窗](screenshots/tailscale-runtime/compact.png)
