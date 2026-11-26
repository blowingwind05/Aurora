# macOS 串流顶栏验证

在有图形会话的 Mac 上，应用主仓和 Moonlight 子仓补丁后，用与正式包相同的 Qt 和子仓 SDL framework 构建测试。测试打开独立的 SDL/Metal 窗口，通过进程内的 Cocoa 事件检查顶栏点击、控制中心、键盘焦点和原生回退。

```sh
cmake -S tests/qt-macos-stream-toolbar -B /tmp/aurora-toolbar-test -DCMAKE_PREFIX_PATH=/path/to/Qt/6.6.3/macos
cmake --build /tmp/aurora-toolbar-test --config Release
ctest --test-dir /tmp/aurora-toolbar-test -C Release --output-on-failure
```

窗口化串流中，释放远端光标后可按 F6 聚焦顶栏。点击视频区将焦点交回 SDL。设置 `AURORA_QT_SESSION_TOOLBAR=0` 可使用 AppKit 顶栏，控制中心仍可通过其原有入口打开。

## 隐私屏保样式

Cocoa fixture 通过 AppKit 安全页进入共用 Qt 编辑器，用只返回默认样式的本地替身检验辅助面板、键鼠归属以及关闭后的安全页。此测试需要主仓中的共用编辑器和 `AuroraPrivacyArtwork.h`。

用已配对的实际包验收文件选择和全屏 Space 时，分别从窗口化和全屏串流的安全页打开编辑器。确认文件面板附着于编辑器，选择和取消后回到编辑器，底层串流收不到此处的键鼠输入。保存后，下次启用隐私屏保应使用新样式。关闭编辑器应回到安全页，断开串流应关闭编辑器和仍打开的文件面板。
