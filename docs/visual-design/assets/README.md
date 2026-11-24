# 视觉素材

## 主图标

全系列的 A 采用 **1∶1 的可见轮廓**。白底、透明、Windows、macOS 应用前景及菜单栏单色版共用该比例，画布留白按用途处理。

- [透明矢量原稿](brand/Aurora-transparent.svg) 和 [透明 512 px PNG](brand/Aurora-transparent-512.png) 可用于窗口与界面展示。
- [白底展示稿](brand/Aurora-white.svg) 用于品牌素材展示。
- Windows 使用[多尺寸 ICO](brand/Aurora-Windows.ico)，任务栏通知区域复用其中的小尺寸帧。附[对应矢量稿](brand/Aurora-Windows.svg)和[任务栏尺寸预览](brand/Windows-taskbar-size-preview.png)。
- macOS 程序坞采用 648 × 648 px 的 A 可见轮廓，约占现有 840 px 白底宽度的 77%。沿用青蓝定稿的等比轮廓。使用 [ICNS](brand/Aurora-mac-Dock-77.icns)作为白底图标，或将 1024 px 画布的[透明前景 PNG](brand/Aurora-mac-foreground-648.png)和独立白底导入 Icon Composer。[等比矢量源稿](brand/Aurora-mac-foreground-648.svg) · [三版尺寸对比](../screenshots/components/macos-dock-icon-spacing-77.png) · [对角留白对比](../screenshots/components/macos-dock-diagonal-comparison.png) · [Apple 图标分层指南](https://developer.apple.com/design/human-interface-guidelines/app-icons/)
- macOS 菜单栏使用 [AuroraMenuBarTemplate.imageset](brand/macOS-MenuBar/AuroraMenuBarTemplate.imageset/Contents.json)。把整个 imageset 加入 Assets.xcassets。附[矢量源稿](brand/macOS-MenuBar/AuroraMenuBarTemplate.svg)和[明暗背景预览](brand/macOS-MenuBar/preview.png)。

## 界面图标

[界面图标清单](icons/README.md)列出每个位置应取用的 SVG、品牌遮罩、图标来源及许可。

## 演示头像

[Tailscale 账户插画头像](demo/tailscale-avatar.webp)由图像生成工具创作，演示对象为虚构人物。

## 设备壁纸

默认壁纸库使用用户上传的八张图。设备没有缓存预览时使用默认图，有缓存时优先显示远端壁纸。先选晴空山谷作为首张。它是 16∶9，画面主体在右侧，设备名和地址叠在左下角。原始 JPG 逐张保存在[原图目录](wallpapers/default/originals/)，显示用 WebP 沿用绘图内容和原有署名。

| 顺序 | 本地显示素材 | 用户提供的原图 | 原型取景 |
| --- | --- | --- | --- |
| 1 | [晴空山谷](wallpapers/default/mountain-valley.webp) | `20261123-144500.jpg`（图七） | 首选默认图、书房主机和串流主屏 |
| 2 | [林间小径](wallpapers/default/forest-path.webp) | `20261123-144526.jpg`（图三） | 工作室电脑。卡片取景略向下，保留林间人物和小路 |
| 3 | [花树下](wallpapers/default/blossom-grove.webp) | `20261123-144514.jpg`（图五） | 随身笔电。显示素材取 `x=0, y=45, w=1024, h=634` |
| 4 | [银杏湖畔](wallpapers/default/ginkgo-lakeside.webp) | `20261123-144454.jpg`（图六） | 客厅电脑。取景略向右下，保留长椅和人物 |
| 5 | [冰湖钢琴](wallpapers/default/baikal-piano.webp) | `20261123-144508.jpg`（图四） | 串流虚拟屏示例 |
| 6 | [冰湖手风琴](wallpapers/default/baikal-accordion.webp) | `20261123-144504.jpg`（图八） | 默认壁纸库 |
| 7 | [向日葵花田](wallpapers/default/sunflower-field.webp) | `20261123-144531.jpg`（图一） | 默认壁纸库 |
| 8 | [逆光云海](wallpapers/default/sunset-sky.webp) | `20261123-144523.jpg`（图二） | 默认壁纸库 |

图五的原图为 1024 × 724 px，上下黑边各 45 px。显示时先取中间 1024 × 634 px 画面，再按卡片比例铺满。其他七张显示素材保留完整画幅。设备卡片原型嵌入前四张，可离线查看。[八张卡片取景对比](../screenshots/components/device-default-wallpapers.png)
