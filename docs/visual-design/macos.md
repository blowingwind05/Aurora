# macOS 适配

[打开 macOS 原型](aurora-macos-preview.html) · [查看 macOS 标题栏](screenshots/macos/07-titlebar.png)

独立串流窗口把红黄绿窗控、设备名、控制中心与品牌图标放在同一条顶栏。红黄绿沿用系统能力，品牌图标放右端。[串流窗口规格](stream-session.md) · [macOS 串流截图](screenshots/stream-session/macos-connected.png)

| 项目 | Windows | macOS 方案 |
| --- | --- | --- |
| 窗口控制 | 右上最小化、最大化、关闭 | 左上红、黄、绿三个系统控制 |
| 品牌组合 | 左上 | 原型选用右上 |
| 品牌字体 | Segoe UI 与 Microsoft YaHei UI | 系统字体与 PingFang SC，跨平台浏览器可回退 |
| 应用设置 | 左下「设置」 | 同一左下入口与相同设置内容，另支持 ⌘+, |
| 系统应用图标 | Windows 多尺寸 ICO | 透明前景叠独立白底，由系统应用平台圆角 |
| 系统状态入口 | [任务栏通知区域](windows.md#托盘右键菜单)，使用彩色 Windows ICO 小尺寸帧 | [AuroraMenuBarTemplate.imageset](assets/brand/macOS-MenuBar/AuroraMenuBarTemplate.imageset/Contents.json)，20 pt 单色模板，由系统着色 |

右置是本方案的品牌排版选择。靠左也可以，把品牌组合放在红黄绿按钮右侧即可。页面内容的大标题继续位于各页内容区左上，同 Windows 共用基线。Apple 的工具栏指南提供靠左或居中的视图标题布局。 [Apple 工具栏指南](https://developer.apple.com/design/human-interface-guidelines/toolbars)

正式 macOS 应用沿用系统窗口按钮、窗口拖动与活动状态，保留应用菜单中的「设置…」和 ⌘+,。原型用浏览器演示标题栏外观，⌘+, 打开同一套设置页面。系统窗口状态与应用菜单由正式版本接入。 [Apple 窗口指南](https://developer.apple.com/design/human-interface-guidelines/windows) · [Apple 设置指南](https://developer.apple.com/design/human-interface-guidelines/settings)

业务页面沿用 Windows 版的导航、卡片、字段、文案、颜色和间距规则。macOS 的 Tailscale 账户由 Aurora 内置节点提供。macOS 方案的截图在 Windows 上通过 Chromium 渲染，系统字体回退和原生窗口行为以实际 macOS 实现为准。

接入菜单栏时，把整个 `AuroraMenuBarTemplate.imageset` 加入 `Assets.xcassets`。如用 AppKit 手动加载图像，将 `NSImage.isTemplate` 设为 `true`。素材包含 20 × 20 px 的 1× 图和 40 × 40 px 的 2× 图。

## 菜单栏右键菜单

[打开菜单原型](aurora-macos-menu-preview.html) · [查看菜单截图](screenshots/macos/17-menu-bar.png)

菜单从单色 A 的下方展开，与状态项右缘对齐。浅色原型用白底，菜单宽 **208 px**，圆角 **11 px**，四周留 **5 px**，命令行高约 **30 px**。使用已定稿的菜单栏单色 SVG，菜单项左对齐，悬停和键盘选中时使用 Aurora 的浅蓝灰底。

| 顺序 | 菜单项 | 动作 |
| --- | --- | --- |
| 1 | 打开极光 | 恢复主窗口。 |
| 2 | 设置… | 恢复主窗口并打开「设置」。 |
| 3 | 退出极光 | 退出应用。菜单只在这一项前加一条分隔线。 |

左键点按状态项恢复主窗口。右键、按住 Control 点按或用键盘激活状态项时打开菜单。打开后用 ↑ / ↓ 选择、Return 执行、Esc 关闭。Esc 关闭后焦点回到状态项。点击菜单外也可关闭。三项在未登录时仍可使用。浏览器原型用隐藏主窗口演示退出，可以点菜单栏图标重新打开。正式实现沿用系统状态项和原生菜单的可访问性，系统绘制实际的菜单外观。[Apple NSStatusItem](https://developer.apple.com/documentation/appkit/nsstatusitem) · [Apple 菜单栏指南](https://developer.apple.com/design/human-interface-guidelines/the-menu-bar)

默认关闭主窗口时隐藏到菜单栏。用户可在「设置 → 界面 → 关闭窗口时」改为「退出极光」。普通窗口通过标题栏空白区域拖动。主窗口最大化或全屏时，外框四角为直角，工作区遵循[统一的圆角与边距](layout.md#窗口与导航)。[关闭窗口偏好](screenshots/macos/23-window-preference.png)
