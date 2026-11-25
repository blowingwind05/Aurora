# Windows 适配

## 托盘右键菜单

[打开 Windows 托盘原型](aurora-windows-tray-preview.html) · [浅色截图](screenshots/components/windows-tray-menu.png) · [深色截图](screenshots/components/windows-tray-menu-dark.png) · [macOS 对应设计](macos.md#菜单栏右键菜单)

入口是任务栏通知区域里的彩色 Aurora A，复用 [Windows 定稿 ICO](assets/brand/Aurora-Windows.ico) 的小尺寸帧。图标由 Windows 管理，可能显示在通知区域或折叠区。右键后，菜单从图标附近向任务栏外侧展开。任务栏在屏幕底部时，菜单向上弹出。正式界面的定位、边缘避让和显示缩放交给系统。[Microsoft 通知区域文档](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area)

托盘入口与弹出位置沿用 Windows 通知区域的习惯。菜单面板采用 Windows 11 风格，宽 160 px，外圆角 8 px，内边距 5 px。浅色用白底、1 px #DBE2ED 边线和柔和阴影；深色用 #292D35 面板和 #4B5260 边线。面板从左到右保持同一底色。Qt 版在托盘菜单范围内应用这套外观，并跟随 Windows 浅色、深色和高对比设置。

三行命令各高至少 31 px，文字 13 px。微软 Fluent 线图标为 16 px，距文字 8 px，随文字色。在「退出 Aurora」上方放一条 1 px 淡线，浅色分隔为 #E6EAF1。悬停铺浅蓝灰，默认项「打开 Aurora」半粗显示。

| 顺序 | 菜单项 | 图标 | 动作 |
| --- | --- | --- | --- |
| 1 | 打开 Aurora | [Open](assets/icons/fluent/ic_fluent_open_16_regular.svg) | 恢复并聚焦主窗口，保留当前页面。 |
| 2 | 设置 | [Settings](assets/icons/fluent/ic_fluent_settings_16_regular.svg) | 恢复主窗口并直接打开左下「设置」对应的页面。 |
| 3 | 退出 Aurora | [Dismiss](assets/icons/fluent/ic_fluent_dismiss_16_regular.svg) | 结束应用并移除托盘图标。 |

左键点托盘图标恢复主窗口。进入通知区域后可用键盘打开快捷菜单，↑ / ↓ 切换命令，Enter 执行，Esc 收起。点击菜单外也能收起。默认关闭主窗口时最小化到托盘，可在「设置 → 界面 → 关闭窗口时」改为退出。三条菜单命令在 Tailscale 未登录时仍可使用。[Microsoft 任务栏文档](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar)

浏览器原型默认展开菜单，方便直接检查外观。点菜单外关闭后可右键重开，也可聚焦托盘图标后按 Shift+F10。退出演示会隐藏主窗口，托盘入口保留以便重复预览。

按 Windows 菜单文案惯例，命令需要额外输入才能完成时才加省略号。「设置」点击即打开页面，文案直接写「设置」。[Microsoft 菜单文字规范](https://learn.microsoft.com/en-us/windows/win32/uxguide/cmd-menus)

## 远控显示

「本机被控」在常用设置下方用独立的「远控显示」子面板放两个下拉框。「远控画面来源」默认按原有显示设置，也可选择受信任的虚拟显示器。「远控时的本地显示器」默认保留本地显示器。选择虚拟显示器后可将其改为「关闭所有本地显示器」，断开后恢复原来的显示布局。[远控显示截图](screenshots/pages/23-host-display.png)

先前选中的虚拟屏不可用时，「远控画面来源」显示「已选虚拟显示器不可用」，用户可重新选择。关屏或目标输出校验失败时，连接反馈显示失败原因并终止本次连接，以免隐私会话落到本地屏幕上。
