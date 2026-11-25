# Aurora 的 Sunshine 宿主组件

极光会保存「允许本机被控」的选择，并在下次启动时尝试恢复内置宿主。退出极光时，当前被控进程和远控连接随之结束。

Windows 和 macOS 共用 [AuroraHost 入口](../runtime/README.md)，由它读取启动凭据和监听停止信号，再调用 Sunshine 的托管运行接口。

可从[上游源码阅读入口](../upstream/README.md)查看各平台的完整源码和固定提交。Windows 宿主的[码率状态机](../protocol/bitrate/HostControl.h)在 Aurora 仓库内维护，由构建送入 Sunshine 源码树。

## Windows

Windows 宿主从 [Sunshine v2026.914.233613](https://github.com/SunshineStream/Sunshine/tree/v2026.914.233613) 的 Aurora fork 锁定提交编译。极光复用 Sunshine 的采集、编码、输入及配对实现。托管入口由极光指定用户配置目录和退出事件，并将 HTTPS 管理端口限制在本机回环地址。

Windows x64 构建使用 [Sunshine 上游的 MSYS2 UCRT64 依赖](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/.github/workflows/ci-windows.yml)、Git 和 PowerShell 7。宿主编译和打包只包含采集所需的资源与极光调用的本机管理接口；托盘和设置界面由极光提供。

```powershell
pwsh -File ./desktop/packaging/Build-Windows-Sunshine.ps1 -Msys2Root C:\msys64
```

脚本检出固定 fork 提交和所需子模块，并输出包含 `AuroraHost.exe`、资源、上游许可及源码记录的目录。将该目录传给 `desktop/packaging/Build-Windows.ps1 -SunshineDirectory`。如果检出路径较长，可用 `-WorkDirectory` 指定短路径。

极光在「本机被控」启动宿主，配置和配对数据随当前 Windows 用户保存。内置版用 `48989` 作为串流基准端口，和独立安装的 Sunshine 并行运行。退出极光会停止内置宿主。极光用本地公开证书指纹校验 HTTPS 管理连接，管理端口及证书路径等参数在应用内显示为由极光管理的值。

每次启动内置宿主时，极光通过父子进程管道传递本次运行所需的随机管理凭据。Sunshine 使用上游逻辑保存凭据校验值，并在本机 HTTPS 接口继续验证凭据。设置通过极光管理。升级会保留原有 Sunshine 配置、配对和证书，只更新内置宿主私有目录中的管理凭据校验值。

远控 Windows 时，在主控端的「控制中心 → 安全 → 隐私保护」选择「关闭」「关闭显示器」或「隐私屏保」。Aurora 会自动选用 Windows 已识别的兼容虚拟输出，切换后验证仅该输出启用。选择「关闭」或结束会话时恢复原显示布局。没有可用虚拟输出时，界面会提示关屏未生效。

隐私屏保使用支持排除遮罩的采集方式，遮罩随会话结束移除。以旧版宿主配置预先启用的隐私设置仍按原配置生效。

常用设置和全部配置项会自动保存。手动重启被控组件后，新的配置才会用于之后的连接。

## macOS

macOS 使用同一个上游提交。构建脚本复用 Sunshine 的 macOS 应用打包和动态库部署，安装到 `Aurora.app/Contents/Helpers/AuroraHost.app`。宿主使用自己的应用标识和极光数据目录，与独立安装的 Sunshine 分开。串流基准端口为 `48989`，管理接口仅绑定 `127.0.0.1:48990`，极光验证本地公开证书的 SHA-256 指纹。

按 [上游 macOS 构建流程](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/.github/workflows/ci-macos.yml) 准备 Xcode、Git 和构建依赖。Homebrew 依赖为 `cmake pkgconf icu4c@78 openssl@3 opus`，构建使用的 Python 需要 Jinja2。由 `desktop/packaging/Build-Mac.ps1` 调用宿主构建，再与 Moonlight 一起封装。macOS 14.2 及以上可启用被控端。

首次使用在 macOS 系统设置中授予屏幕录制和辅助功能权限，音频和局域网权限按所用功能授权。极光管理宿主的启动与退出。使用内置 Tailscale 时，同账号设备可以自动配对。也可以在本机被控页面确认手动配对的 PIN。

隐私屏保的 macOS 实现使用 ScreenCaptureKit 从远控画面中排除 AppKit 遮罩。预览构建通过 `AURORA_MACOS_PRIVACY_PREVIEW=ON` 启用，仅接受单内建屏和 SDR VideoToolbox 串流。正式构建默认关闭预览，仍待 Mac 实机验证。
