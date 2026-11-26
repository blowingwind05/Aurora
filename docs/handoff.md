# Aurora 托孤手册

这份手册写给接手 Aurora 的维护者。Aurora 目前是 Qt Quick 和 C++ 桌面应用，Windows 与 macOS 都可作为主控端和被控端。设备发现、配对、连接及权限入口集中在桌面应用里。近期工作主要集中在桌面可靠性、串流设置、显示器和隐私保护。

## 从哪里开始

1. 从 GitHub 的 `main` 克隆主仓，按[贡献指南](../CONTRIBUTING.md)准备环境。先跑主程序和一条真实串流，再改引擎或拆目录。
2. 用两台自有测试机复现问题，记录主控与被控系统、两端版本、网络路径、操作步骤和结果。做显示器或隐私保护测试时保留能直接操作被控设备的人或途径。
3. 界面以[视觉方案](visual-design/README.md)为准，串流能力以当前实际协商和设备测试结果为准。可以用 AI 修明显的界面问题，也要在实际程序里重走操作。
4. 有空可以顺手完善文档。优先补能复现的测试记录、过时的构建命令、权限入口和已确认的功能边界。稳定说明写在对应文档，临时故障日志放在关联 Issue 或测试记录中。

## 默认基线

交接使用通过 CI 的源码基线 [`ede0849`](https://github.com/blowingwind05/Aurora/commit/ede08495cb5d4ff843a7ad0ef2e803594cac3c64)。Windows x64 与 macOS arm64 都通过原生构建和下载包校验。Mac 的虚拟屏切换已接上两种主控，实际设备间的画面和输入验收仍待完成。

| 主仓所固定的引擎 | 提交 |
| --- | --- |
| Moonlight fork | [`f89e322`](https://github.com/blowingwind05/moonlight-aurora/commit/f89e322c4b7de55fc0aaa3ccca11419e4f8a12eb) |
| Sunshine fork | [`1d1e55d`](https://github.com/blowingwind05/Sunshine-aurora/commit/1d1e55da5ca5a11b9a36584f6d348f3543bf209c) |

- [Windows x64 验收包](https://github.com/blowingwind05/Aurora/actions/runs/45842078269/artifacts/15131436881)
- [macOS arm64 验收包](https://github.com/blowingwind05/Aurora/actions/runs/45842077438/artifacts/15131790433)

## 身份验证和实际连接路线

Aurora 设备间的正常远控连接统一通过 Tailscale 核验账号、设备节点和授权。即使实际串流走局域网或公网 IPv6，也先从已认证的 Tailscale 通道取得可信地址与宿主证书指纹，连接目标再核对证书及设备身份。

画面、声音和操作数据按网络可达性、质量与会话状态自动选路。Aurora 优先使用通过验证且可用的局域网。条件合适时也能用公网 IPv6 直连或 Tailscale 路径，连接过程中可重新评估并切换。 [连接与会话授权](protocol-design/connection.md)

Aurora 的内置 Tailscale 使用独立的 tsnet 节点、登录状态和数据目录。它可以和本机安装的系统 Tailscale 同时在线。Windows 在 Aurora 内切换网络来源时，只改变 Aurora 使用的连接来源，系统客户端和两个节点各自保留的登录状态照常可用。macOS 的 Aurora 使用内置节点，宿主仍可同时运行自己的系统 Tailscale。 [AuroraTailnet](../desktop/tailnet/README.md)

## 目录树

以下展开常用源码、设计和测试目录。生成目录会在本地构建后出现。

```text
Aurora/
├── .github/
│   └── workflows/                 # 两端构建、打包、原生验收和来源证明
├── desktop/                        # 桌面产品、运行时和构建入口
│   ├── Assets/                     # 实际随产品打包的资源
│   │   ├── icons/                  # 产品图标
│   │   └── Wallpapers/             # 默认壁纸和屏保预设素材
│   ├── app/
│   │   ├── qml/                    # 主程序的 Qt Quick 界面
│   │   │   ├── common/             # 主题、公共图标入口、按钮和提示等控件
│   │   │   ├── pages/              # 设备、账户、被控、设置和更多页面
│   │   │   ├── shell/              # 主窗口、侧栏、布局与导航
│   │   │   └── stream/             # 串流顶栏、连接页、控制中心和屏保编辑器
│   │   └── src/                    # Qt/C++ 应用代码
│   │       ├── adaptors/           # 与子程序、底层服务和平台接口的对接
│   │       │   ├── moonlight/      # AuroraStream 的启动、输出和串流控制
│   │       │   ├── sunshine/       # AuroraHost 的配置、管理和进程适配
│   │       │   └── tailscale/      # 应用内节点和系统 Tailscale 的适配
│   │       ├── desktop/            # 桌面应用编排与设备、连接和宿主协调
│   │       ├── discovery/          # 局域网和串流宿主发现
│   │       ├── pairing/            # 配对、授权交换和设备信任
│   │       ├── platform/           # 原生窗口、托盘、单实例、自启动等
│   │       ├── preferences/        # 全局和逐设备偏好的读写
│   │       ├── presence/           # Aurora 设备识别、在线状态和目录
│   │       ├── session/            # 串流生命周期、换路、画质、显示器与隐私控制
│   │       ├── ui/                 # C++ 到 QML 的状态桥和窗口生命周期
│   │       └── wallpaper/          # 壁纸发现、预览和本地缓存
│   ├── runtime/                    # Aurora 自己的运行时入口和宿主辅助代码
│   │   ├── host/                   # AuroraHost 的启动、停止、权限与隐私辅助
│   │   └── stream/                 # AuroraStream 的入口、进程消息和 Qt 串流外壳
│   ├── protocol/                   # 主仓维护的共用协议代码
│   │   ├── bitrate/                # 跨组件的码率请求、响应与宿主状态机
│   │   └── identity/               # 配对及授权所用的身份签名格式
│   ├── tailnet/                    # Go 写的 AuroraTailnet，封装 tsnet 和转发
│   ├── upstream/                   # 引擎子模块、版本来源和按平台准备源码
│   │   ├── moonlight-aurora/       # Moonlight fork，编入 AuroraStream
│   │   └── sunshine-aurora/        # Sunshine fork，编入 AuroraHost
│   ├── packaging/                  # Win/Mac 构建、部署、校验和安装器
│   └── licenses/                   # 随桌面产品分发的许可材料
├── docs/
│   ├── protocol-design/            # 建连、身份、选路、运行时和协议验收记录
│   ├── visual-design/              # 视觉规范、交互设计及桌面/移动端原型
│   │   ├── assets/                 # 原型资源和对照图标
│   │   ├── screenshots/            # 桌面和移动端参考截图
│   │   └── source/                 # 原型共用样式及交互脚本
│   ├── handoff.md                  # 本交接手册
│   ├── signing.md                  # 签名、来源证明和下载验证
│   └── streaming.md                # 串流运行时和会话内设置
└── tests/                          # 跨模块的测试和发布包检查
    ├── qt-ui/                      # 共用 QML 页面、控件与操作回归
    ├── qt-macos-stream-toolbar/    # Qt 外壳接入 Cocoa/SDL 窗口的验收
    ├── qt-more/                    # 「更多」等入口的界面测试
    ├── qt-native-instance/         # 原生实例、启动和退出行为的测试
    ├── runtime-entry/              # 子程序启动与配对 HTTPS 回环
    ├── upstream-bitrate/           # 跨引擎码率协议和状态机测试
    └── windows-privacy-image/      # Windows 隐私屏保素材及渲染检查
```

按模块放置的 C++ 测试通常在各自的 `tests/` 子目录，例如 `desktop/app/src/session/tests/`。构建和上游展开副本通常在 `desktop/obj/`，包在 `dist/`。正式包从 `desktop/Assets/` 和应用构建资源取图标与壁纸，`docs/visual-design/assets/` 用于原型。

## 留给下一位维护者的 12 件事

### 1. 给适配层使用 Aurora 自己的名字

Moonlight 和 Sunshine 已在发布构建中编成静态引擎库，由第一方子程序 `AuroraStream` 和 `AuroraHost` 分别链接。桌面程序的 `adaptors` 层负责启动、控制和解释这两个子程序的结果。运行时入口及引擎名称见[运行时说明](../desktop/runtime/README.md)。

待办是把主仓 `desktop/app/src/adaptors/moonlight/`、`sunshine/` 改成按 Aurora 组件命名的目录，例如 `aurora-stream/`、`aurora-host/`，同步更新内部类型、CMake、测试和文档引用。上游 Git 子模块、引擎来源及许可按原项目记录。UI 和会话层调用 Aurora 的业务接口，上游寻址、控制命令和输出格式的细节留在适配层。

### 2. 注意嵌套子模块和推送顺序

主仓固定 Moonlight 与 Sunshine fork 的提交。Moonlight 还固定 Aurora 的 `moonlight-common-c` fork，Sunshine 使用自己的嵌套依赖，其中 `libvirtualhid` 有 Aurora fork。两份 `moonlight-common-c` 若涉及同一协议，先核对各自所在路径和 pin，再决定哪一份需要改。检出命令、实际参与构建的依赖和源码副本入口见[上游引擎说明](../desktop/upstream/README.md)。

改嵌套依赖时从最深层开始提交并推送。然后在相应引擎 fork 中提交新的子模块指针并推送。最后在主仓提交引擎的新指针，验证全新检出可以取得每一层提交。主仓的 Git 子模块指针固定实际构建版本，`EngineSources.json` 记录基线上游版本。比对时用 `git submodule status --recursive` 和 `git diff --submodule=log`，以发行包的 `SOURCE.txt` 核实产物对应的源码。

### 3. 多测两端实际界面和行为

至少分别走 Windows 控制 Mac 与 Mac 控制 Windows，再用 Mac 对 Mac 和 Windows 对 Windows 补覆盖。平时用真实设备测试首次配对、重连、连接失败恢复、退出和后台驻留。还要试小窗、HiDPI、全屏与退出全屏、控制中心、窗口拖拽、显示器切换、鼠标离开画面及快速重复操作。

遇到白屏、裁切、重影、异常焦点或毫无反馈等问题，先对照[对应视觉原型](visual-design/README.md)，带上截图、复现步骤和机型请 AI 修好实际代码，再重测触发路径及另一平台。原生 Cocoa 夹具已验证顶栏和焦点等操作，真实串流中反复退出全屏仍值得单独复测。

### 4. 先更新视觉原型，再改正式 UI

改布局、交互和文案之前，先在 `docs/visual-design/` 的原型和对应规范里改好。用原型核对大小窗口、平台差异、键盘、状态和减少动态效果，再落到 QML 或平台桥接。修实现与现有稿不一致时，沿用已确认规范即可。上线前核对实际程序和原型，图标从产品资源目录打包。

### 5. Mac 测试新包前重置旧权限项

在有本地操作权的测试 Mac 上退出旧版和旧被控服务。打开「系统设置 → 隐私与安全」，在「辅助功能」和录屏权限列表里移除旧的 Aurora 或 Aurora Host 项，再从固定位置打开新 `Aurora.app`。按新版实际提示授权，然后重启被控组件并确认已出画、鼠标和键盘可用。不同系统版本可能将录屏权限列为「录屏与系统录音」。[Apple 权限说明](https://support.apple.com/zh-cn/guide/mac-help/mchld6aa7d23/mac)

同一台机器反复从不同解压路径运行多个测试包时，尤其容易混淆权限项。排查时核对系统提示里的程序、实际路径和被控组件日志。请在本地操作时重置权限，避免切断当前远控会话。

### 6. 串流时在控制中心改画面设置，当前连接立即生效

目标是在已经连接时打开「控制中心 → 画质」，改分辨率、帧率、码率、编码格式或 HDR 后，当前画面立即应用新设置，面板也随即更新为两端确认的实际值。用户在这次远控里就能看到结果。

目前部分设置能即时生效，例如在协商范围内调整支持热更新的 Windows NVENC 宿主码率。Windows 主控的部分画面设置会在同一个窗口里重连后应用，Mac 主控的一些设置要等下次连接。逐项补齐，验收时直接边串流边改，用实际画面、帧率和编码参数核对。如果某台设备无法实时应用，控制中心应明确告诉用户生效时机。看[串流开发说明](streaming.md)和[码率协议文档](protocol-design/live-bitrate.md)。

### 7. 真机验证 Mac 虚拟显示器和隐私屏保

**Mac 虚拟显示器。** AuroraHost 在会话中使用运行时解析的 CoreGraphics 虚拟显示器接口创建扩展屏，保留会话专用的对象和显示器 ID。顶栏枚举物理屏及本会话的虚拟屏。切换时暂停鼠标，宿主重建目标屏的 AVCapture 采集并发新关键帧。主控呈现新画面后，在有序加密输入通道确认画面世代，宿主再用该屏的 Quartz 坐标开启鼠标。当前可同连接切换的目标受已协商画面比例约束。已通过原生编译、界面和离线协议测试，双机真机测试尚待完成。

**Mac 隐私屏保。** 预览实现用 AppKit 在本地物理屏显示遮罩，同时由 ScreenCaptureKit 在采集时排除该遮罩，使对端仍看到桌面。拓扑、Space 或采集状态无法继续确认覆盖时，中止对应的受保护串流，避免自然回退到可能暴露画面的状态。默认构建关闭此预览功能，在专用测试构建可设 `AURORA_MACOS_PRIVACY_PREVIEW=ON`。现有支持范围为单内建屏、SDR VideoToolbox。计划先验证默认屏保，再验证自定义、全屏、切换 Space、显示拓扑变化与断开恢复。见[宿主说明](../desktop/packaging/Sunshine.md)。

### 8. 继续验证 Windows 隐私屏保的两种实现

- **标准模式。** 当前默认使用宿主自带的普通置顶遮罩。对端从支持排除遮罩的 Windows 采集链路继续看桌面。缺点是遮罩依赖普通桌面的窗口层级与可见性，任务视图、切换虚拟桌面和更高层级的系统窗口可能盖住或移走它，导致本地内容露出。UAC 安全桌面、锁屏、显示器热插拔和异常退出同样需要实测，验收时观察每块物理屏上的实际画面。
- **UIAccess 本机测试。** 走独立 helper，改善本地遮罩窗口层级与持续覆盖。生产分发需要满足 Windows 对 UIAccess 程序的签名和受信任安装位置要求。当前交接基线没有正式发布签名。只在测试机部署自有测试证书签署的 helper，按产品检查路径放在 `%ProgramFiles%\Aurora Developer\AuroraPrivacyOverlay.exe`。独立 helper 还需配对当前的素材契约。[宿主和素材接口](../desktop/runtime/host/PrivacyScreensaver.md) · [微软 UIAccess 要求](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-securityoverview)

开关已经实现，位置是 **Windows「设置 → 通用 → 开发 → 隐私屏保实现」**。选项包括「标准模式」和「UIAccess 本机测试」。已签名测试 helper 缺失时，UIAccess 选项置灰并提示。切换在下一次开启隐私屏保时生效。接手后优先用实际安装的测试 helper 验证此入口、图像预设和恢复操作。独立演示源码和本地签名步骤需从原维护者的 `Aurora-Privacy-Demo` 取得。正式签名的状态与验证入口见[签名说明](signing.md)。

### 9. 完成 Windows 虚拟显示驱动的分发链路

Windows 关屏和部分虚拟显示路径需要系统识别的虚拟输出。候选使用第三方 MttVDD 25.7.23，安装器可在用户有权安装驱动时一起处理，便携包保留显式手动安装路径。当前交接验收包能查找已安装的兼容虚拟输出。驱动和安装器的完整接入另有本地候选，尚未通过正式包链路及干净机验证。接手时先核对候选的源码、锁定版本、许可和驱动签名，再验证安装、升级、卸载及已装驱动时的处理。

### 10. 把所有虚拟显示器过一遍真机测试

- **Windows 被控端。** 分别在未装、已装、不同显示布局下测试发现、创建或启用输出、关闭实体屏、重连和退出恢复。将安装器与便携版的结果分开记录。
- **Mac 被控端。** 用 Windows 和 macOS 主控分别创建、切进、切回、删除虚拟屏。核验新帧、鼠标移动、按住拖拽、点击、滚轮和离开画面时的指针。覆盖 Retina、实体外接屏、静止桌面、横竖比例和断线回收。
- **共同故障场景。** 试屏幕休眠、拔外屏、切换期间撤销授权、进出全屏和异常结束。记录显示 ID、活动源、实际显示布局和鼠标落点。移除当前正在采集的屏需要先切回仍可用的屏。

### 11. 有空再推进 Android 和 iOS 主控

[移动端视觉方案](visual-design/mobile.md)和[交互原型](visual-design/aurora-mobile-preview.html)已准备。开发时尽量延续 Qt，移动端做主控，集成 Moonlight 客户端能力。优先复用应用内 Tailscale 节点，平台的网络权限、后台生命周期和重连行为需要分别做技术验证。首个可用版本先跑通登录、发现、配对、出画、触控和键盘。手机端暂没有可发布客户端。

### 12. 最后再补齐其他远程工具已有的功能

这项的**重要性和紧迫性均最低**。先把已有桌面能力、跨端稳定性与关键验收做扎实，再根据实际需求补尚缺的通用远控功能。新增入口沿用 Aurora 的视觉设计流程。
