# 连接与会话授权

[协议方案](README.md) · [即时码率](live-bitrate.md) · [协议验收](verification.md)

每次建连保留被控端 UUID、已固定的 Sunshine 证书、精确地址和端口，以及 Aurora 当前连接尝试。Aurora 设备间连接统一通过 Tailscale 核验账号、节点身份及授权，并为所选路径保留所需租约。局域网和公网 IPv6 直连也使用经该认证通道取得的会话材料，实际串流按网络条件自动选路。IP、UUID 和能力标记用于发现和选路，连接目标还需用已配对的证书和对应私钥验证。

## 配对与建连

| 阶段 | 约定 |
| --- | --- |
| 发现与选路 | 复用 Aurora 的发现和 `serverinfo` 检查。播放前可尝试已发现的多个路径，每次切换都重新验证目标并释放旧路径。 |
| 首次配对 | 沿用 GameStream 的 PIN 和证书交换。自动配对沿用已有授权流程。内置 Tailscale 将已验证的节点与转发源连接关联，系统模式沿用受控配对通道。Moonlight 保存 Sunshine 证书并经已配对路径复核。 |
| 已配对连接 | 局域网直连先经 Tailscale 取得预期证书。实际播放和控制 HTTPS 都校验指纹，并使用精确端点和被控端 UUID 约束目标。 |
| 会话建立 | Sunshine 从 `/launch` 或 `/resume` 的 TLS 连接取得已启用的配对证书。RTSP 建立串流时，从本次启动对象继承主控身份和会话令牌。串流就绪后，令牌才可用于会话控制。 |
| 会话结束 | 退出、换路、账号失效、解绑和禁用使相关控制授权失效。重连及播放进程替换使用新的会话令牌。已经进入驱动的调用在原会话内收尾。 |

[建连](../../desktop/app/src/desktop/NativeDesktopSessions.cpp) · [配对客户端](../../desktop/app/src/pairing/client/PairingClient.cpp) · [内置配对主机](../../desktop/app/src/pairing/host/EmbeddedPairingHost.cpp) · [系统配对主机](../../desktop/app/src/pairing/systemhost/SystemPairingHost.cpp) · [Sunshine 会话身份](https://github.com/blowingwind05/Sunshine-aurora/blob/1d1e55da5ca5a11b9a36584f6d348f3543bf209c/src/nvhttp.cpp)

## 应用层加密

被控端按应用层规则要求加密 RTSP、控制、音频和视频。主控在发送串流启动请求前核对固定的 HTTPS 证书，在协商阶段检查加密能力。局域网直连和两种 Tailscale 路径使用相同的规则。发现广播只提供地址线索。

Tailscale 负责确认账号和设备节点。发起局域网直连时，Aurora 从同账号且已授权的被控端通过 Tailscale 通道取得当前 TLS 指纹。Moonlight 在发送 HTTPS 请求前核对该指纹。已有匹配的本地凭据时直接启动播放。证书无法确认时可尝试已发现的 Tailscale 路径。证书私钥保存在各自设备，直连会话协商新的加密密钥。

播放组件支持从空宿主缓存开始，用 Tailscale 提供的指纹校验 HTTPS 握手，再核对宿主 UUID 和被控端返回的配对状态。主控仍以现有 Moonlight 客户端证书向 Sunshine 完成 TLS 私钥证明。无界面命令 `route-probe <ip:port> <host-uuid> access` 复用 `serverinfo` 检查这一授权。当前桌面端对首次授权仍走已有流程。 [TLS 证书校验](https://github.com/blowingwind05/moonlight-aurora/blob/5e36c37489046a286fcfe38f9ca9aa90adb4321c/app/backend/nvhttp.cpp) · [回环测试](../../tests/runtime-entry/Test-PinnedStreamerAccess.py)

### 主控端授权设计

首次授权复用 Aurora 已验证的 Tailscale 通道。被控端以通道关联的账号和稳定节点 ID 识别主控，通过现有本机授权检查决定该节点能否连接。主控使用现有凭据签署本次请求的随机挑战。挑战绑定双方节点、被控端指纹和主控证书摘要，仅在发出挑战的通道内有效。接收端在授权写入前重新核对节点及账号世代。

Sunshine 复用现有 GameStream TLS 的客户端证书认证。对于由 Tailscale 签发的授权，客户端证书与已认证节点、授权有效期和撤销状态关联。客户端使用私钥证明后才能启动或恢复串流。当前串流更换地址时复用授权，同时重新协商媒体密钥。节点撤销、账号切换或授权过期后，阻止新的启动或恢复。旧版宿主沿用现有配对交互。 [内置 Tailscale 节点复核](../../desktop/tailnet/auxiliary.go) · [系统 Tailscale 节点复核](../../desktop/app/src/pairing/systemhost/SystemPairingHost.cpp)

播放组件已支持生成证明。签名输入按顺序包含以零字节结尾的 `aurora-client-proof/1`、32 字节随机数、32 字节宿主 TLS 指纹、32 字节授权范围摘要和 32 字节主控证书 SHA-256。组件核对预期宿主指纹后，使用现有客户端私钥签名，向父进程返回公钥证书和签名。 [共享签名格式](../../desktop/protocol/identity/ClientProof.h) · [播放组件签名入口](https://github.com/blowingwind05/moonlight-aurora/blob/d805192de7201a9a50e8f3a4282299388688a017/app/main.cpp)

Sunshine 候选实现通过已认证的回环管理接口接收证明，核对自己的 GameStream 证书指纹，再用主控证书验签。登记沿用现有客户端记录。同一范围只可续期原证书，单次授权有效五分钟。证书绑定和撤销写入状态文件，重启后的授权需要重新取得。禁用或解绑的记录拒绝自动续期。

桌面授权交换在同一次 Tailscale 通道中提供签名挑战、宿主指纹与地址候选。被控端在提交证明前复核节点。桌面端按认证通道中返回的能力选择证书授权或兼容配对。短期授权会在会话内后台续期。同一账号快速重连时，只有宿主证书、本次宿主运行、主控证书和本地续期时间都匹配才复用。被控端处理账号切换时通过本机管理接口让原授权失效，再开放新账号的授权入口。实际建连耗时仍需验证。 [引擎版本](../../desktop/upstream/README.md)

## 网络路径

| 路径 | 播放与控制 |
| --- | --- |
| 局域网 | 直连所选 Sunshine 地址。控制使用同一地址的 GameStream HTTPS 端口及已固定的证书。 |
| 公网 IPv6 | Tailscale 认证通道返回宿主证书和至多四个公网 IPv6 候选。主控只在没有可用局域网或持续弱网时逐一测路。播放仍使用已确认的证书。公网地址需要宿主现有的 IPv6 监听和网络可达性。 |
| 系统 Tailscale | 通过系统 Tailscale 网络直连选定节点。沿用直连证书规则，账号和节点状态由已有系统客户端适配器核验。 |
| 内置 Tailscale | 主控使用绑定到目标节点的回环租约。控制走播放租约的本地 `base - 5` HTTPS 端口。中继透传 TLS，目标和证书由会话两端验证。 |

播放开始后，控制沿该会话的已选路径发送。租约失效、账号变化或证书不匹配时，停止发送新的会话控制请求。由建连接口重新选路并取得新授权。沿用现有的退出顺序，子进程退出后再释放其占用的本地租约。

[转发端口](../../desktop/tailnet/relay.go) · [租约校验](../../desktop/app/src/adaptors/tailscale/EmbeddedStatusClient.cpp) · [控制路径校验](../../desktop/app/src/session/SessionPrivacy.cpp) · [控制进程](../../desktop/app/src/adaptors/moonlight/control/ControlAdapter.cpp)

## 自动选路和切换

局域网可用时优先直接启动。没有可用局域网时，使用通过验证的 Tailscale 路径启动，首帧之后才比较同一宿主的可信公网 IPv6 候选。若 Tailscale 启动失败，可尝试已经通过认证通道取得的 IPv6 候选。后台比较最长 12 秒。候选须至少成功两次，并超过延迟或成功率阈值。探测失败率衡量认证连接的可达性。

已播放会话持续弱网 8 秒后使用这组候选做同口径 HTTPS 测量。收到恢复样本时清零计时，并取消因本次弱网启动的探测。结果只用于同一连接尝试、同一 Tailscale 授权和同一宿主指纹。Windows 复用现有串流窗口的停流通知。支持换路的 macOS 播放进程通过父进程提供的标准输入收到停流请求，在 SDL 主线程检查隐私状态和进行中的控制操作。隐私状态不允许交接时，旧播放继续运行。旧播放正常停流退出后才用 GameStream 恢复原应用。候选启动失败时最多尝试原路径一次。

在 Tailscale 或公网路径播放时，每隔 30 秒使用现有的 IPv4 局域网发现查找当前设备，包括开播时尚未出现的地址。发现结果先按已认证的节点和设备身份收窄，再使用当前会话的证书指纹和宿主 UUID 进行 HTTPS 探测。局域网三次检查均成功、中位数不高于 50 毫秒时停止比较公网候选。当前路径没有明显优势时自动回迁。一轮比较结束后至少间隔 30 秒再做下一轮，断链恢复可立即启动。首次建连沿用已有的发现结果。

Windows 公网 IPv6 播放使用候选引擎支持的控制消息。桌面端收到该能力消息并确认已出画后，隐私和即时码率请求使用播放会话的原端点、UUID 和已确认的宿主指纹。隐私状态经认证确认已关闭，或宿主经认证返回该接口不存在时，才允许换路。旧引擎继续按原流程播放和换路。

播放开始后，已绑定证书的会话在控制传输意外中断时，可先通过 Tailscale 重新确认原证书，再对备用路径进行固定证书的 HTTPS 实测。通过后才恢复同一个远端应用，使用新会话密钥和控制令牌。用户主动退出、被控端正常结束、应用约定退出，以及隐私控制状态未确认或仍在变更时，不执行自动恢复。Windows 也可在中继租约失效时走这一流程。macOS 的断链恢复由播放进程检查隐私状态。旧 macOS 播放进程继续使用原播放流程。

选路阈值和候选队列见 [NativeDesktopRouteQuality.cpp](../../desktop/app/src/desktop/NativeDesktopRouteQuality.cpp)。同一会话内的换路请求和退出确认见 [SessionSupervisor.cpp](../../desktop/app/src/session/SessionSupervisor.cpp) 与 [播放进程消息定义](../../desktop/runtime/stream/AuroraStreamSessionProtocol.h)。[会话回归](../../desktop/app/src/session/tests/SupervisorTests.cpp)覆盖正常交接、缺少证书、强制停流及 macOS 的隐私否决、旧版和未经请求的退出码。

以下是进一步的选路策略。发现数据中的地址只建立候选。账号或节点授权变更时取消相关探测，清除旧候选测量。

- 首次连接自动选路。局域网候选通过身份和可用性检查后直接使用，其中可包含局域网 IPv6 地址。只有缺少可用局域网、局域网断开或质量持续低于可用阈值时，才探测和比较公网 IPv6 直连与 Tailscale。
- 候选使用少量带序号的加密探测比较往返延迟、延迟波动和无响应比例。活跃串流另外记录实际传输表现。连接失败和超时按样本计入，过期或来自其他身份的结果弃用。比较范围仅包括已验证且符合优先级的链路。
- 当前链路仍可用时，候选须在持续观察期内超过配置的差值阈值才自动切换。恢复时使用单独的阈值和冷却期。断链触发故障切换。回到可用局域网后停止探测公网候选。
- 复用 GameStream `/resume`，在新路径创建新的加密会话和控制令牌，并保留被控端运行中的应用。旧子进程结束后释放它占用的中继租约。

## 会话控制权限

- Sunshine 先匹配请求的配对证书，再定位此次启动建立的活动串流。会话令牌由受固定证书保护的 `/launch` 或 `/resume` 响应交给实际播放进程，用于后续查询和设置。只有加密 RTSP、仍在运行且已发出视频的串流可接受控制。
- 码率和隐私共用配对证书及活动串流校验。隐私操作涉及全机显示，仍需检查唯一活动会话、采集后端和遮罩状态。重协商前确认遮罩关闭，避免旧串流退出时遮罩已释放而主控仍显示已保护。
- 复用 Sunshine 的配对撤销和会话停止入口。待执行操作记录所属授权，编码线程开始执行前检查授权仍有效。撤销先阻止尚未开始的操作，再清理该会话的资源。

当前候选已比较配对证书与串流拥有者。码率状态查询仍会按证书返回唯一活动串流的令牌，发版前须将令牌交付限定到该次启动。旧版隐私入口沿用已有规则，具备会话令牌的版本可迁移到共同授权校验。[码率所有权校验](https://github.com/blowingwind05/Sunshine-aurora/blob/1d1e55da5ca5a11b9a36584f6d348f3543bf209c/src/rtsp.cpp) · [隐私资格与撤销检查](https://github.com/blowingwind05/Sunshine-aurora/blob/1d1e55da5ca5a11b9a36584f6d348f3543bf209c/src/nvhttp.cpp)
