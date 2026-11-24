# Windows Qt 会话浮层试验

Windows 实验会话由 Avalonia 持有顶层窗口，Moonlight 用 SDL 把画面嵌在 `WindowsStreamSurface` 的子 HWND 中。Qt 控制中心和连接详情通过独立的 Win32 owned tool window 盖在同一个会话窗上，视频的父窗口、渲染与尺寸处理继续走现有路径。浮层仅占菜单矩形范围。窗口移动、缩放或结束会话时关闭浮层，重新打开时按当前坐标定位。

接线复用实验主界面子进程的 JSON 行传输。只有 Windows 下启用 `AURORA_QT_SESSION=1` 且通过现有 `--qt-shell` 参数配置了 Qt Shell 和 `Live.qml` 时才开启。新增的 `WindowsSessionPopup.qml` 与 `Live.qml` 同目录。在已连接的窗口中打开控制中心或连接状态。若 Qt 浮层无法启动或未完成窗口挂载，当前入口回退到原生弹层。

- 「全屏」发出带布尔目标值的请求。宿主调用既有窗口动作，并以真实的 `WindowState` 为准显示「全屏」或「退出全屏」。窗口化时可从顶栏触发，全屏时继续从既有的悬浮按钮触发。
- 连接详情接收 Windows 会话已有的往返延迟及网络质量标志。这一小步仅显示实时往返延迟。会话刚就绪但缺少采样时显示「暂无连接参数」。
- 「更多串流设置」打开同一会话已有的原生菜单，沿用其中的画质、声音、输入和隐私处理。

## 离线重放

在已应用补丁的临时副本和 Windows x64 开发者 PowerShell 运行。将 Qt 路径设为已安装的 Qt 5.15，将 `$sdlDll` 指向现有构建附带的 x64 `SDL2.dll`。使用已缓存的还原包时传 `-OfflinePackages`。

```powershell
$qtPrefix = 'D:/anaconda3/Library'
$sdlDll = 'C:/temp/Aurora/Streamer/SDL2.dll'
cmake -S desktop/qt-shell -B build/qt-shell -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qtPrefix"
cmake --build build/qt-shell
pwsh -NoProfile -File ./tests/qt-windows-session/Run.ps1 -QtShell ./build/qt-shell/AuroraQtShell.exe -QtBin "$qtPrefix/bin" -SdlDll $sdlDll
```

重放启动单独的 Avalonia 测试应用，使用实际的 `StreamSessionWindow.Run`。离线 SDL 子进程通过同样的 HWND 邀请和 ready/stop 事件接入，连续绘制测试帧，再从 stdout 送入两份连接采样。测试检查浮层的 Win32 owner、与视频区域重叠、Z 顺序、两次在 Qt 中呈现的延迟，以及从 Qt 按钮发起全屏后同一个 SDL HWND 仍在出画。测试输入只投递到重放自己的窗口句柄。

测试入口捕获异常，将诊断写入标准错误并返回非零。运行脚本还为本次重放的失败设置独立日志，位于 `obj/native-session-failures-*.log`。
