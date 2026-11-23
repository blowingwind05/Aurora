# Aurora 的 Sunshine 宿主组件

## Windows

Windows 宿主从 [Sunshine v2026.914.233613](https://github.com/SunshineStream/Sunshine/tree/v2026.914.233613) 的提交 `63d35f702ee9e362e43263742981836ec0710384` 编译。极光复用 Sunshine 的采集、编码、输入及配对实现。仓库中的补丁让极光指定用户配置目录和退出事件，并将 HTTPS 管理端口限制在本机回环地址。

Windows x64 构建使用 [Sunshine 上游的 MSYS2 UCRT64 依赖](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/.github/workflows/ci-windows.yml)、原生 x64 Node.js、Git 和 PowerShell 7。宿主构建关闭 Sunshine 的独立托盘图标，使用极光的托盘界面。

```powershell
pwsh -File ./desktop/upstream/sunshine/Build-Windows-Sunshine.ps1 -Msys2Root C:\msys64
```

脚本会检出固定源码和所需子模块，应用补丁，并输出包含 `AuroraHost.exe`、资源、上游许可及源码记录的目录。将该目录传给 `desktop/Build.ps1 -SunshineDirectory`。如果检出路径较长，可用 `-WorkDirectory` 指定短路径。

极光在「本机被控」启动宿主，配置和配对数据随当前 Windows 用户保存。内置版用 `48989` 作为串流基准端口，和独立安装的 Sunshine 并行运行。退出极光会停止内置宿主。极光用本地公开证书指纹校验 HTTPS 管理连接，管理端口及证书路径等参数在应用内显示为由极光管理的值。

每次启动内置宿主时，极光通过父子进程管道传递本次运行所需的随机管理凭据。Sunshine 使用上游逻辑保存凭据校验值，并在本机 HTTPS 接口继续验证凭据。设置通过极光管理。升级会保留原有 Sunshine 配置、配对和证书，只更新内置宿主私有目录中的管理凭据校验值。

有受信任的虚拟屏时，可在「本机被控」的常用设置里将「远控画面来源」设为虚拟屏，再独立选择保留或关闭本地显示器。新选择默认保留本地显示器。Sunshine 在连接时启用目标屏，最后一个连接结束时恢复原布局。虚拟输出不可用或关屏模式未成功切换时，会拒绝启动串流。极光通过 Windows 识别虚拟输出，也支持预先安装的 Virtual Display Driver 和 SudoVDA。

常用设置和全部配置项会自动保存。手动重启被控组件后，新的配置才会用于之后的连接。

## macOS

macOS 使用同一个上游提交。构建脚本复用 Sunshine 的 macOS 应用打包和动态库部署，安装到 `Aurora.app/Contents/Helpers/AuroraHost.app`。宿主使用自己的应用标识和极光数据目录，与独立安装的 Sunshine 分开。串流基准端口为 `48989`，管理接口仅绑定 `127.0.0.1:48990`，极光验证本地公开证书的 SHA-256 指纹。

按 [上游 macOS 构建流程](https://github.com/SunshineStream/Sunshine/blob/v2026.914.233613/.github/workflows/ci-macos.yml) 准备 Xcode、Git 和构建依赖。Homebrew 依赖为 `cmake node pkgconf icu4c@78 miniupnpc openssl@3 opus`，构建使用的 Python 需要 Jinja2。由 `desktop/Build-Mac.ps1` 调用宿主构建，再与 Moonlight 一起封装。macOS 14.2 及以上可启用被控端。

首次使用在 macOS 系统设置中授予屏幕录制和辅助功能权限，音频和局域网权限按所用功能授权。极光管理宿主的启动与退出，配对先使用 PIN。常用宿主设置可在极光内完成。
