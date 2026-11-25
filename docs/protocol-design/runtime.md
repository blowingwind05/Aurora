# 内置运行时裁剪

[协议方案](README.md) · [连接与会话授权](connection.md) · [协议验收](verification.md)

Aurora 管理设备身份、选路、自动配对和会话界面。Moonlight 和 Sunshine 继续负责已配对 HTTPS、RTSP、输入和媒体。发布构建应裁掉被 Aurora 接管的上游功能，缩短关键路径时以实际往返次数和首帧时间为准。

## 构建范围

| 组件 | 从 Aurora 发布构建移除 | 继续使用 |
| --- | --- | --- |
| Moonlight | 原版主窗口、设备和应用模型、设置页资源、图形配对界面、上游更新界面、客户端 mDNS 和 Discord 集成。内置构建也停止联网获取上游兼容数据，沿用现有的本地兼容判定。 | 已配对的 `list`、后台首次配对、串流和会话控制，现有 SDL 和解码器，以及 Aurora 自己的连接页。 |
| Sunshine | UPnP 启动任务、源文件和 miniupnpc 链接依赖。 | Aurora 管理 API、局域网发现仍可用的服务发布、GameStream 证书验证、RTSP 和编码器。 |

[Moonlight 构建裁剪](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/computermanager.cpp) · [Moonlight 发现裁剪](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/moonlight-qt.pro) · [Sunshine UPnP 裁剪](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/cmake/dependencies/common.cmake)

## 入口与共用依赖

AuroraStream 和 AuroraHost 由 Aurora 提供进程入口，分别链接 Moonlight 和 Sunshine 的私有静态库。两个平台复用同一套启动输入和结果，窗口接入、停止通知及系统权限留在各平台适配。入口、引擎接入和发布验证见[运行时源码](../../desktop/runtime/README.md)。

Aurora 桌面程序负责设备、选路和设置，子程序通过启动输入和各自的控制接口使用这些数据。Aurora 与 AuroraStream 已可使用同一套 Qt 部署。Moonlight 与 Sunshine 的依赖能否进一步共用，由同平台的版本、ABI、构建选项和运行时链接结果决定。Windows 当前分别使用 MSVC 与 MSYS2 UCRT64 构建两套引擎。

## 已配对连接

Aurora 将选定的 UUID 和端点交给内置 Moonlight。已保存配对证书时，Moonlight 从选定端点直接读取 HTTPS `serverinfo`，同时核对 UUID、证书和配对状态。省去原版的唤醒和发现、明文 `serverinfo` 首探、五秒重试和公网连通性诊断。失败交还 Aurora，由现有连接流程处理。后台首次配对继续使用已有 PIN 和证书交换。

Aurora 当前仍先启动 `list` 复核配对，再启动播放进程，两者会各自确认主机。后续可让同一个播放进程完成核验和启动，用明确的失败结果驱动原有配对入口。合并这两步需要同时覆盖证书更换、未配对、租约撤销和取消连接。

[主控命令构建](../../desktop/app/src/adaptors/moonlight/MoonlightCommand.cpp) · [主控配对复核](../../desktop/app/src/desktop/NativeDesktopSessions.cpp) · [后台配对](../../desktop/app/src/pairing/client/PairingClient.cpp)

## 验收

- 构建产物包含 Aurora 连接页。原版 `qml.qrc`、客户端 mDNS 和 Discord 不进入内置 Moonlight。Sunshine 在没有 miniupnpc 的工具链上仍能配置和构建。
- 用回环的合成主机记录已配对请求。检查固定证书和 UUID 后只从选定端点读取 HTTPS 信息。覆盖证书变化、缺少配对、错误 UUID，以及局域网和内置中继的端口转换。
- 分别记录点击到配对复核完成、RTSP 建立和首帧显示。按局域网、系统 Tailscale、内置 Tailscale，以及宿主冷启动和就绪分组比较中位数与 P90。
