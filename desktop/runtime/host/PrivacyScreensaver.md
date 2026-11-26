# 宿主隐私屏保

远控中的「控制中心 → 安全 → 隐私保护」展示被控端确认的模式和素材编辑能力。Windows 标准遮罩、Windows UIAccess 本机测试和 macOS 实验遮罩共用素材入口。画面设置、关屏和屏保能力各自以会话响应为准。 [串流运行时](../../../docs/streaming.md) · [隐私屏保交互](../../../docs/visual-design/stream-privacy-screen.md)

## Windows 实现与本机开关

Windows 被控机上，从 Aurora 的「**设置 → 通用 → 开发 → 隐私屏保实现**」选择「标准模式」或「UIAccess 本机测试」。这是被控机本地开关，在下次开启隐私屏保时生效。设置在宿主 profile 内创建或删除 `privacy-uiaccess-test` 标记。界面按 helper 文件是否存在启用测试选项，实际运行时另行检查权限令牌和遮罩就绪状态。 [本机设置](../../app/qml/pages/GeneralSettingsPage.qml) · [偏好存储](../../app/src/preferences/DesktopPreferences.cpp) · [宿主 profile](../../app/src/adaptors/sunshine/host/HostProfile.cpp)

标准模式使用 WGC 和具备 `WDA_EXCLUDEFROMCAPTURE` 的本机遮罩，校验遮罩覆盖和排除能力。普通置顶遮罩会受到任务视图、虚拟桌面切换和更高层级窗口影响，本地桌面可能露出。UAC 安全桌面、锁屏和显示器拓扑变化需分别实测。选择 UIAccess 时，宿主仍要求同一套 WGC 支持，并从 Windows 的 Program Files 已知目录运行 `Aurora Developer\AuroraPrivacyOverlay.exe`。helper 缺失、令牌不合格或就绪检查失败时，本次屏保启动失败，可在被控机恢复标准模式。 [Windows 宿主实现](../../upstream/sunshine-aurora/src/display_device.cpp) · [helper 启动与令牌校验](AuroraHostPrivacyOverlay.h)

### UIAccess 测试件的签署与临时安装

独立开发项目 `Aurora-Privacy-Demo` 提供 `Run-PrivacyDemo.ps1`、`PrivacyDemo.cs` 和 `PrivacyDemo.manifest`。在该项目所在目录、获准使用的 Windows 测试机上运行。脚本使用系统 .NET Framework 的 x64 C# 编译器生成 helper，属于独立开发工具。Aurora 常规构建不打包这个测试 helper。

```powershell
powershell.exe -NoProfile -File .\Run-PrivacyDemo.ps1 -ForAurora -TestMinutes 10
```

部署脚本显示就绪并等待输入时，在第二个 PowerShell 窗口核对签名。

```powershell
Get-AuthenticodeSignature -LiteralPath "$env:ProgramFiles\Aurora Developer\AuroraPrivacyOverlay.exe"
```

- 集成模式使用源文件及带 `asInvoker`、`uiAccess="true"` 的清单。它接受 1–20 分钟的测试窗口，素材由 Aurora 会话提供。`-Standard` 和 `-ImagePath` 属于独立预览，和 `-ForAurora` 互斥。
- 开发脚本在 UAC 授权后校验复制件哈希，将其临时部署到管理员可写的 Program Files 子目录，用仅供本次测试的代码签名证书签署，并检查 Authenticode 为 `Valid`。测试结束或超时清理本次部署、临时信任和签名密钥。退出前先关闭屏保。常规屏保和正式包使用原有安全策略。 [Windows UIAccess 的签名与安全目录要求](https://learn.microsoft.com/windows/client-management/mdm/policy-csp-localpoliciessecurityoptions#useraccountcontrol_onlyelevateuiaccessapplicationsthatareinstalledinsecurelocations)
- 宿主以普通 `open` 启动 helper，要求 `TokenUIAccess=1`、`TokenElevation=0`。会话参数固定为 `--aurora-session <host-pid> <lease-prefix>`，helper 核对活动的 `AuroraHost` 父 PID、同一 Windows 会话及本地 lease。宿主等待 `.ready`，通过 `.lost` 监测覆盖丢失，退出时发 `.stop`。任意外部素材路径和管理员运行均不属于该会话契约。 [宿主契约](AuroraHostPrivacyOverlay.h)

## macOS 实验遮罩

macOS 的宿主构建默认 `AURORA_MACOS_PRIVACY_PREVIEW=OFF`。明确设置环境变量 `AURORA_MACOS_PRIVACY_PREVIEW=ON` 后重建宿主，测试 profile 需启用 `aurora_privacy_screensaver = enabled`，并具有屏幕录制权限。当前预览接受已配对的 SDR 4:2:0 VideoToolbox 串流，以及单块活动、未镜像、未睡眠的内建屏。虚拟扩展屏与该遮罩预览分开验收。 [预览构建](../../packaging/Sunshine.md) · [Mac 遮罩实现](../../upstream/sunshine-aurora/src/platform/macos/aurora_privacy.mm)

宿主先用不透明 AppKit 窗口盖满物理屏。候选宿主在公开最大窗口层级检查遮罩 ID、进程、层级、全屏几何和遮挡。同层窗口遮挡时，尝试将同一窗口重新置顶，再检查覆盖。ScreenCaptureKit 通过 `SCContentFilter(display:excludingWindows:)` 排除该窗口 ID，确认首帧后才进入受保护采集。确认到期、覆盖丢失、切换 Space、会话失活、休眠或显示拓扑变化会结束受保护流。 [Apple 的窗口排除接口](https://developer.apple.com/documentation/screencapturekit/sccontentfilter/init%28display%3Aexcludingwindows%3A%29)

构建、离屏夹具和包内资源检查可分别执行。两机实测仍待完成，覆盖物理屏遮挡、控制端画面、同层窗口、Space、断开及恢复。

## 素材和降级

宿主从 `platf::aurora_profile()/privacy-screensaver.png` 读取已保存素材。主控编辑器和配对素材接口使用规范化的 8 位不透明 RGB PNG，上传和已存文件的接口上限为 8 MiB，单边 4096 像素，总计最多 8,388,608 像素。Windows 本地 WIC 解码器另设 64 MiB、单边 8192 像素和 16,777,216 总像素的防护上限，逐像素检查不透明度。macOS 用 ImageIO 后台解码无 Alpha 通道的单帧 PNG，再把图像安装到仍属本次会话的原遮罩窗口。三条路径都居中等比裁切铺满，素材缺失或解码失败时保留原遮罩。 [素材接口](../../upstream/sunshine-aurora/src/aurora_privacy_appearance.h) · [Windows WIC 实现](AuroraHostPrivacyImage.h) · [Mac ImageIO 实现](../../upstream/sunshine-aurora/src/platform/macos/aurora_privacy_artwork.mm)

Windows 测试 helper 从同一会话的 `<lease-prefix>.image` 以只读方式取素材。头部四个小端 32 位整数依次为 `0x4D495041`（APIM）、版本 `1`、宽和高。其后为自上而下紧密排列的 B、G、R、255 数据。宿主持有映射直至 helper 退出。缺失或无效的映射使用默认遮罩。旧 helper 沿用会话参数并绘制默认遮罩，自定义素材需要支持该映射格式的签名开发件。 [像素交接实现](AuroraHostPrivacyImage.h)

## Windows 离线验证

在 Windows x64 开发环境中运行。夹具覆盖 WIC 解码、透明度和尺寸检查、内存 GDI 绘制及共享映射格式。签名 helper 的安装、实际令牌和双端画面需另外在测试机验收。

```powershell
cmake -S tests/windows-privacy-image -B desktop/obj/privacy-image-tests
cmake --build desktop/obj/privacy-image-tests --config Release
ctest --test-dir desktop/obj/privacy-image-tests -C Release --output-on-failure
```
