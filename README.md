# Aurora（极光）

极光是面向自有设备的开源远程桌面和串流应用。登录、设备发现、配对、被控端管理和远控会话都在 Aurora 里完成。Windows 和 macOS 的发布包随应用提供 Moonlight 串流组件、Sunshine 宿主和 Tailscale 应用内节点。

如果你习惯用一款工具远程办公、管理家里的电脑或串流游戏，当熟悉的远控软件开始收费时，Aurora 提供了一个可自主管理的开源选择。

## 开始使用

1. **打开 Aurora。** Windows 便携包从 `Aurora.exe` 启动，移动时保留整个应用目录。macOS 解压后从访达打开 **Aurora**。
2. **登录并启用被控。** 在「Tailscale 账户」登录同一账号，在要被连接的电脑上开启「本机被控 → 允许本机被控」。macOS 被控要求 14.2 或更新版本。按系统提示授权屏幕录制和辅助功能，授权后重启被控组件。
3. **连接设备。** 从「我的设备」选择可连接的电脑，点击卡片画面进入远控。已有配对可以继续使用，首次连接会尝试后台配对。能够连到同一台已配对主机时，优先走局域网。

macOS 使用内置 Tailscale。Windows 可在「Tailscale 账户」选择内置节点或系统客户端。内置节点首次登录使用 Tailscale 官方网页，账户切换和连接状态在 Aurora 内管理。

串流偏好和「允许本机被控」的选择保存在当前用户的设置目录。关闭主窗口时，Windows 默认收进托盘，macOS 默认隐藏到菜单栏。关闭方式和登录自启动可在「设置 → 通用」调整。

设备卡优先使用被控电脑壁纸的本地缩略图。远控窗口和设备列表各有自己的工作区，从「控制中心」可以访问当前会话的操作。

用安装器部署的旧版 Windows Aurora，请先通过 Windows 设置卸载，再安装新版。便携包请解压到新目录。

在 Windows 被控端使用远程手柄需要 ViGEmBus。安装器会在驱动缺失时安装它。使用便携包时，可从 `Components/ViGEmBus` 运行随包附带的安装程序。

## 开发与设计

- [贡献指南](CONTRIBUTING.md) 包含开发环境、构建命令、代码定位和验证步骤。
- [完整视觉设计](docs/visual-design/README.md) 包含布局、颜色、组件、平台适配和验收口径。
- [上游组件](desktop/upstream) 记录 Moonlight、Sunshine 和 Tailscale 的构建入口与源码版本。

GitHub Actions 的 **Build Aurora for Windows** 生成便携 ZIP 和安装器。Windows 与 macOS 的下载包附带 SHA-256 校验文件，公开仓库的构建还会生成来源证明。[签名与下载验证](docs/signing.md)

旧桌面端及其构建脚本可从 [desktop-dotnet-archive](https://github.com/blowingwind05/Aurora/tree/desktop-dotnet-archive) 检出。

## 许可

Aurora 源码采用 [GPL-3.0-only](LICENSE)。随应用分发的组件保留各自的许可证和源码记录。[第三方声明](THIRD-PARTY-NOTICES.txt)
