# Aurora 的 Moonlight 串流组件

基线为 [Moonlight Qt v6.0.0](https://github.com/moonlight-stream/moonlight-qt/tree/v6.0.0)，提交 `e7a2d9f44d7b43fe7f6d2e0f08bf55b7cca48818`。发布构建检出 Aurora 的锁定 fork 提交，再复制第一方运行时源码。可从[上游源码阅读入口](../upstream/README.md)打开源码，或直接查看[码率协议解析](../protocol/bitrate/ClientProtocol.h)。

Windows 使用 `Build-Windows-Streamer.ps1`，它沿用上游 qmake 项目、MSVC、jom 和 Qt 部署工具。工具链和依赖版本见基线中的 [构建配置](https://github.com/moonlight-stream/moonlight-qt/blob/v6.0.0/appveyor.yml)。

将 Windows 串流部署目录传给 `desktop/packaging/Build-Windows.ps1 -StreamerDirectory <部署目录>`。串流部署包含 `AuroraStream.exe`、Qt 依赖和许可证。`Build-Windows.ps1` 使用同一 Qt SDK 从 `desktop/CMakeLists.txt` 构建 `Aurora` 目标，打包时将桌面 QML 放在发布目录的 `ui/desktop/app/qml/` 下。

macOS 原生构建从 `desktop/packaging/Build-Mac.ps1` 开始，它调用 `Build-Mac-Streamer.sh` 并将编译得到的组件安装到 Aurora.app 内部。两个平台编译同一份 [AuroraStream 入口](../runtime/README.md)，由它选择命令并调用 Moonlight 的配对、检查和串流实现。运行时保留上游许可、对应的 fork 提交和第一方源码。编译目录和安装包留在 Git 忽略目录中。
