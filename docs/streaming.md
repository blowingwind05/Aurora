# 串流运行时开发

[贡献指南](../CONTRIBUTING.md) · [串流窗口视觉规范](visual-design/stream-session.md)

## 当前实现

[建连入口](../desktop/app/src/desktop/NativeDesktopSessions.cpp) 核验设备身份、选择局域网或 Tailscale 路径、确认配对，再启动 [Moonlight 串流组件](../desktop/upstream/moonlight-aurora)。macOS 的 Tailscale 路径由内置节点提供。Windows 可以选用内置节点或已安装的系统客户端。局域网连接沿用已建立的设备信任和 Sunshine 证书校验。

确认配对时，Aurora 调用 `Moonlight list`。启动串流时，Moonlight 再解析主机和目标应用。内置 Tailscale 的配对检查和串流共用所选路由的[转发租约](../desktop/app/src/session/SessionSupervisor.cpp)。[启动警告处理](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/cli/commandlineparser.cpp) 将警告写入日志并让 CLI 继续执行。

[串流偏好](../desktop/app/src/preferences/DesktopPreferences.cpp) 保存手动选择。默认的「跟随被控端」会在连接时使用已配对 Windows 或 macOS 宿主报告的采集尺寸。源尺寸有效时，运行时按实际宽高比建立视频流。手动指定尺寸优先。旧宿主缺少尺寸信息时沿用串流组件的偏好。Windows 把视频嵌入 [Aurora 会话窗](../desktop/app/qml/stream/NativeSessionWindow.qml)。窗口化时，Windows 和 macOS 都根据实际视频宽高比调整视口。macOS 的串流组件使用 [Aurora 原生连接窗口](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/cli/aurora-mac-stream.mm) 显示进度与失败，出画时交给 SDL 视频窗及其 AppKit 控制中心。原生连接入口在当前 Mac 入口中省略了 QML 提示所需的 1.5 秒等待。窗口化时从连接状态打开详情。全屏时可从悬浮控制中心打开同一详情。面板使用当前会话的实测指标，缺失值隐藏。隐私保护由被控端确认是否支持和开启。

## Qt Quick 会话界面

Aurora 默认启用 `AURORA_QT_SESSION=1`。Windows 的 Aurora 会话窗和两端的串流组件共用 [QML 连接页](../desktop/app/qml/stream/ConnectionStage.qml)。Windows 在 Aurora 会话窗中嵌入串流画面。macOS 的 AuroraStream 用 Qt Quick 绘制连接页顶栏，并在开始播放时交给 SDL 视频窗口。窗口化控制中心首页复用 Qt Quick，系统窗口边框、已连接顶栏和控制中心设置子页由 AppKit 承载。

## 已配对设备的建连优化

- 让同一 Moonlight 进程验证已保存的主机证书，确认预期设备和目标应用，然后启动串流。通过进程状态区分未配对、证书变化、网络不可达和启动失败。未配对时进入首次授权流程。
- 保留当前转发租约的生命周期。账号或节点变化时取消连接，换路时释放旧租约。设备发现和首次授权继续按各自的操作使用转发。
- 分别记录点击、身份核验、选路、配对状态、宿主编码器就绪、RTSP 建连、首个视频包和首次呈现。Windows 当前的 `ready` 表示渲染器具备呈现条件。首次呈现计时从渲染端报告。按平台、网络路径、宿主冷启动和已就绪状态比较中位数与 P90。

## 会话内设置

| 操作 | 实现路径 |
| --- | --- |
| 全屏、鼠标模式、左右键互换、反向滚轮、本机静音和性能统计 | 将本机操作送到当前串流进程，复用窗口、输入和渲染处理。切换鼠标模式和按键映射时释放已经转发的按键。 |
| 分辨率、帧率、编码格式和 HDR | Windows 在当前窗口内按新参数重协商，继续使用所选路由和远端应用，首帧就绪后确认设置。切换失败时尝试恢复原参数。隐私屏保开启或状态未确认时，以及停止串流会退出远端应用时，暂缓重协商。macOS 目前在下次连接应用这些参数。 |
| 视频码率 | Windows 和 macOS 主控可向支持重配置的 Windows NVENC 被控端请求即时调整，上限为本次协商的码率预算。Windows 主控可通过同窗口重协商调整预算，macOS 主控对不支持即时调整的宿主依旧在下次连接时应用。 |

已确认的画面偏好由主控端按账号、网络和被控端身份保存。初次连接时仍使用「跟随被控端」获取源画面比例，手动设置优先。按设备存储和参数校验见 [DesktopPreferences.cpp](../desktop/app/src/preferences/DesktopPreferences.cpp)。

### 即时码率协议初稿

下文描述当前实现候选。后续版本、会话授权和合并条件见[协议设计与验收](protocol-design/README.md)。

控制请求沿当前会话选中的局域网或 Tailscale 路径发送，复用 GameStream 配对证书和转发租约。Moonlight 校验已配对的 Sunshine 证书，Sunshine 验证配对的客户端证书并将请求限定在该证书持有的唯一活动串流。修订后的端点为 `POST /aurora-video-bitrate-v2`，请求体为空。查询使用 `action=status`，设置使用 `action=set&session=<令牌>&revision=<版本>&kbps=<整数>`。

成功响应为 `status <令牌> <请求码率> <编码器码率> <协商上限> <版本>` 或同格式的 `applied`。码率单位为 kbps。版本从 0 开始，在接受变更时原子递增。Sunshine 同时只处理一个变更。`stale` 表示会话令牌或版本已过期，`busy` 表示已有变更，`expired` 表示到期前未执行，`pending` 表示驱动仍可能完成。收到非确认结果后，主控用状态查询更新当前会话值，只有匹配的 `applied` 结果才保存偏好。

`unsupported` 或经固定证书验证后的旧宿主 404 表示缺少当前版本的即时控制。编码器重建等暂时不可用状态以 `unavailable` 表示。两端继续使用同一套初始协商和实时调整的码率开销计算。

| 环节 | 代码 |
| --- | --- |
| 窗口状态、即时调整与重协商 | Windows 的 [SessionBitrate.cpp](../desktop/app/src/session/SessionBitrate.cpp) 和 [SessionVideo.cpp](../desktop/app/src/session/SessionVideo.cpp)、[macOS 控制中心](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/streaming/aurora-session-toolbar.mm) |
| 复用会话路由、验证工作进程结果 | [ControlAdapter.cpp](../desktop/app/src/adaptors/moonlight/control/ControlAdapter.cpp) |
| HTTPS、配对证书和请求校验 | [Moonlight HTTPS 实现](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvhttp.cpp) |
| 活动会话校验、编码线程和 NVENC 重配置 | [Sunshine RTSP 实现](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/rtsp.cpp) |
| 不重启播放进程、同窗口重协商和回复校验 | [会话测试](../desktop/app/src/session/tests/SupervisorTests.cpp)、[控制测试](../desktop/app/src/adaptors/moonlight/control/tests/ControlTest.cpp) |

## 串流组件裁剪

[Windows 构建](../desktop/packaging/Build-Windows-Streamer.ps1) 与 [macOS 构建](../desktop/packaging/Build-Mac-Streamer.sh) 定义 `AURORA_STREAM_RUNTIME`。该宏限制启动入口，现有目标仍编译原版 QML 资源并部署 Qt Quick 相关文件。

将 Aurora 的启动、连接状态和错误处理接入原生会话入口后，可从运行时构建清单移除原版界面源文件及 `qml.qrc`。拆除启动代码和偏好类的 QML 引用后，移除 Qt Quick/QML 链接项及部署脚本的 `-qmldir` 扫描。依据编译依赖保留网络、证书、SDL、编解码、平台窗口和实际使用的资源。

验收时检查部署依赖，并从 Aurora 正常入口测试已配对连接、首次授权、重连、取消和失败重试。覆盖 Windows 与 macOS，以及局域网和内置 Tailscale。
