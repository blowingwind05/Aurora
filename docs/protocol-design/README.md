# Aurora 远程桌面协议方案

2026-11-25 · 设计草案 · Windows 与 macOS 建连 · Windows 即时码率

Aurora 沿用 Moonlight 和 Sunshine 的 GameStream 配对、建连、RTSP 协商和媒体通道。局域网、系统 Tailscale 和内置 Tailscale 共用同一套证书和会话授权规则。即时码率是活动串流的可选扩展，首版覆盖支持动态 CBR 重配置的 Windows NVENC 编码器。

> **新增控制先对齐，再合入。** 沿用现有配对证书、HTTPS 请求封装、路径租约和串流生命周期。每个接口应说明谁能调用、它属于哪一次串流、何时确认成功，以及旧版本和超时如何处理。每条约定都应有实现入口和回归用例。

## 交付内容

| 内容 | 入口 |
| --- | --- |
| 主控和被控端身份、配对、路径选择、隐私控制 | [连接与会话授权](connection.md) |
| 请求与响应、码率预算、并发、超时和版本兼容 | [即时码率](live-bitrate.md) |
| 审计结论、合并条件、实现映射与测试矩阵 | [协议验收](verification.md) |
| 运行时裁剪、建连等待和构建验证 | [内置运行时裁剪](runtime.md) |
| 运行时实现和构建入口 | [串流运行时](../streaming.md) |
| 完整上游源码树、锁定提交与文件路径与独立协议测试 | [上游源码阅读入口](../../desktop/upstream/README.md) |
| 会话窗口、控制中心和连接反馈 | [独立串流窗口](../visual-design/stream-session.md) |

## 组件职责

| 组件 | 职责 | 复用入口 |
| --- | --- | --- |
| Aurora 主控 | 选路、管理会话窗口和播放进程、展示被控端确认的状态、保存设备偏好。 | [建连入口](../../desktop/app/src/desktop/NativeDesktopSessions.cpp) · [会话监督](../../desktop/app/src/session/SessionSupervisor.cpp) |
| Moonlight | 使用已有配对证书建立 GameStream 连接，在选定路径发送已配对 HTTPS 控制请求。 | [配对实现](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvpairingmanager.cpp) · [已配对 HTTPS 控制](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvhttp.cpp) |
| Sunshine | 从已验证的 TLS 连接识别主控，将授权绑定到 RTSP 建立的活动串流，在编码线程应用参数。 | [隐私控制](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/nvhttp.cpp) · [即时码率候选](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/rtsp.cpp) |
| Tailscale | 提供直达节点或转发租约。内置中继透传已有 GameStream 端口。 | [Tailscale 转发](../../desktop/tailnet/relay.go) |

## 设计评审与实现验收

设计评审先确定各组件职责、身份和会话边界、失败语义及复用点。实现验收从正常建连入口沿调用链核实每条规则，用回归用例覆盖异步结果和旧版本。变更协议时，同一项工作应更新对应约定、实现和测试入口。

## 实现顺序

1. 让配对复核、实际播放和控制使用同一个已验证目标。以内置中继现有的精确匹配作为直连路径的实现基础。
2. 在既有 `/launch` 和 `/resume` 的已配对响应中交付会话令牌，随 RTSP 启动对象转交给活动串流。复用同一权限判定供码率和隐私控制调用。
3. 复用 `NvHTTP`、`ControlAdapter`、编码线程和 NVENC API 完成码率读写。新增范围限于会话令牌交付、版本化码率操作、并发判定和结果确认。
