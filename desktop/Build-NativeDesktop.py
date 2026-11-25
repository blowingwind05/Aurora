#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Assemble a Qt-native Aurora desktop from built components."""

import argparse
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time


DESKTOP = Path(__file__).resolve().parent
REPOSITORY = DESKTOP.parent
WIN_STREAM = "Components/Moonlight"
WIN_HOST = "Components/Sunshine"
WIN_TAILNET = "Components/Tailscale"


def require_file(path):
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def require_directory(path):
    if not path.is_dir():
        raise NotADirectoryError(path)
    return path


def pe_executable(path, windowed=False):
    require_file(path)
    with path.open("rb") as binary:
        dos = binary.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError(f"Expected a Windows executable at {path}")
        binary.seek(struct.unpack_from("<I", dos, 60)[0])
        header = binary.read(24)
        optional = binary.read(70)
    if (len(header) != 24 or header[:6] != b"PE\0\0\x64\x86" or
            struct.unpack_from("<H", header, 20)[0] < 70 or
            struct.unpack_from("<H", header, 22)[0] & 0x2000 or
            len(optional) != 70 or optional[:2] != b"\x0b\x02"):
        raise ValueError(f"Expected a Windows x64 executable at {path}")
    if windowed and struct.unpack_from("<H", optional, 68)[0] != 2:
        raise ValueError(f"Build {path} as a Windows GUI executable with desktop/native-package.")


def macho_executable(path, architecture):
    cpu = {"arm64": 0x0100000c, "x64": 0x01000007}[architecture]
    require_file(path)

    def has_executable_header(binary, offset):
        binary.seek(offset)
        header = binary.read(32)
        order = {b"\xcf\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xcf": ">"}.get(header[:4])
        if not order or len(header) != 32:
            return False
        machine, _, kind = struct.unpack_from(order + "III", header, 4)
        return machine == cpu and kind == 2

    with path.open("rb") as binary:
        header = binary.read(8)
        fat = {b"\xca\xfe\xba\xbe": (">", "I", 20), b"\xbe\xba\xfe\xca": ("<", "I", 20),
               b"\xca\xfe\xba\xbf": (">", "Q", 32), b"\xbf\xba\xfe\xca": ("<", "Q", 32)}.get(header[:4])
        if not fat:
            if has_executable_header(binary, 0):
                return
        else:
            order, offset_type, stride = fat
            count = struct.unpack_from(order + "I", header, 4)[0] if len(header) == 8 else 0
            if 0 < count <= 128:
                table = binary.read(count * stride)
                if len(table) == count * stride:
                    for entry in range(0, len(table), stride):
                        machine = struct.unpack_from(order + "I", table, entry)[0]
                        offset = struct.unpack_from(order + offset_type, table, entry + 8)[0]
                        if machine == cpu and offset <= path.stat().st_size - 32 and has_executable_header(binary, offset):
                            return
    raise ValueError(f"Expected a macOS {architecture} executable at {path}")


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(require_file(source), target)


def copy_tree(source, target, ignore=None):
    shutil.copytree(require_directory(source), target, symlinks=True, ignore=ignore)


def stage_preview(preview):
    shell = DESKTOP / "qt-shell"
    assets = DESKTOP / "Assets"
    design = REPOSITORY / "docs/visual-design/assets"
    target_shell = preview / "desktop/qt-shell"
    copy_tree(shell / "qml", target_shell / "qml")
    copy_file(shell / "bridge/Live.qml", target_shell / "bridge/Live.qml")
    for name in ("Aurora.png", "Aurora-Windows-32.png", "tailscale-dot.svg", "panel-top-tight.svg"):
        copy_file(assets / name, preview / "desktop/Assets" / name)
    copy_tree(assets / "Wallpapers", preview / "desktop/Assets/Wallpapers")
    copy_tree(design / "icons", preview / "docs/visual-design/assets/icons")
    brand = "brand/Aurora-transparent-512.png"
    copy_file(design / brand, preview / "docs/visual-design/assets" / brand)


def stage_windows(destination, launcher, streamer, sunshine, tailscale):
    pe_executable(launcher, windowed=True)
    pe_executable(streamer / "AuroraStream.exe")
    pe_executable(sunshine / "AuroraHost.exe")
    pe_executable(tailscale / "AuroraTailnet.exe")
    require_file(streamer / "licenses/SOURCE.txt")
    require_file(sunshine / "licenses/SOURCE.txt")
    require_file(sunshine / "zlib1.dll")
    require_file(sunshine / "assets/apps.json")
    require_file(sunshine / "assets/web/index.html")
    require_file(tailscale / "licenses/SOURCE.txt")

    major_versions = [n for n in (5, 6) if (streamer / f"Qt{n}Core.dll").is_file()]
    if len(major_versions) != 1:
        raise ValueError("Supply a streamer deployment containing one Qt runtime.")
    major, = major_versions
    launcher_bytes = launcher.read_bytes().lower()
    imports = [n for n in (5, 6) if f"qt{n}core.dll\0".encode() in launcher_bytes]
    if imports != [major]:
        raise ValueError("The launcher and streamer must use the same Qt major version.")
    for module in ("Gui", "Network", "Qml", "Quick", "QuickControls2", "Widgets"):
        require_file(streamer / f"Qt{major}{module}.dll")

    plugins = "plugins" if (streamer / "plugins/platforms/qwindows.dll").is_file() else "."
    require_file(streamer / plugins / "platforms/qwindows.dll")
    qml = "qml" if (streamer / "qml/QtQuick").is_dir() else "."
    require_file(streamer / qml / "QtQuick" / ("Controls.2" if major == 5 else "Controls") / "qmldir")

    stream_target = destination / WIN_STREAM
    copy_tree(streamer, stream_target, ignore=shutil.ignore_patterns(
        "AuroraQtShell.exe", "QtPreview", "portable.dat", "Moonlight Game Streaming Project"
    ))
    copy_tree(sunshine, destination / WIN_HOST)
    copy_file(tailscale / "AuroraTailnet.exe", destination / WIN_TAILNET / "AuroraTailnet.exe")
    copy_tree(tailscale / "licenses", destination / WIN_TAILNET / "licenses")
    copy_file(launcher, destination / "Aurora.exe")
    for library in stream_target.iterdir():
        if library.is_file() and library.suffix.lower() == ".dll":
            # Both Windows executables need app-local imports. Use the same staged DLL bytes.
            os.link(library, destination / library.name)
    (destination / "qt.conf").write_text(
        f"[Paths]\nPrefix = {WIN_STREAM}\nPlugins = {plugins}\nImports = {qml}\n"
        f"Qml2Imports = {qml}\nQmlImports = {qml}\n"
        + ("\n[Platforms]\nWindowsArguments = darkmode=1\n" if major == 5 else ""),
        encoding="utf-8",
    )
    stage_preview(destination / "QtPreview")
    for notice in ("LICENSE", "THIRD-PARTY-NOTICES.txt"):
        copy_file(REPOSITORY / notice, destination / notice)


def stage_macos(destination, launcher, runtime_app, architecture, deploy_qt):
    contents = runtime_app / "Contents"
    stage_macos_components(destination, launcher, contents, contents / "Helpers/AuroraHost.app",
                           contents / "MacOS/AuroraTailnet", contents / "Resources/Tailscale/licenses",
                           architecture, deploy_qt)


def stage_macos_components(destination, launcher, streamer_contents, host, tailnet, tailnet_licenses,
                           architecture, deploy_qt):
    macho_executable(launcher, architecture)
    if streamer_contents:
        streamer_resources = streamer_contents / "Resources"
        macho_executable(streamer_contents / "MacOS/AuroraStream", architecture)
        require_file(streamer_contents / "PlugIns/platforms/libqcocoa.dylib")
        require_file(streamer_resources / "ThirdParty/Moonlight/SOURCE.txt")
    if tailnet:
        macho_executable(tailnet, architecture)
        require_file(tailnet_licenses / "SOURCE.txt")
    if host:
        macho_executable(host / "Contents/MacOS/AuroraHost", architecture)
        with require_file(host / "Contents/Info.plist").open("rb") as info:
            host_info = plistlib.load(info)
        if (host_info.get("CFBundleExecutable") != "AuroraHost" or
                host_info.get("CFBundleIdentifier") != "org.aurora-remote.desktop.host"):
            raise ValueError("Supply the staged AuroraHost.app with its Aurora host bundle identity.")
        require_file(host / "Contents/Resources/assets/apps.json")
        require_file(host / "Contents/Resources/assets/web/index.html")
        require_file(host / "Contents/Resources/licenses/SOURCE.txt")

    contents = destination / "Contents"
    resources = contents / "Resources"
    copy_file(DESKTOP / "Info.plist", contents / "Info.plist")
    copy_file(launcher, contents / "MacOS/Aurora")
    if streamer_contents:
        copy_file(streamer_contents / "MacOS/AuroraStream", contents / "MacOS/AuroraStream")
        copy_tree(streamer_contents / "Frameworks", contents / "Frameworks")
        copy_tree(streamer_contents / "PlugIns", contents / "PlugIns")
        copy_tree(streamer_resources / "qml", resources / "qml")
        copy_file(streamer_resources / "qt.conf", resources / "qt.conf")
    stage_preview(resources / "QtPreview")

    if deploy_qt:
        deploy_qt(destination)

    if tailnet:
        copy_file(tailnet, contents / "MacOS/AuroraTailnet")
        copy_tree(tailnet_licenses, resources / "Tailscale/licenses")
    if streamer_contents:
        copy_tree(streamer_resources / "ThirdParty/Moonlight", resources / "ThirdParty/Moonlight")
        copy_file(streamer_resources / "AuroraStreamBrand.png", resources / "AuroraStreamBrand.png")
        if (streamer_resources / "moonlight.icns").is_file():
            copy_file(streamer_resources / "moonlight.icns", resources / "moonlight.icns")
    copy_file(DESKTOP / "Assets/Aurora.icns", resources / "Aurora.icns")
    for notice in ("LICENSE", "THIRD-PARTY-NOTICES.txt"):
        copy_file(REPOSITORY / notice, resources / notice)
    if host:
        host_target = contents / "Helpers/AuroraHost.app"
        if sys.platform == "darwin":
            host_target.parent.mkdir()
            subprocess.run(["/usr/bin/ditto", str(host), str(host_target)], check=True)
        else:
            copy_tree(host, host_target)


def deploy_mac_qt(bundle, qt_bin):
    contents = bundle / "Contents"
    qmake = require_file(qt_bin / "qmake")
    deploy = require_file(qt_bin / "macdeployqt")
    version = subprocess.check_output([str(qmake), "-query", "QT_VERSION"], text=True).strip()
    if not version.startswith("6."):
        raise ValueError("Use macdeployqt from the macOS Qt 6 SDK.")
    has_frameworks = (contents / "Frameworks").exists()
    if has_frameworks:
        with require_file(contents / "Frameworks/QtCore.framework/Resources/Info.plist").open("rb") as info:
            streamer_version = plistlib.load(info).get("CFBundleVersion")
        if version != streamer_version:
            raise ValueError(f"The macOS deployment tool must match the streamer Qt version {streamer_version}.")

    subprocess.run([str(deploy), str(bundle), "-codesign=-", "-no-strip", "-appstore-compliant",
                    f"-qmldir={contents / 'Resources/QtPreview/desktop/qt-shell'}"], check=True)
    if not has_frameworks:
        qt_prefix = Path(subprocess.check_output([str(qmake), "-query", "QT_INSTALL_PREFIX"], text=True).strip())
        notices = contents / "Resources/ThirdParty/Qt"
        for license_file in (*qt_prefix.glob("LICENSE.*"), *qt_prefix.glob("LICENSES/*")):
            if license_file.is_file():
                copy_file(license_file, notices / license_file.name)


def validate_package(package):
    package = package.resolve(strict=True)
    for item in package.rglob("*"):
        if item.is_symlink():
            if os.path.isabs(os.readlink(item)) or not item.resolve(strict=True).is_relative_to(package):
                raise ValueError(f"Bundle link escapes the package at {item}")
    subprocess.run([sys.executable, str(REPOSITORY / "tests/Check-NativeDesktopPackage.py"), str(package)],
                   check=True)


def assemble(output, stage):
    # Keep a failed deployment out of the caller's publication directory.
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".aurora-native-", dir=output.parent) as work:
        bundle = Path(work) / output.name
        stage(bundle)
        validate_package(bundle)
        for attempt in range(6):
            if output.exists() or output.is_symlink():
                raise FileExistsError(output)
            try:
                bundle.rename(output)
                break
            except PermissionError as error:
                # Windows can briefly hold newly inspected executables open for scanning.
                if getattr(error, "winerror", None) not in (5, 32) or attempt == 5:
                    raise
                time.sleep(0.25 * (attempt + 1))
    print(output)


def check_output_location(output, *input_trees):
    sources = input_trees + (DESKTOP / "qt-shell", DESKTOP / "Assets", REPOSITORY / "docs/visual-design/assets")
    if any(output.resolve().is_relative_to(tree.resolve()) for tree in sources):
        raise ValueError("Choose an output outside the runtime and source input trees.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    platform = parser.add_subparsers(dest="platform", required=True)
    windows = platform.add_parser("windows")
    for flag in ("launcher", "streamer-directory", "sunshine-directory", "tailscale-directory", "output"):
        windows.add_argument("--" + flag, type=Path, required=True)
    mac = platform.add_parser("macos")
    mac.add_argument("--output", type=Path, required=True)
    mac.add_argument("--architecture", choices=("arm64", "x64"), required=True)
    mac_source = mac.add_mutually_exclusive_group(required=True)
    mac_source.add_argument("--runtime-app", type=Path, help="Integrated Aurora.app for donor verification")
    mac_source.add_argument("--streamer-contents", type=Path, help="Contents directory from Build-Mac-Streamer.sh")
    mac_source.add_argument("--without-streamer", action="store_true", help="Deploy a launcher-only Qt bundle")
    mac.add_argument("--launcher", type=Path, help="Custom Qt launcher (defaults to the streamer's deployed launcher)")
    mac.add_argument("--qt-bin", type=Path, help="Qt SDK bin directory for deploying a custom launcher")
    mac.add_argument("--host-app", type=Path, help="AuroraHost.app from Build-Mac-Sunshine.sh")
    mac.add_argument("--tailscale-directory", type=Path, help="Output directory from Build-Tailnet.ps1")
    args = parser.parse_args()

    if args.platform == "macos":
        if args.host_app and not args.streamer_contents:
            mac.error("--host-app requires --streamer-contents")
        if args.runtime_app and args.tailscale_directory:
            mac.error("--runtime-app already supplies Tailnet")
        if bool(args.launcher) != bool(args.qt_bin) or (not args.streamer_contents and not args.launcher):
            mac.error("Supply --launcher and --qt-bin together, or use the launcher from --streamer-contents")

    try:
        if args.platform == "windows":
            if sys.platform != "win32" or args.output.suffix.lower() == ".app":
                raise ValueError("Stage the Windows portable folder on Windows.")
            check_output_location(args.output, args.streamer_directory, args.sunshine_directory, args.tailscale_directory)
            assemble(args.output, lambda folder: stage_windows(
                folder, args.launcher, args.streamer_directory, args.sunshine_directory, args.tailscale_directory))
        else:
            if sys.platform != "darwin" or args.output.name != "Aurora.app":
                raise ValueError("Stage Aurora.app on macOS.")

            deploy_qt = None
            if args.qt_bin:
                def deploy_qt(bundle):
                    deploy_mac_qt(bundle, args.qt_bin)

            if args.runtime_app:
                check_output_location(args.output, args.runtime_app)
                assemble(args.output, lambda bundle: stage_macos(
                    bundle, args.launcher, args.runtime_app, args.architecture, deploy_qt))
            else:
                input_trees = (args.streamer_contents, args.host_app, args.tailscale_directory)
                check_output_location(args.output, *(path for path in input_trees if path is not None))
                # The streamer builder deploys its launcher alongside the shared Qt runtime.
                launcher = args.launcher or args.streamer_contents / "MacOS/AuroraQtShell"
                tailnet = args.tailscale_directory
                assemble(args.output, lambda bundle: stage_macos_components(
                    bundle, launcher, args.streamer_contents, args.host_app,
                    tailnet / "AuroraTailnet" if tailnet else None, tailnet / "licenses" if tailnet else None,
                    args.architecture, deploy_qt))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Native Aurora packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
