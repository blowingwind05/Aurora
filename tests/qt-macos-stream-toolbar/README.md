# macOS 串流顶栏验证

在有图形会话的 Mac 上，应用主仓和 Moonlight 子仓补丁后，用与正式包相同的 Qt 和子仓 SDL framework 构建测试。测试打开独立的 SDL/Metal 窗口，通过进程内的 Cocoa 事件检查顶栏点击、控制中心、键盘焦点和原生回退。

```sh
cmake -S tests/qt-macos-stream-toolbar -B /tmp/aurora-toolbar-test -DCMAKE_PREFIX_PATH=/path/to/Qt/6.6.3/macos
cmake --build /tmp/aurora-toolbar-test --config Release
ctest --test-dir /tmp/aurora-toolbar-test -C Release --output-on-failure
```

窗口化串流中，释放远端光标后可按 F6 聚焦顶栏。点击视频区将焦点交回 SDL。设置 `AURORA_QT_SESSION_TOOLBAR=0` 可使用 AppKit 顶栏，控制中心仍可通过其原有入口打开。
