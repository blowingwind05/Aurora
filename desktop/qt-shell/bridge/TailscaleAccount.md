# Qt Tailscale 账户页

账户页、运行方式选择、已保存账号菜单、确认对话框和操作反馈由同一 Qt 窗口呈现。Qt 管道连接后首次进入账户页时，C# 创建未挂载到任何窗口的 `TailscalePage` 实例作为动作和状态适配器。该实例复用模式校验、状态读取、运行方式切换、系统账号切换、活动会话确认、恢复连接和退出逻辑。账号页实例随 Qt 管道关闭而释放。登录继续在 C# 校验官方授权 URL 后，只在用户触发登录时交给系统浏览器。

## 管道契约

C# 在原 stdin/stdout 状态快照中携带 `tailscaleAccount`，Qt 用专用的 `sendTailscale(action, target, revision)` 发送 JSON 行。既有设备和设置消息保留其字段。

| 方向 | 字段或操作 | 行为 |
| --- | --- | --- |
| C# 到 Qt | `mode`、`modeLabel`、`supportsEmbedded`、`hasSystemRuntime`、`runtimeNotice` | 当前运行方式和安装能力。系统缺失时内置选项仍可用。 |
| C# 到 Qt | `state`、`status`、`detail`、账号显示字段、`profiles` | 由现有账户页投影。已保存账号带此次快照内的序号键、显示文本和选中标记，用于菜单显示。 |
| C# 到 Qt | `revision`、`busy`、`confirmation`、`progress`、`feedback`、`retryAvailable`、`notice`、`noticeVersion`、`focusVersion` | 用于请求校验、忙碌互锁、按卡片显示反馈、提示和焦点恢复。确认包含一次性 ID、标题、正文、按钮文字和危险标记。 |
| Qt 到 C# | `tailscale.enter` | 启动或复用账户控制器。旧的 `account` 请求也进入此控制器。 |
| Qt 到 C# | `tailscale.primary`、`tailscale.refresh`、`tailscale.retry`、`tailscale.logout` | 对应现有登录／恢复入口、状态刷新、失败重试及退出确认。 |
| Qt 到 C# | `tailscale.mode`、`tailscale.profile` | `accountTarget` 分别携带 `embedded`／`system` 或对应快照内的账号序号键。C# 按当前 `accountRevision` 和账户可用性校验，再调用原有切换流程。 |
| Qt 到 C# | `tailscale.confirm`、`tailscale.cancel` | `accountTarget` 携带确认 ID。C# 只完成当前待确认请求，忽略陈旧或重复 ID。关窗和管道退出取消未完成的确认。 |

快照省略 CLI 账号 ID、Owner ID、授权链接和令牌。切换及退出沿用原有登录与运行方式的有效性检查。活动远控时切换系统账号先等待 Qt 确认，运行方式切换和退出登录始终先确认。Tailscale 状态变化沿原 `OnAccountStatusChanged` 更新侧栏与设备目录。账号操作期间 Qt 的设备连接和手动刷新暂停。

## Windows 隔离回放

在已应用补丁的临时副本根目录，用已配置 MSVC x64 的开发者 PowerShell 构建。将 Qt 前缀替换为本地安装路径。

```powershell
$qtPrefix = 'D:/anaconda3/Library'
$env:Path = "$qtPrefix/bin;$env:Path"
cmake -S desktop/qt-shell -B build/qt-shell -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qtPrefix"
cmake --build build/qt-shell
dotnet build tests/qt-shell-tailscale/Replay.csproj --disable-build-servers
pwsh -NoProfile -NonInteractive -File tests/qt-shell-tailscale/Replay.ps1 -QtExecutable build/qt-shell/AuroraQtShell.exe -Scale 1.25
```

`-Scale` 接受 `1`、`1.25` 和 `1.5`。隔离入口给子进程设置离屏渲染和 Windows 进程级错误模式，并捕获输出。回放入口捕获异常，将详情写入测试输出目录的 `tailscale-replay-error.log` 并返回非零。用 `-FailureProbe` 可重放缺少参数的错误路径，预期返回 1，不启动 Qt。

回放使用真实 C#／Qt 管道和模拟状态，不启动 Aurora 宿主。覆盖刷新、登录与恢复请求、系统客户端缺失、来源切换确认和取消、账号菜单、失败重试、退出确认及登录焦点。正式账号环境的授权、服务切换和退出需要单独验证。Qt 账户切片依赖宿主保留现有 C# Dispatcher，窗口默认选择、发布构建和其他可见页面由上层集成负责。