# 界面图标清单

Windows、macOS 和手机端共用这套界面图标。Lucide 官方 SVG 来源固定为 **1.77.0**，原档保留 24 × 24 的绘图坐标和 `currentColor` 描边。分发时保留随附许可，涉及 Feather 衍生图标的 MIT 许可也在同一文件内。[上游版本](https://github.com/lucide-icons/lucide/tree/1.77.0/icons) · [本地许可](lucide/LICENSE)

| 使用位置 | 文件 |
| --- | --- |
| 「我的设备」侧栏图标、设备列表空状态 | [monitor.svg](lucide/monitor.svg) |
| 「本机被控」侧栏图标、「这台电脑」线框的源稿 | [panel-top.svg](lucide/panel-top.svg) |
| 「设置」侧栏入口 | [settings.svg](lucide/settings.svg) |
| 「更多」侧栏入口 | [menu.svg](lucide/menu.svg) |
| 「帮助」 | [circle-question-mark.svg](lucide/circle-question-mark.svg) |
| 「反馈」菜单入口 | [message-square.svg](lucide/message-square.svg) |
| 「关于」菜单入口 | [info.svg](lucide/info.svg) |
| 反馈页的邮箱 | [mail.svg](lucide/mail.svg) |
| 刷新设备、重新检测本机、刷新 Tailscale | [refresh-cw.svg](lucide/refresh-cw.svg) |
| 复制地址 | [copy.svg](lucide/copy.svg) |
| 文件传输 | [folder-sync.svg](lucide/folder-sync.svg) |
| 观看模式 | [square-play.svg](lucide/square-play.svg) |
| 终端 | [square-terminal.svg](lucide/square-terminal.svg) |
| 端口映射 | [panels-top-left.svg](lucide/panels-top-left.svg) |
| 设备卡片「更多工具」 | [Fluent Apps Regular](fluent/ic_fluent_apps_24_regular.svg)，右上角为菱形，16 px 显示，随按钮前景着色。[上游 SVG](https://github.com/microsoft/fluentui-system-icons/blob/main/assets/Apps/SVG/ic_fluent_apps_24_regular.svg) · [本地 MIT 许可](fluent/LICENSE) |
| 设备卡片「重启」「关机」 | [rotate-ccw.svg](lucide/rotate-ccw.svg) · [power.svg](lucide/power.svg) |
| 串流窗口「控制中心」 | [双横向滑杆](prototype/stream-control-center.svg)，原型定制，16 px 显示 |
| 手机串流「键盘」和「触控模式」 | [键盘](prototype/mobile-keyboard.svg) · [触控板](prototype/mobile-touchpad.svg)。原型 CSS 内嵌同一 SVG，修改源稿时同步更新。[手机原型样式](../../source/aurora-mobile.css) |
| 退出 Tailscale 登录 | [log-out.svg](lucide/log-out.svg) |
| 返回本机被控 | [arrow-left.svg](lucide/arrow-left.svg) |
| 已登录空列表中的「检查 Tailscale」 | 纯文字主按钮 |
| 「全部配置项」入口 | [chevron-right.svg](lucide/chevron-right.svg) |
| 展开连接帮助 | [chevron-down.svg](lucide/chevron-down.svg) |
| Windows 原型标题栏的最小化、最大化和关闭 | [minus.svg](lucide/minus.svg) · [square.svg](lucide/square.svg) · [x.svg](lucide/x.svg) |

## Windows 托盘菜单

命令使用 Microsoft Fluent System Icons 的 16 px Regular 版本。预览中图标作为单色遮罩，与命令文字同色。[上游版本](https://github.com/microsoft/fluentui-system-icons/tree/6c2c9075cf8952ef48369b2475233b6e9761efed) · [本地 MIT 许可](fluent/LICENSE)

| 使用位置 | 文件 |
| --- | --- |
| 打开极光 | [Open](fluent/ic_fluent_open_16_regular.svg) |
| 设置 | [Settings](fluent/ic_fluent_settings_16_regular.svg) |
| 退出极光 | [Dismiss](fluent/ic_fluent_dismiss_16_regular.svg) |

## 品牌素材与尺寸适配

- **Aurora 主图标与 macOS 菜单栏图标**使用项目定稿素材。按应用、窗口与系统菜单栏的用途取用对应变体。[主图标用法](../../README.md#定稿主图标)
- **Tailscale** 使用 Dashboard Icons 收录的九点 SVG，保留实点和 20% 不透明度的浅点，侧栏选中态可统一着色。Simple Icons 也有收录，浅点采用空心描边。这次选用保留深浅实点的版本。品牌标志独立于 Lucide 许可。[矢量图标](prototype/tailscale-dot.svg) · [Dashboard Icons 源文件](https://github.com/homarr-labs/dashboard-icons/blob/main/svg/tailscale.svg) · [Simple Icons 版本](https://github.com/simple-icons/simple-icons/blob/develop/icons/tailscale.svg)
- **「这台电脑」的大图标**与侧栏共用 Lucide 的 `panel-top` 造型。面板 SVG 收紧 viewBox，实际可见轮廓为 **28 × 28 px**，颜色沿用面板文字。这份派生 SVG 保留 Lucide 许可。[面板 SVG](prototype/panel-top-tight.svg) · [布局与尺寸](../../layout.md)
- **下拉菜单的选中对勾**使用 Phosphor Icons 的 Check Bold 官方 SVG。占位为 16 × 16 px，颜色跟随选中文字。[图标文件](phosphor/check-bold.svg) · [上游原稿](https://github.com/phosphor-icons/core/blob/2b75f3ad12b420c9504ef05df8d2564a28f8500e/assets/bold/check-bold.svg) · [本地 MIT 许可](phosphor/LICENSE)
- **「开源地址」**使用 GitHub Primer Octicons 的官方 GitHub 标志，尺寸 16 × 16 px。[图标文件](primer/mark-github-16.svg) · [上游原稿](https://github.com/primer/octicons/blob/e6626e159c4ba786d512e252182b62617014c752/icons/mark-github-16.svg) · [本地 MIT 许可](primer/LICENSE)

## 新图标选型

1. 先查本表。相同含义复用已有图标，并在不同页面保持方向和状态一致。
2. 本表没有合适图标时，优先从 Lucide 选语义和轮廓都接近的图标。只有尺寸或留白不同，可先用局部尺寸和留白调整，随后用实际像素核对。
3. Lucide 没有合适形状时，再查许可明确的其他开源库。仍没有合适图形，或关键轮廓必须和设计稿一致时，再自绘 SVG。自绘图标沿用 24 × 24 坐标、圆角线端与可跟随状态变色的描边，记录文件、用途和来源。
4. 在实际使用尺寸检查可见轮廓、垂直中心、明暗和状态。图标独立作按钮时应有可读名称、悬停提示和键盘焦点。需要匹配系统控件时，由各平台使用自己的窗口控件。
