# Aurora（极光）

极光是面向自有设备的开源远程访问与桌面串流项目。Windows 和 macOS 集成版使用 Moonlight 主控、Sunshine 被控组件，以及基于 Tailscale `tsnet` 的应用内节点。

## 从极光窗口开始

1. 双击集成包里的 `Aurora.exe`。移动时请保留整个应用目录，包含 `Components/` 下的 Moonlight、Sunshine 和 Tailscale。
2. **从 Mac 或安卓控制这台 Windows：** 在侧栏打开「本机被控」，开启「允许本机被控」。内置 Sunshine 使用极光专属端口，独立安装的 Sunshine 可以同时运行。极光会在启动内置宿主时建立本机管理凭据。修改设置后可以从这里重启被控组件。
3. Mac 或安卓也可使用 [Moonlight](https://moonlight-stream.org/) 手动添加「复制局域网」或「复制 Tailscale」给出的带端口地址。跨网连接需要两端登录同一 Tailscale 账号。首次在本机被控设置里确认 PIN，之后可从 Moonlight 打开 `Desktop`。
4. **从极光控制其他设备：** 选择可连接的主机，点「连接桌面」。已有的 Moonlight 配对可继续使用。未配对时，极光会尝试后台配对，完成后打开桌面串流。

关闭主窗口时，Windows 默认最小化到托盘，macOS 默认隐藏到菜单栏。可在「设置 → 界面 → 关闭窗口时」切换。后台运行时，可从图标菜单重新打开窗口，或选择「退出极光」停止内置宿主并退出。

Windows 和 macOS 13 及更新版本会在极光首次运行时登记登录自启动。可在「设置 → 界面 → 开机自启动」关闭，极光会保留你的选择。自启动会打开极光，内置被控服务由「允许本机被控」控制。

「我的设备」会发现局域网和同一 Tailscale 账号下正在运行极光的设备。已识别过的同账号设备离线后仍可显示。离线壁纸叠加一层近白色薄雾，重新上线后恢复原图。设备记录存于当前用户的极光数据目录 `KnownDevices/`，局域网设备的本机标识存为 `presence-id`。

macOS 版极光使用内置 Tailscale 节点。Windows 版可以在「Tailscale 账户」中选择内置节点或系统客户端。内置节点有自己的设备名称、地址和登录状态，首次使用或登录过期时通过 Tailscale 官方网页授权。切换运行方式只影响极光自己的选择，两套节点可以同时在线。Windows 的系统模式可以从已登录的账号中选择，该操作会切换系统客户端的活动账号。

后台配对需要两端极光登录同一 Tailscale 账号，并启用本机被控。极光通过所选网络核验设备身份，Moonlight 继续验证 Sunshine 的配对证书。集成版串流组件包含[后台配对改动](desktop/upstream/moonlight-qt/0001-aurora-background-pair.patch)。原版 Moonlight 的待处理 PIN 请求在「本机被控」中确认。

连接后，设备列表与远控会话各有自己的窗口。Windows 会话窗由极光显示设备名、状态、全屏和断开，串流组件直接在窗内渲染。macOS 窗口化会话将原生窗控、设备名、连接状态和会话操作放在同一条顶栏，统计与释放鼠标可从「控制中心」打开。串流设置会保存在当前用户的设置目录。

Windows 和 macOS 集成版启用「本机被控」后，会保存主屏幕壁纸的缩略图。已配对的极光通过局域网或 Tailscale 获取预览，并保存在本地。主控只在「我的设备」位于前台时检查更新，每台设备通常间隔两分钟，退出远控后额外检查一次。壁纸没变时无需重传，连不上时沿用上次缓存。预览只处理系统能提供的壁纸图片，不采集桌面画面。

Windows 管理独立安装的 Sunshine 时，可以选用「记住登录」。勾选且登录成功后，凭据保存到当前用户的 Windows 凭据管理器，可用「忘记已保存的登录」清除。

Windows 管理界面只连接本机 Sunshine 的 HTTPS 接口，校验证书 SHA-256 指纹。内置版从当前用户的极光数据目录读取公开证书。读不到默认公开证书时，点「导入公开证书指纹」按 Windows 提示确认管理员授权。辅助进程只将该公开证书的指纹传回极光，不读取私钥或改动文件权限。指纹存放在当前用户的本机应用数据中，不是登录凭据。自定义证书路径则需要管理员让当前用户能够读取对应的公开证书。

Windows 的「本机被控 → 全部配置项」在主窗口内打开，使用极光原生控件，覆盖 [Sunshine 2026.914.233613 的配置页](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/src_assets/common/assets/web/config.html)的 96 个设置项，含显示、网络、输入和编码器。部分复合参数使用文本或 JSON。保存时会重新读取当前配置、合入已改的字段并回读校验，保留其他设置值。Sunshine 的保存接口会重写配置文件，可能不保留注释和排版。Windows 管理独立安装的 Sunshine 时可使用原版入口访问应用列表和配对记录。

### 自动发现与选路

- 局域网通过 [Zeroconf](https://github.com/novotnyllc/Zeroconf) 查找串流设备的 mDNS 广播，再查询极光的串流端口 `48989`。当前在活动的以太网或 Wi-Fi 查找同子网的 IPv4 主机。访客 Wi-Fi 或禁用组播的网络可能无法发现。
- 极光通过 Tailscale 本地接口读取同账号设备，对已在线的设备查询同一个极光串流端口。两条发现路径都只将内置宿主作为极光的连接目标。
- 两条路径在串流主机 ID 一致时合并。首次后台配对通过 Tailscale 核验账号身份，之后优先尝试同一台主机的局域网连接，必要时回退 Tailscale。已有配对的设备可以在 Tailscale 离线时走局域网。
- 设备列表显示各设备的连接方式。顶栏不显示全局网络状态；发现失败时在设备区提示。
- 内置模式通过 `tsnet` 传送发现、配对和串流数据。TCP 和 UDP 串流转发限于极光的 Sunshine 串流端口，管理接口保留在本机。节点随极光运行，关闭到托盘或菜单栏时继续在线。

## 从源码构建

- `desktop/` 存放 Windows 和 macOS 共用的 Avalonia 桌面端、资源和打包脚本。
- `desktop/Platform/Windows/` 存放依赖 Windows 凭据管理器、服务和命名管道的实现。
- `desktop/upstream/` 存放 Moonlight、Sunshine 和 Tailscale 的源码构建脚本及补丁。
- `cli/` 存放 PowerShell 命令行工具的模块，入口是仓库根目录的 `Aurora.ps1`。
- `tests/` 存放回归测试，`dist/` 存放本地构建产物。

| 组件 | 仓库内的接入代码和构建入口 | Windows 集成包 |
| --- | --- | --- |
| Moonlight | [固定版本、集成入口和补丁](desktop/upstream/moonlight-qt) | `Components/Moonlight/AuroraStream.exe` |
| Sunshine | [固定版本、构建脚本和补丁](desktop/upstream/sunshine) | `Components/Sunshine/AuroraHost.exe` |
| Tailscale | [`tsnet` 节点封装、构建脚本和测试](desktop/upstream/tsnet) | `Components/Tailscale/AuroraTailnet.exe` |

构建脚本按各自固定版本取回上游完整源码，临时检出和编译缓存放在 `desktop/obj/`。部署目录中的 `licenses/SOURCE.txt` 记录上游版本和来源。macOS 组件放在 `Aurora.app` 内，使用 macOS 的应用包目录结构。

接入上游改动时可以单独更新一个组件。修改该组件的固定版本后，先验证补丁和该组件的构建，再验证极光的发现、配对和串流。其他组件可以继续使用现有版本。

### macOS 集成版

- 集成版包名为 `Aurora-macOS-arm64-integrated.zip`（Apple 芯片）或 `Aurora-macOS-x64-integrated.zip`（Intel）。解压后打开 Finder 中的 **Aurora**。应用包含从源码编译的 Moonlight 主控组件和 Sunshine 宿主组件。旧的 `source-integrated.zip` 测试包是此前的主控版。
- macOS 14.2 及以上可在「本机被控」启用宿主。首次使用时，在系统提示中允许 Aurora 或 Aurora Host 使用屏幕录制、辅助功能和按需请求的音频或本地网络权限。修改权限后可在极光内重启宿主。
- 这些测试包仅做临时签名，未做 Apple 公证。若首次被系统拦截，只在确认来源可信后按 [Apple 的单应用打开流程](https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac)操作；不需要关闭系统防护。

在 Mac 构建需要 .NET 9 SDK、PowerShell 7、Apple 编译工具、带 `qmake` 和 `macdeployqt` 的 Qt 6，以及 [Sunshine 构建依赖](desktop/upstream/sunshine)。按[内置节点构建说明](desktop/upstream/tsnet)生成对应架构的 `AuroraTailnet`，然后运行 `pwsh -File ./desktop/Build-Mac.ps1 -Architecture arm64 -TailscaleDirectory ./desktop/obj/tailnet-mac`。Qt 工具路径可通过 `-Qmake` 指定。可用 `-WithoutHost` 构建标记为 `controller` 的主控包。Windows 上可用 `-WithoutStreamer -Rcodesign <路径>` 交叉生成标记为 `shell` 的兼容性测试包。

将构建流程提交到 GitHub 默认分支后，在 **Actions → Build Aurora for macOS → Run workflow** 选择 Apple 芯片版（`arm64`）、Intel 版（`x64`）或两者。构建完成后，在运行详情的 **Artifacts** 下载集成 ZIP。此流程只在手动触发时运行。

### Windows

运行 [Moonlight](desktop/upstream/moonlight-qt)、[Sunshine](desktop/upstream/sunshine) 和 [Tailscale](desktop/upstream/tsnet) 的源码构建脚本，保留各脚本输出的部署目录。安装 .NET 9 SDK 后，在 PowerShell 中执行以下命令。

```powershell
.\desktop\Build.ps1 -StreamerDirectory <Moonlight目录> -SunshineDirectory <Sunshine目录> -TailscaleDirectory <Tailscale目录>
```

Windows 集成包写到 `dist/Avalonia-0.8-source-integrated/`，首次构建会从 NuGet 获取锁定的依赖。`dist/` 在 Git 忽略列表中。

## 命令行与诊断

下列命令用于早期 PowerShell 工具。图形桌面端使用上面的集成版构建流程。

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
| [Tailscale](https://github.com/tailscale/tailscale) | Windows 和 macOS 的应用内节点使用从源码构建的 `tsnet`。Windows 版也可选用系统客户端。 |
| [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt) | 调用官方程序的配对、应用列表及串流入口；沿用 Moonlight 的配对记录、解码和输入。 |
| [Sunshine](https://github.com/SunshineStream/Sunshine) | Windows 集成版从源码编译被控组件，复用采集、编码和远程输入。 |

极光使用 Sunshine 和 Moonlight 的串流协议及配对记录。极光代码使用 [GPL-3.0-only](LICENSE)，Moonlight 的后台配对改动保留为针对上游源码的补丁。独立安装或一同分发的组件保留各自许可证。

## 下一阶段

先用两台 Windows 机器验证完整配对和串流，确认设备状态及错误提示；随后补统一的设备和连接界面。文件传输、剪贴板、显示器及隐私屏、移动端输入、访客访问按阶段评估。远程唤醒不属于首版。

设备发现回归测试无需登录或连接主机，可直接运行 `pwsh -NoProfile -File ./tests/Discovery.Tests.ps1`。桌面端测试通过 `-AssemblyPath` 接收本次构建的 `Aurora.dll`。
