# 极光的串流与被控子程序

| 子程序 | Aurora 所有的入口 | 复用的引擎 |
| --- | --- | --- |
| AuroraStream | [进程入口](stream/AuroraStreamMain.cpp)读取内部启动命令和 relay 参数，处理运行时探测，将配对、查询、播放和控制请求分派给 Moonlight。 | [Moonlight 适配](https://github.com/blowingwind05/moonlight-aurora/blob/f89e322c4b7de55fc0aaa3ccca11419e4f8a12eb/app/main.cpp)保留 Qt 与 SDL 初始化、逐命令参数处理、身份和配对存储、RTSP 以及 `Session`。 |
| AuroraHost | [进程入口](host/AuroraHostMain.cpp)和[启动处理](host/AuroraHostBootstrap.cpp)通过父进程管道读取管理凭据，在 Windows 等待停止事件，在 macOS 监听父进程管道关闭。 | [Sunshine 适配](https://github.com/blowingwind05/Sunshine-aurora/blob/1d1e55da5ca5a11b9a36584f6d348f3543bf209c/src/main.cpp)读取现有 profile、设置本地管理凭据，并继续执行管理、采集、编码和输入服务。 |

两条发布构建沿用各引擎的依赖、编译与打包配置。Moonlight 编译为 `AuroraMoonlightEngine` 静态库，Sunshine 编译为 `aurora_sunshine_engine` 静态库，分别链接到第一方入口。上游 `main()` 不进入内置目标，Qt 资源和平台包元数据继续由可执行文件携带。构建后对两个二进制执行 `--aurora-runtime-info`，发布包携带本目录用到的源码和对应上游许可。

设备身份、路由、会话选择和子进程生命周期由 Aurora 桌面程序协调。现有设备配对仍使用 Moonlight 和 Sunshine 的 GameStream 证书交换。串流走局域网、系统 Tailscale 或内置 relay 时，共用同一入口与信任策略。
