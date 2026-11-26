# Aurora 远程桌面视觉方案

2026-11-20 · Windows 与 macOS · 浅色界面

> **新增内容先对齐，再合入。** 补上设计稿未覆盖的状态、说明或操作时，先确定它属于页面、卡片还是表单，再复用同一层级已有的边线、内边距和文字基线。尤其要检查可见文字的起点，按钮自身的内边距会让它看起来和上方文字错开。宽窗和窄窗都要看实际渲染。 [新增内容对齐规则](layout.md#新增内容如何对齐)

## 定稿主图标

![Aurora 定稿主图标预览](assets/brand/Aurora-transparent-512.png)

Aurora 使用这枚**青蓝渐变的折带 A**作为主图标。全系列的 **A 可见轮廓宽高比为 1∶1**，各版本画布留白按平台用途设置。按用途取用下表中的同一套定稿素材。

| 用途 | 选用文件 |
| --- | --- |
| Windows 应用、任务栏和快捷方式 | [Aurora-Windows.ico](assets/brand/Aurora-Windows.ico)（多尺寸图标）· [Windows 矢量源稿](assets/brand/Aurora-Windows.svg) · [任务栏尺寸预览](assets/brand/Windows-taskbar-size-preview.png) |
| Windows 原型标题栏左侧 | 同一个 [Aurora-Windows.ico](assets/brand/Aurora-Windows.ico) 的 32 × 32 px 帧，在标题栏缩到约 24 × 24 px 的可见尺寸 |
| Windows 通知区域托盘 | [Aurora-Windows.ico](assets/brand/Aurora-Windows.ico) 的小尺寸帧，由 Windows 按显示缩放取用 |
| macOS 应用、访达和程序坞 | [77% 留白版 ICNS](assets/brand/Aurora-mac-Dock-77.icns) · [Icon Composer 透明前景](assets/brand/Aurora-mac-foreground-648.png) · [尺寸对比](screenshots/components/macos-dock-icon-spacing-77.png) |
| macOS 菜单栏状态项 | [AuroraMenuBarTemplate.imageset](assets/brand/macOS-MenuBar/AuroraMenuBarTemplate.imageset/Contents.json)（20 pt，含 1× 和 2×）· [明暗预览](assets/brand/macOS-MenuBar/preview.png) |
| macOS 标题栏、网页和其他界面中的品牌标识 | [Aurora-transparent.svg](assets/brand/Aurora-transparent.svg)（透明矢量原稿）。需要位图时用 [Aurora-transparent-512.png](assets/brand/Aurora-transparent-512.png)，当前 macOS 原型使用其缩小版 |
| 白底文档、品牌展示 | [Aurora-white.svg](assets/brand/Aurora-white.svg)（白底展示稿） |

## 打开原型

- [Windows 浏览器原型](aurora-preview.html) · [Windows 托盘右键菜单](aurora-windows-tray-preview.html)
- [macOS 浏览器原型](aurora-macos-preview.html) · [macOS 菜单栏右键菜单](aurora-macos-menu-preview.html)

两版使用同一套页面、卡片和交互。Windows 的品牌在左，窗口按钮在右。macOS 的红黄绿窗口按钮在左，品牌在右。设备数据和操作结果均为演示。两版截图来自浏览器渲染，macOS 截图展示拟定的窗口样式与布局。

## 交付内容

| 内容 | 入口 |
| --- | --- |
| 页面、菜单、状态与反馈 | [交互设计](interaction.md) · [刷新提示](interaction.md#刷新提示) · [Tailscale 账户](interaction.md#tailscale-账户) · [退出登录确认](interaction.md#退出登录确认) · [未登录时的引导](interaction.md#未登录时的引导) |
| Tailscale 运行方式与账号切换 | [交互、双平台原型和状态截图](tailscale-runtime.md) |
| 独立串流窗口 | [三态原型、截图与实现规格](stream-session.md) |
| 对齐基线、间距、尺寸与响应式 | [布局与距离](layout.md) · [侧边栏宽度](layout.md#侧边栏宽度) |
| 配色、组件状态与新增元素验收 | [视觉与组件规范](visual-guidelines.md) · [字体与字重](visual-guidelines.md#字体与文案) · [圆角规范](visual-guidelines.md#圆角规范) · [次级文字操作](visual-guidelines.md#次级文字操作) · [设置和配置的分隔线](visual-guidelines.md#设置和配置的分隔线) · [设置下拉菜单](visual-guidelines.md#设置下拉菜单) · [开关状态](visual-guidelines.md#开关状态) |
| 两个平台的差异与窗口约定 | [Windows 托盘右键菜单](windows.md#托盘右键菜单) · [macOS 适配](macos.md) · [macOS 菜单栏右键菜单](macos.md#菜单栏右键菜单) |
| Windows 和 macOS 页面、组件截图 | [截图索引](screenshots.md) |
| 主图标文件与壁纸占位图 | [素材说明](assets/README.md) |
| 界面图标文件、来源和许可 | [界面图标清单](assets/icons/README.md) |
| 两个平台的可编辑原型 | [Windows 源文件](source/aurora-windows.html) · [Windows 托盘源文件](source/aurora-windows-tray.html) · [macOS 源文件](source/aurora-macos.html) · [macOS 菜单栏源文件](source/aurora-macos-menu.html) |

## 开发与验收

构建与代码入口见[Aurora 贡献指南](../../CONTRIBUTING.md)。正式界面的共同规格与验收项目见[视觉与交互规范](visual-guidelines.md)。原型尺寸和缩放规则见[布局与距离](layout.md)。
