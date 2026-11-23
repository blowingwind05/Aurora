# Windows 界面回归检查

在 Windows 安装 .NET 9 SDK 后，从仓库根目录运行以下命令。报告和截图可保存在 Git 忽略的 `dist/` 目录。

```powershell
dotnet run --project tests/windows-visual/Aurora.WindowsVisual.Tests.csproj -c Release -- dist/visual-check
```

检查使用 Windows 上的 Avalonia 正式样式，并按 100% 和 125% 渲染。输出记录按钮、内容容器、文字布局盒和可见字形的纵向中心，覆盖「登录 Tailscale」「前往登录」「重启 Sunshine」、含下伸字母的英文和动态中英混排，分别检查默认、悬停、禁用和键盘焦点。

被控页用模拟宿主验证宽窗和窄窗、弹窗焦点、取消与 Esc、关闭失败、关闭和重新开启。自启动测试验证首次默认登记以及随后保留关闭选择。这些测试不访问正在运行的 Sunshine 或写入 Windows 启动项。
