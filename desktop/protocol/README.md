# Aurora 通信协议

这里维护 Aurora 自己定义且由多个组件使用的协议代码。串流的配对证书、HTTPS 请求、会话所有权和编码器操作沿用上游组件的接入逻辑。

- 即时码率的[客户端请求与响应](bitrate/ClientProtocol.h)供桌面端和 Moonlight 使用。[宿主状态机](bitrate/HostControl.h)由 Sunshine 调用。
- [主控证书证明](identity/ClientProof.h)由 Moonlight 和 Sunshine 共用。
- [配对](../app/src/pairing)负责桌面端的授权流程、连接绑定、状态推进和撤销。[适配器](../app/src/adaptors)负责与 Moonlight、Sunshine 和 Tailscale 交互，包括读取 Moonlight 已保存的证书。
- [AuroraTailnet](../tailnet/README.md)是独立 Go 子进程。节点管理、转发和 RPC 处理随子进程源码维护。
- [协议设计](../../docs/protocol-design/README.md)记录包格式与验收要求。[上游源码入口](../upstream/README.md)记录构建复制位置与固定源码。

运行时适配器对桌面端提供稳定的请求、结果和错误类型。上游命令行、存储格式、线程与平台细节在适配器内处理。跨端协议需要扩展时，通过版本和能力标识保留旧行为。
