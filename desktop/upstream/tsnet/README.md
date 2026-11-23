# 极光内置 Tailscale 节点

`AuroraTailnet` 使用上游 Tailscale 的 `tsnet` 库。极光直接启动它，通过标准输入和输出与它通信。节点首次通过 Tailscale 官方页面加入 tailnet，节点密钥和登录状态保存在当前用户的 `Aurora/Tailnet` 数据目录。退出极光会结束后台进程，退出账号才会清除该节点的登录。

macOS 版极光使用内置节点。Windows 版极光可以选用内置节点或系统 Tailscale，切换只改极光自己的网络选择。向尾网提供的入口用于设备发现、同账号配对和 Aurora Sunshine 串流。Moonlight 通过仅监听本机回环地址的临时端口连接对应节点。Sunshine 管理接口保留在本机。

构建需要 Go 1.27.4、Git 和 PowerShell 7。Windows 目标还需要 GNU `windres` 和它的 C 预处理器，例如 MSYS2 UCRT64 的工具链。用 `-SourceDirectory` 可复用固定版本的 Tailscale Git 仓库，用 `-CachedModuleDownloads` 可复用 Go 的模块下载缓存。

```powershell
pwsh -File ./desktop/upstream/tsnet/Build-Tailnet.ps1 -Runtime win-x64 -OutputDirectory ./desktop/obj/tailnet-win -RunTests
pwsh -File ./desktop/upstream/tsnet/Build-Tailnet.ps1 -Runtime osx-arm64 -OutputDirectory ./desktop/obj/tailnet-mac -RunTests
```

将对应平台的输出目录传给 `desktop/Build.ps1` 或 `desktop/Build-Mac.ps1` 的 `-TailscaleDirectory`。`-RunTests` 适用于在目标平台上运行构建。脚本从固定的 Tailscale v1.104.7 源码构建，并保存依赖许可证和源代码版本。
