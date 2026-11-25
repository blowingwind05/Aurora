# 协议验收

[协议方案](README.md) · [连接与会话授权](connection.md) · [即时码率](live-bitrate.md)

## 合并条件

| 优先级 | 候选实现与影响 | 需要落地的改动和回归 |
| --- | --- | --- |
| P1 · 阻止合并 | 内置中继的 Moonlight 寻址检查 UUID 和活动端点。直连的播放与 `list` 仍走通用寻址，可匹配已存地址并使用另一路径。控制进程则使用建连阶段选定的 UUID 和精确端点，播放与控制可能脱节。 | 用同一精确目标约束 `list`、播放和控制。覆盖缓存地址变化、直连地址复用、主机 UUID 或证书变化、同一设备经三种路径连接。[Moonlight 寻址](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/computerseeker.cpp) · [建连与配对复核](../../desktop/app/src/desktop/NativeDesktopSessions.cpp#L205) |
| P1 · 阻止合并 | Sunshine 按 TLS 证书匹配唯一活动串流，状态请求未携带会话令牌。持同一已配对身份的不同播放进程会共享这一授权范围。请求通过 HTTPS 验证后可以等待编码线程，领取变更时也没有配对授权的复核。 | 由配对的启动响应交付令牌，查询与设置均匹配会话。复用停流与配对撤销路径，在应用前检查待执行操作。覆盖异证书、同证书不同会话、禁用或解绑后的排队请求，以及断流和重连。[会话所有权](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/rtsp.cpp) · [编码执行](../../desktop/protocol/bitrate/HostControl.h#L222) · [现有隐私撤销](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/nvhttp.cpp) |
| P1 · 阻止合并 | 单槽只限制同一时间的待执行数量。请求只有会话令牌和目标值，较早的设置晚到时仍可覆盖新值。已开始的驱动调用可在 HTTP 超时后成功。刷新结果更新了滑块，但主控仍按保存的旧值判定是否提交。 | 对会话版本做原子比较和递增，区分等待、失败及已确认。覆盖延迟、重复、乱序、丢失应答及重新选择旧偏好值。以远端确认值更新会话显示，按对应 ACK 保存偏好。[队列](../../desktop/protocol/bitrate/HostControl.h#L179) · [状态刷新](../../desktop/app/src/session/SessionBitrate.cpp#L47) · [偏好比较](../../desktop/app/src/desktop/NativeDesktopSessions.cpp#L285) |
| P1 · 阻止合并 | 旧宿主 404、当前编码器暂不可用和会话资格不满足可落到同一个 `unavailable`。主仓已允许重新查询，但设置路径仍可在缺少能力快照时进入重连。 | 区分明确缺少能力与暂态。状态未知时先查询，用户知情后再进行会中重协商。覆盖旧版 AuroraStream、旧宿主、首次出画、捕获重建、预算越界和隐私约束。[404 处理](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvhttp.cpp) · [能力缓存](../../desktop/app/src/session/SessionBitrate.cpp#L21) · [重协商决策](../../desktop/app/src/session/SessionVideo.cpp#L59) |
| P2 · 维护 | 码率入口单独组装已配对 POST，通用 `openConnection` 增加了仅码率使用的 404 输出参数。主仓已将 FEC、音频和包开销合并为 `adjusted_encoder_kbps`。 | 让已有配对 POST 封装统一处理证书、重定向、超时和响应上限，在控制层解释认证后的 HTTP 状态。用共享协议样例验证字段约束，并对齐首次协商和重配置的预算结果。[预算代码](../../desktop/protocol/bitrate/HostControl.h#L97) · [Moonlight 配对 POST](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvhttp.cpp) |

## 验收矩阵

| 范围 | 已有入口 | 新增覆盖 |
| --- | --- | --- |
| 证书和配对 | [配对离线测试](../../desktop/app/src/pairing/client/PairingClientFixtures.cpp) · [宿主管理证书测试](../../desktop/app/src/adaptors/sunshine/admin/tests/BundledAdminTest.cpp) | 使用本地测试证书验证实际 Moonlight 配对 HTTPS。覆盖证书替换、重定向、缺少或禁用的客户端证书、已固定证书的旧版 404。 |
| 会话与路径 | [会话监督测试](../../desktop/app/src/session/tests/SupervisorTests.cpp) · [转发测试](../../desktop/tailnet/relay_test.go) · [配对来源测试](../../desktop/tailnet/pair_source_test.go) | 局域网、系统 Tailscale 直达及内置中继使用同一授权断言。覆盖控制使用原租约、失效后清理、播放进程替换和隐私遮罩保护。 |
| 协议输入与兼容 | [控制进程测试](../../desktop/app/src/adaptors/moonlight/control/tests/ControlTest.cpp#L157) | 三端复用成功及拒绝样例。覆盖空或超长字段、重复字段、负数、溢出、上限、不可用与暂不可用、错误版本及异常响应。 |
| NVENC 与失败恢复 | [Sunshine 码率候选](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/rtsp.cpp) · [会话测试](../../desktop/app/src/session/tests/SupervisorTests.cpp#L361) | 使用可控时钟和假的驱动回调验证排队、超时及晚到结果，并用支持和不支持动态调整的 Windows 环境验证编码器能力与画面连续性。 |

Windows AuroraStream `d805192` 的[回环测试](../../tests/runtime-entry/Test-PinnedStreamerAccess.py)使用空宿主缓存和独立客户端证书验证双向 TLS。测试确认错误指纹在发送 HTTPS 请求前被拒绝，错误 UUID 和宿主拒绝的客户端也无法授权，选路探测的三次请求均走 HTTPS。同一证书生成的挑战签名用 OpenSSL 验证，并覆盖随机数、宿主指纹和授权范围任一字段变化后的验签失败。

Sunshine 授权候选的隔离夹具编译了验签、证书链与状态读写代码，验证真实 AuroraStream 签名的登记，以及错误宿主指纹、损坏签名、换证书或换范围、过期、重启、禁用、解绑、批量解绑和状态写入失败。Sunshine `7567a68` 通过 Windows 正式宿主打包，`54dc5c2` 通过 Windows 增量构建和原生管理回环。原生回环覆盖能力声明、管理认证、证明登记、短期授权撤销和禁用后拒绝。

双节点 tsnet 回归通过本地控制服务与 DERP 建立真实的认证通道，验证签名桥接、授权消息往返和实际节点 ID 的复核。隔离的 Windows 端到端验证使用主仓固定的 Moonlight `c7cb5b2` 和 Sunshine `54dc5c2`，通过了桌面配对客户端、tsnet、AuroraStream 签名以及原生 Sunshine 验签和撤销。测试使用独立配置，范围是身份与授权。冷启动到首帧、真实网络换路和 macOS 原生仍需验证。

同一锁定版本已通过 Windows 便携包组装、依赖扫描、SHA-256 校验及独立桌面的 `--check-native` 自检，并核对了包内 Moonlight 与 Sunshine 的源码提交。AuroraTailnet 也已从同一 Tailscale 提交交叉编译为 macOS arm64 和 x64，验证了构建元数据。

Windows 复验包 `Aurora-0.8-source-integrated-6eceae0f` 使用相同的上游锁定版本，通过依赖扫描、SHA-256 校验及独立桌面启动自检。会话隔离回归覆盖弱网计时清零、换路交接及断链恢复。包内 AuroraStream 的双向 TLS 测试覆盖 IPv4 与 IPv6 回环，证书不匹配时没有发送 HTTP 请求。

在没有宿主证书缓存的新配置下，锁定的 Moonlight `c7cb5b2` 能执行固定证书的播放访问检查，隐私和码率子命令却依赖旧配对保存的证书。基于该版本的本地 Moonlight 候选 `7741be0` 复用了播放探测的 HTTPS、指纹、UUID 和 `PairStatus` 校验，使用本次验证的证书发送控制 POST。`Test-PinnedStreamerAccess.py --check-session-controls` 在 Windows 上验证了 IPv4 和 IPv6 回环的隐私及码率状态请求。错误指纹、错误 UUID 和宿主拒绝授权时均没有发送控制 POST。回归还区分了已认证的隐私状态查询 404、宿主返回的暂不可用与修改请求失败。macOS 换路的 C++ 守卫通过了合成状态测试，AppKit 界面和原生构建仍待验收。

Windows 公网 IPv6 会话的隔离回归覆盖出画前的控制拒绝、旧组件兼容、指纹缺失、端点不一致、隐私状态未确认及经认证确认缺少隐私接口。桌面和播放组件已通过构建，主仓子模块锁定通过该回归的 Moonlight `7741be0`。真实公网连接、跨设备连续换路和 macOS 原生构建仍需验收。

使用独立 Git 索引，以 Moonlight `7741be0`、Sunshine `54dc5c2` 和 Tailscale `cbc74fe` 组装了 Windows 候选便携包。迁移至 `desktop/packaging/` 的串流构建入口、两个引擎版本记录、依赖扫描、SHA-256、隔离桌面启动自检和包内 AuroraStream 的 IPv4/IPv6 双向 TLS 回归均通过。macOS 原生验收会核对 donor 中的两个引擎版本记录后再重新组装。

局域网回迁的离线发现回归覆盖远程开始播放时没有候选、随后发现同一设备的新地址、重复或冲突的地址、未启用被控的设备及已失效的节点授权。使用同一组引擎组装了含回迁和比较冷却的 Windows 候选，两个引擎的版本记录、依赖扫描、SHA-256 和隔离桌面启动自检均通过。切换物理网络后持续出画的端到端行为仍待验收。

暂缓的 Sunshine 本地候选 `4595223` 曾通过 Windows 编译前端检查和协议回归，覆盖本次启动令牌的校验、旧请求及旧令牌的拒绝和停流后的失效。主仓共享解析和测试已恢复为现有码率协议，Sunshine 子模块继续固定在 `54dc5c2`。

## 审计快照

基线提交为 `ea421b0b4af9eeb51474696aba64c70c36f7d62f`，连同 2026-11-25 的未提交主控改动。Windows Sunshine 在审计期间接入外部候选，随后把初始协商与即时调整的开销计算合为同一实现。表中发现已复核到更新后的主仓补丁。

- Moonlight `0029-aurora-live-bitrate.patch` SHA-256 · `792e41af871ca383d9b97faf72f30e1ec6926e7102ab26668aeb75eea27b6485`
- Sunshine 主仓 `0015-aurora-windows-live-video-bitrate.patch` SHA-256 · `24f5a2ed852d5482a0b5df101886295bccc6b0ed3e0a3db259b279efdf635cdc`
- Sunshine 外部候选 `Aurora-sunshine-live-bitrate-ea421b0.patch` SHA-256 · `50a2351a30fa9ca07b01d27309553e7173b8a37aef04ed3367d1f0c8f8720640`

`stream-control-offline` 和 `AuroraSessionSupervisor` 在 Windows 上离线构建并通过。Sunshine 外部候选的合成测试使用四版 NVENC SDK 头文件通过，主仓更新后的控制器用 SDK 13.1 重跑通过。覆盖解析、会话授权、生命周期、预算和模拟驱动。Moonlight HTTPS 实现通过本地应用检查。真实 HTTPS、实际显卡与三条网络路径的端到端行为仍需验收。
