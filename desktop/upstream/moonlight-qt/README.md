# Aurora 的 Moonlight 串流组件

基线为 [Moonlight Qt v6.0.0](https://github.com/moonlight-stream/moonlight-qt/tree/v6.0.0)，提交 `e7a2d9f44d7b43fe7f6d2e0f08bf55b7cca48818`。在独立的 Moonlight 源码工作目录检出该版本并初始化递归子模块。检查并应用 [后台配对补丁](0001-aurora-background-pair.patch)。

```text
git apply --check <Aurora>/desktop/upstream/moonlight-qt/0001-aurora-background-pair.patch
git apply <Aurora>/desktop/upstream/moonlight-qt/0001-aurora-background-pair.patch
```

Windows 使用 `Build-Windows-Streamer.ps1`，它沿用上游 qmake 项目、MSVC、jom 和 Qt 部署工具。工具链和依赖版本见基线中的 [构建配置](https://github.com/moonlight-stream/moonlight-qt/blob/v6.0.0/appveyor.yml)。

将 Windows 串流部署目录传给 `desktop/Build.ps1 -StreamerDirectory <部署目录>`。串流部署包含 `AuroraStream.exe`、Qt 依赖和许可证。`Build.ps1` 使用同一 Qt SDK 从 `desktop/CMakeLists.txt` 构建 `Aurora` 目标，打包时将桌面 QML 放在发布目录的 `ui/desktop/app/qml/` 下。

macOS 原生构建从 `desktop/Build-Mac.ps1` 开始，它调用 `Build-Mac-Streamer.sh` 并将编译得到的组件安装到 Aurora.app 内部。两个平台共用 `AuroraStreamEntry.h` 和后台配对补丁，组件只接受极光的配对、检查和串流入口。运行时保留上游许可、对应源码版本和本仓库的补丁。编译目录和安装包留在 Git 忽略目录中。
