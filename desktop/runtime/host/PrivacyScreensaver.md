# Windows 隐私屏保素材

宿主每次启用屏保时，从 `platf::aurora_profile()` 返回的目录读取固定文件 `privacy-screensaver.png`，并在本次遮罩会话中使用这份快照。WIC 解码 PNG，宿主逐像素验证不透明度。文件上限为 64 MiB，每边最多 8192 像素，总计最多 16,777,216 像素。大小相容的 RGB PNG 和每个像素都不透明的 RGBA PNG 均可使用。标准遮罩保留原有的捕获排除和窗口覆盖检查。

宿主通过现有的 `--aurora-session <host-pid> <lease-prefix>` 启动已安装的测试 helper。有可用素材时，宿主在同一会话的 `<lease-prefix>.image` 映射中写入一次像素并持有它，直至 helper 退出。前 16 字节是四个小端 32 位整数，依次为 `0x4D495041`（APIM）、版本 `1`、宽和高。其后为从上到下紧密排列的 B、G、R、255 字节，每行宽度为图像宽度乘四。helper 在现有的令牌、父进程和会话检查后以只读方式打开映射，从自己的位图副本绘制。lease 的 ready、stop 和 lost 事件保持原契约。

两种遮罩都以居中裁切的方式等比铺满。素材缺失、不可读取、含透明像素、解码失败，或 helper 遇到缺失和无效的映射时，对应实现继续使用原有遮罩。旧 helper 可以接受原会话参数，并继续绘制自己的默认遮罩。要显示自定义素材，需要通过原有签名及安装流程部署采用本契约的开发 helper。

主仓的 `tests/windows-privacy-image` 可以独立用 Windows CMake 和 CTest 重放 WIC 解码、内存 GDI 绘制和映射格式。
