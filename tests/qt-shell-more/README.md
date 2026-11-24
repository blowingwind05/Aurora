# Qt 主窗「更多」离线测试

测试使用内存网络响应和记录 URL 的替身启动器。实际的 Live.qml 在离屏平台使用软件渲染，Windows 测试进程禁用错误弹窗。

使用 Qt 5.15 或 Qt 6.6 的 Quick、Quick Controls 2、Network 和 Test 开发组件。从仓库根目录运行以下命令。Windows 使用 x64 开发者 PowerShell，并将所选 Qt 的 bin 目录加入 PATH。

~~~powershell
cmake -S tests/qt-shell-more -B build/qt-shell-more "-DCMAKE_PREFIX_PATH=$qtPrefix"
cmake --build build/qt-shell-more --config Release
ctest --test-dir build/qt-shell-more -C Release --output-on-failure
~~~

测试覆盖九个固定目标、未知动作、打开失败文案、GitHub 请求策略、版本比较、重复点击、HTTP 与解析错误、两个响应上限，以及菜单的转圈、关闭和提示消失。
