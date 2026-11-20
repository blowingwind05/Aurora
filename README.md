# Aurora（极光）

极光是面向自有设备的开源远程访问与桌面串流项目，目标是把跨网设备发现、配对和串流做成像 UU 远程一样好上手的一套产品。当前有 **Windows 图形原型**，不是独立安装包；实际串流仍由上游组件完成。

## 从极光窗口开始

1. 双击 `dist/Avalonia-0.8-integrated/Aurora.exe`。这是默认亮色的 Windows x64 图形程序，包含 .NET 和极光串流组件。移动时请移动整个 `dist/Avalonia-0.8-integrated/` 目录。
2. **从 Mac 或安卓控制这台 Windows：** 在侧栏打开「本机被控」，安装 Sunshine（已有安装不覆盖），再在同一页创建或登录 Sunshine 管理账号。修改设置后先保存，再自行选择是否重启被控服务。
3. Mac 或安卓安装 [Moonlight](https://moonlight-stream.org/)。同一个局域网用自动发现或「复制局域网」；异地使用「复制 Tailscale」，两端登录同一 Tailscale 账号。首次在本机被控设置里确认 PIN；之后可从 Moonlight 打开 `Desktop`。
4. **从极光控制其他设备：** 选择可连接的主机，点「连接桌面」。已有的 Moonlight 配对可继续使用。未配对时，极光会尝试后台配对，完成后打开桌面串流。

后台配对需要两端极光登录同一 Tailscale 账号，被控电脑运行极光并在「本机被控」允许自己的设备自动配对。极光用 Tailscale 的设备身份校验请求，随后用本机已登录并验证过证书的 Sunshine 管理接口完成配对。集成版串流组件包含[后台配对改动](desktop/upstream/moonlight-qt/0001-aurora-background-pair.patch)。原版 Moonlight 和仅使用局域网的新设备可通过兼容入口手动配对。

「记住登录」默认关闭；仅在勾选且 Sunshine 登录成功后，把密码交给当前 Windows 用户的凭据管理器。极光不把密码写入配置、日志或 Git，可在设置窗口点「忘记已保存的登录」。

Windows 管理界面只连接本机 Sunshine 的 HTTPS 接口，校验证书 SHA-256 指纹。读不到默认公开证书时，点「导入公开证书指纹」按 Windows 提示确认管理员授权。辅助进程只将该公开证书的指纹传回极光，不读取私钥或改动文件权限。指纹存放在当前用户的本机应用数据中，不是登录凭据。自定义证书路径则需要管理员让当前用户能够读取对应的公开证书。

「本机被控 → 全部配置项」在主窗口内打开，使用极光原生控件，覆盖 [Sunshine 2026.914.233613 的配置页](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/src_assets/common/assets/web/config.html)的 96 个设置项，含显示、网络、输入和编码器。部分复合参数使用文本/JSON；应用列表编辑、已配对客户端和故障排查不属于这个配置页，仍保留原版入口。保存时会重新读取当前配置、合入已改的字段并回读校验，保留其他设置值；Sunshine 的保存接口会重写配置文件，可能不保留注释和排版。

### 自动发现与选路

- 局域网通过 [Zeroconf](https://github.com/novotnyllc/Zeroconf) 查找 Sunshine/Moonlight 使用的 mDNS 服务，就算未安装 Tailscale 也能用。首版在活动的以太网/Wi-Fi 找同子网的 IPv4 主机；访客 Wi-Fi 或禁用组播的网络可能无法发现。
- Tailscale 仍从系统客户端读取同账号设备，不扫描地址段；对列表中已在线的设备读取默认串流端口的主机信息。仅在线不代表可串流。非默认端口目前仅支持通过局域网广播发现。
- 两条路径在串流主机 ID 一致时合并。首次后台配对通过 Tailscale 核验账号身份，之后优先尝试同一台主机的局域网连接，必要时回退 Tailscale。已有配对的设备可以在 Tailscale 离线时走局域网。
- 设备列表显示各设备的连接方式。顶栏不显示全局网络状态；发现失败时在设备区提示。
- 从侧栏打开「异地连接」，可在主窗口内检查系统 Tailscale 状态、恢复连接或开始登录；Windows 未安装时可选择通过 WinGet 安装官方客户端。登录在 Tailscale 官方浏览器页面完成，极光不接触账号密码。同一局域网可以直接使用，无需登录 Tailscale。

## 从源码构建

### macOS 主控版

- 集成版包名为 `Aurora-macOS-arm64-integrated.zip`（Apple 芯片）或 `Aurora-macOS-x64-integrated.zip`（Intel）。解压后双击 Finder 中的 **Aurora**。应用包含 .NET 和由 Moonlight 源码编译的极光串流组件。Aurora 管理配对、启动和结果反馈，串流画面在独立窗口显示。
- Windows 被控机需要运行 Sunshine 和极光。首次自动配对使用同一 [Tailscale](https://tailscale.com/download/mac) 账号确认设备身份，之后优先尝试局域网串流。macOS 如询问「本地网络」权限，可允许极光查找同网设备。
- 这些测试包仅做临时签名，未做 Apple 公证。若首次被系统拦截，只在确认来源可信后按 [Apple 的单应用打开流程](https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac)操作；不需要关闭系统防护。

在 Mac 构建需要 .NET 9 SDK、PowerShell 7、Apple 编译工具和带 `qmake`、`macdeployqt` 的 Qt 6。运行 `pwsh -File ./desktop/Build-Mac.ps1 -Architecture arm64`，Qt 工具可以通过 `-Qmake` 指定。构建自动获取固定版本源码、编译组件并签署 Aurora.app。在 Windows 上可用 `-WithoutStreamer -Rcodesign <路径>` 交叉生成单独标有 `shell` 的兼容性测试包。该包连接时使用 Mac 已安装的 Moonlight。

将构建流程提交到 GitHub 默认分支后，在 **Actions → Build Aurora for macOS → Run workflow** 选择 Apple 芯片版（`arm64`）、Intel 版（`x64`）或两者。构建完成后，在运行详情的 **Artifacts** 下载集成 ZIP。此流程只在手动触发时运行。

### Windows

在 Visual Studio x64 开发终端中运行 `pwsh -File ./desktop/upstream/moonlight-qt/Build-Windows-Streamer.ps1 -Qmake <路径>`，生成极光串流组件。安装 .NET 9 SDK 后，运行 `powershell.exe -NoProfile -File .\desktop\Build.ps1 -StreamerDirectory <组件目录>` 生成 `dist/Avalonia-0.8-integrated/Aurora.exe`。首次构建会从 NuGet 安装锁定依赖。`dist/` 在 Git 忽略列表中。

## 命令行与诊断

- 主控端：Windows、PowerShell 7.3 或更高版本，以及官方 [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt/releases)。命令行以 [Moonlight Qt 6.0.0](https://github.com/moonlight-stream/moonlight-qt/releases/tag/v6.0.0) 为基线；更旧的 Windows 版本可能不能正确返回应用列表。
- 被控端：Windows 上运行官方 [Sunshine](https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html)，在被控端本地完成初次设置。主控和被控都需要安装系统 [Tailscale](https://tailscale.com/download/windows)，并用同一 Tailscale 账号加入同一 tailnet。
- 极光不会自动安装软件、注册服务、修改防火墙或开启端口转发。首次配对在被控端 Sunshine 管理界面手动确认。

在主仓库目录里用 PowerShell 7 运行：

```powershell
pwsh -NoProfile -File ./Aurora.ps1 Doctor
pwsh -NoProfile -File ./Aurora.ps1 Devices
pwsh -NoProfile -File ./Aurora.ps1 Pair '目标设备名'
pwsh -NoProfile -File ./Aurora.ps1 Apps '目标设备名'
pwsh -NoProfile -File ./Aurora.ps1 Stream '目标设备名'
pwsh -NoProfile -File ./Aurora.ps1 Stream '目标设备名' -AppName 'Steam Big Picture'
```

`Pair` 由 Moonlight 发起配对，按 Moonlight 提示将 PIN 提交到被控端的 Sunshine。已配对的机器可直接用 `Apps` 或 `Stream`；串流默认使用 Sunshine 的 `Desktop` 应用条目，自定义名称以 `Apps` 列表为准。配对和串流结果以 Moonlight 窗口为准，退出码不能证明会话成功。若设备重名，用 `Devices` 显示的 `Id` 或 DNS 名精确选择。便携版 Moonlight 或自定义路径可用 `-MoonlightExe`、`-TailscaleExe` 指定。

`Doctor` 检查系统 Tailscale 是否可读、Moonlight 是否可找到，不会连接被控端，也不会检测对方的 Sunshine 是否运行。若受限终端拒绝读取 Tailscale 本地服务，在日常登录 Tailscale 的 Windows 用户会话中运行。看不到目标时，先检查两台机器的 Tailscale 登录账号、在线状态和网络访问策略。

## 复用边界

| 组件 | 原型怎么用 |
| --- | --- |
| [Tailscale](https://tailscale.com/docs/reference/tailscale-cli#status) | 读取已安装客户端的设备列表，只选当前账号的候选设备；Tailscale 负责跨网连通。 |
| [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt) | 调用官方程序的配对、应用列表及串流入口；沿用 Moonlight 的配对记录、解码和输入。 |
| [Sunshine](https://github.com/LizardByte/Sunshine) | 直接使用官方被控端，复用采集、编码和远程输入。 |

极光使用 Sunshine 和 Moonlight 的串流协议及配对记录。极光代码使用 [GPL-3.0-only](LICENSE)，Moonlight 的后台配对改动保留为针对上游源码的补丁。独立安装或一同分发的组件保留各自许可证。

## 下一阶段

先用两台 Windows 机器验证完整配对和串流，确认设备状态及错误提示；随后补统一的设备和连接界面。文件传输、剪贴板、显示器及隐私屏、移动端输入、访客访问按阶段评估。远程唤醒不属于首版。

设备发现回归测试不需要登录或连接主机：`pwsh -NoProfile -File ./tests/Discovery.Tests.ps1`。
