# 上游引擎

Windows 和 macOS 共用 Aurora 主仓的两个 Git 子模块。主仓里的子模块提交固定引擎版本，[EngineSources.json](EngineSources.json) 记录所基于的上游发行版。第一方的[运行时入口](../runtime/README.md)、[通信协议](../protocol/README.md)和 [AuroraTailnet](../tailnet/README.md)在 Aurora 主仓维护。

| 组件 | 引擎源码 | Aurora 维护的子模块 |
| --- | --- | --- |
| AuroraStream | [moonlight-aurora](moonlight-aurora) | [moonlight-common-c-aurora](https://github.com/blowingwind05/moonlight-common-c-aurora) |
| AuroraHost | [sunshine-aurora](sunshine-aurora) | [libvirtualhid-aurora](https://github.com/blowingwind05/libvirtualhid-aurora) |

在 Aurora 仓库根目录检出两份开发源码。

```sh
git submodule update --init -- desktop/upstream/moonlight-aurora desktop/upstream/sunshine-aurora
```

修改 fork 时在对应子模块中切分支。需要更新 Aurora 的构建版本时，先将引擎提交推送到 fork，再在 Aurora 主仓提交该子模块的新指针。

## 构建副本

[源码准备脚本](Prepare-EngineSource.py)读取主仓的子模块提交，优先从已检出的源码取用该提交。CI 也可以从 fork 获取同一提交。[子模块初始化脚本](Initialize-BuildSubmodules.py)在构建副本里按平台取用所需依赖。Windows Sunshine 和 libvirtualhid 链接一份 lizardbyte-common。Sunshine 在 macOS 上的 Boost.Locale 兼容修正由引擎 fork 持有。[Boost.Locale 接入](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/cmake/dependencies/Boost_Sunshine.cmake)

构建入口和工具链说明在 `packaging`。[Moonlight 构建](../packaging/Streamer.md) · [Sunshine 构建](../packaging/Sunshine.md)

构建副本、生成文件和对象文件放在 `obj`。排查实际编译的代码时，从 `desktop/obj/upstream-sources/<组件>/<平台>/latest-source.txt` 读取副本目录。组件名为 `moonlight` 或 `sunshine`，平台名为 `windows-x64`、`macos-arm64` 或 `macos-x64`。发行包的 `SOURCE.txt` 收录相同的版本和子模块记录。

```powershell
$source = (Get-Content desktop/obj/upstream-sources/moonlight/windows-x64/latest-source.txt -Raw).Trim()
Get-Content (Join-Path $source 'AURORA-SOURCE.txt')
Get-Content (Join-Path $source 'app/backend/nvhttp.cpp')
```

| 功能 | 源码 |
| --- | --- |
| 目标和已配对证书 | [Moonlight 寻址](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/computerseeker.cpp) · [HTTPS](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvhttp.cpp) |
| 配对与活动串流身份 | [Moonlight 配对](https://github.com/blowingwind05/moonlight-aurora/blob/639e79c997beda47126763f3785c14a2ef9a44a6/app/backend/nvpairingmanager.cpp) · [Sunshine GameStream](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/nvhttp.cpp) · [Sunshine RTSP](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/rtsp.cpp) |
| 码率更新 | [主控协议](../protocol/bitrate/ClientProtocol.h) · [被控端协议](../protocol/bitrate/HostControl.h) · [Sunshine NVENC](https://github.com/blowingwind05/Sunshine-aurora/blob/c6a2b19e8cc1ff1fd909971721b8edbea1d985c6/src/nvenc/nvenc_base.cpp) |

协议解析和码率状态机可独立运行测试。

```text
cmake -S tests/upstream-bitrate -B desktop/obj/upstream-bitrate-tests
cmake --build desktop/obj/upstream-bitrate-tests --config Release
ctest --test-dir desktop/obj/upstream-bitrate-tests -C Release --output-on-failure
```
