# 早期 Windows 服务版 Tailscale

此目录保留旧版后台服务的构建和恢复资料。当前 Aurora 集成包使用[应用内节点](../tsnet)。

Windows 版使用 [Tailscale v1.104.7](https://github.com/tailscale/tailscale/tree/v1.104.7) 的提交 `cbc74fe48ef1b1649f69a4d9a602319427e25102`。极光用上游的 Windows 服务、命令行和签名版 [Wintun 0.14.1](https://www.wintun.net/) 建立系统路由，供 Moonlight 和 Sunshine 的 TCP、UDP 串流使用。

构建需 Windows x64、Git、PowerShell 7、[Go 1.27.4](https://go.dev/dl/) 和 Wintun 上游 ZIP。脚本验证版本、修订和驱动哈希，应用本目录补丁并生成包含源码记录及许可的部署目录。

```powershell
git clone --depth 1 --branch v1.104.7 https://github.com/tailscale/tailscale.git <源码目录>
pwsh -File ./desktop/upstream/tailscale/Build-Windows-Tailscale.ps1 -SourceDirectory <源码目录> -GoExecutable <go.exe路径> -WintunZip <wintun-0.14.1.zip路径> -OutputDirectory <部署目录>
```

将部署目录传给 `desktop/Build.ps1 -TailscaleDirectory`。Windows 版优先使用已安装的系统 Tailscale。未安装系统 Tailscale 的电脑可以启用内置服务。两者各自保留原有设备身份和登录状态。源码集成版只部署后台服务及供极光调用的命令行，无独立客户端界面或新增托盘图标。macOS 目前复用系统 Tailscale。

## Windows 切换接口

桌面端先校验打包记录中的 SHA-256，并持有已验证的安装程序及其父目录句柄，再用 `runas` 执行 `AuroraTailnetSetup.exe embedded` 或 `AuroraTailnetSetup.exe system`。程序只接受这两个参数之一，并验证进程已获管理员权限。查询状态和使用 LocalAPI 无需为切换操作预先提升权限。

| 模式 | Windows 服务 | 命名管道（不含 `\\.\pipe\` 前缀） |
| --- | --- | --- |
| `embedded` | `AuroraTailscale` | `ProtectedPrefix\Administrators\AuroraTailscale\tailscaled` |
| `system` | `Tailscale` | `ProtectedPrefix\Administrators\Tailscale\tailscaled` |

启用内置模式时，已注册的系统 Tailscale 会阻止切换，即使它处于停止或禁用状态。切回系统模式时，程序先停用内置服务，再等待系统服务及其 LocalAPI 就绪。若旧版曾禁用系统服务，程序可依据受保护的 `%ProgramFiles%\AuroraTailnet\SwitchState\stock-startup.json` 恢复原来的启动方式，前提是记录与已验证的系统服务程序匹配。管理员预先禁用且没有匹配记录的服务保持禁用。设备凭据仍留在各自原有的存储目录。

旧版内置服务若占用 `Tailscale` 名称，程序校验确切的旧路径、文件权限及二进制哈希，并使用 `AuroraTailscale` 注册内置组件。切换完成后删除已验证的旧 SCM 注册。选择系统模式且未安装官方服务时，内置服务停止并保持禁用，桌面端可展示安装指引。切换失败时，程序尝试恢复原来的启动模式和运行状态。

| 退出码 | 结果 |
| --- | --- |
| 0 | 选中的服务及其 LocalAPI 已就绪 |
| 1 | 命令参数无效 |
| 2 | 已注册服务无法验证 |
| 3 | 目录、运行文件或启动模式记录无法安全使用 |
| 4 | 操作失败，或系统服务已被管理员禁用 |
| 5 | 另一项切换正在运行，或 SCM 正在删除旧注册 |
| 6 | 已选择系统模式，尚未安装系统 Tailscale |
| 7 | 恢复原状失败，需要检查服务状态 |
| 8 | 已注册系统 Tailscale，内置模式未启用 |

`-RunSetupTests` 用模拟的 SCM 服务验证切换与回退。可将现有模块下载缓存传给 `-CachedModuleDownloads` 进行离线构建，仍由 Go 校验固定模块的校验和。
