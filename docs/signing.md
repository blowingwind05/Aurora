# 签名与下载验证

## 桌面构建

Windows 安装器和便携 ZIP、macOS ZIP 都附带同名的 `.sha256` 文件。Windows 从源码构建的便携目录另有一份 `SHA256SUMS.txt`，包含目录内每个文件的摘要。摘要可用于确认下载或复制后的文件与发布的版本一致。

从 GitHub Actions 下载时，先解开 Actions 的外层归档，再校验其中的 Aurora 安装器或 ZIP。公开仓库的 Windows 和 macOS 构建流程为这些文件出具来源证明，可用已登录的 GitHub CLI 核验。来源证明绑定仓库、构建流程、提交和文件摘要。Windows 的 Authenticode 和 Apple 的公证是独立的系统信任机制。[GitHub 构建来源证明](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations)

```sh
gh attestation verify AuroraSetup.exe --repo blowingwind05/Aurora
gh attestation verify Aurora-macOS-arm64-integrated.zip --repo blowingwind05/Aurora
```

本地编译或离线传输可用仓库里的脚本验证 SHA-256。保留原始文件名，将校验文件放在同一目录。便携目录把 `-Path` 指向解压后的目录。

```powershell
pwsh -File ./desktop/Artifact-Checksums.ps1 -Path ./AuroraSetup.exe -Verify
```

```sh
shasum -a 256 -c Aurora-macOS-arm64-integrated.zip.sha256
codesign --verify --deep --strict --verbose=2 Aurora.app
```

### Windows

当前安装器与 Aurora 自身的程序使用 SHA-256 和构建来源证明验证。ViGEmBus 沿用原厂签名的安装器，安装前会核对固定的摘要。Aurora 的 Windows 测试包仍可能显示 SmartScreen 提示。

正式的 Authenticode 可申请免费的 SignPath Foundation 开源签名。它要求项目已有可验证的发布和一定信誉，并对修改后的上游二进制有单独条件。申请通过后再接入签名流水线，签过的包重新生成最终摘要和来源证明。Microsoft Store 的 MSIX 路线可由商店免费签名，适合评估主控版。系统驱动的部署需单独设计。[SignPath 条件](https://signpath.org/terms) · [微软签名选项](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) · [MSIX 打包限制](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-prepare)

### macOS

测试包使用免费 ad-hoc 签名，并在归档后解压核验。直接公开分发时，要获得 Apple 的发布者信任需 Developer ID 和公证。Apple Developer Program 通常是每年 99 美元。符合条件的学校可以作为机构申请免年费，个人学生身份本身不属于免年费范围。[Apple Developer ID](https://developer.apple.com/help/account/certificates/create-developer-id-certificates) · [Apple 年费减免](https://developer.apple.com/help/account/membership/fee-waivers)

## 未来的移动端

- **Android**。给发布版 APK 使用自己的长期签名密钥，备份密钥和密码，并把签名证书的 SHA-256 指纹与 APK 一起公布。构建时使用 `apksigner verify --print-certs` 核验签名。签 APK 本身免费。小范围分发可用 Android Developer Console 的免费限量账户，最多 20 台授权设备。广泛分发再按目标地区和渠道确认开发者验证规则。Google Play Console 注册费目前是一次性 25 美元。[APK 签名工具](https://developer.android.com/tools/apksigner) · [免费限量分发](https://developer.android.com/developer-verification/guides/limited-distribution) · [Play Console 费用](https://support.google.com/googleplay/android-developer/answer/6112435)
- **iOS**。自己的设备先用免费 Apple 账号和 Xcode Personal Team 测试，配置文件通常 7 天到期，需要重新安装。TestFlight 和 App Store 分发使用 Apple Developer Program。同一份会员可用于 iOS 分发与 macOS Developer ID。[Personal Team](https://developer.apple.com/help/account/basics/about-your-developer-account) · [Apple 开发者计划](https://developer.apple.com/help/account/membership/programs-overview/)

签名私钥、密码和配置文件放在仓库外。Android 的正式签名密钥在首次对外发布时创建并备份，后续版本沿用。iOS 和 Android 的构建流程等客户端源码落地后接入。
