# SPDX-License-Identifier: GPL-3.0-only
"""Offline layout fixtures with synthetic executable headers and a mock Mac deployment."""

import importlib.util
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest


sys.dont_write_bytecode = True
DESKTOP = Path(__file__).resolve().parents[1] / "desktop"
spec = importlib.util.spec_from_file_location("native_assembly", DESKTOP / "Build-NativeDesktop.py")
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)


def put(root, relative, data=b"fixture"):
    file = root / relative
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(data)
    return file


def pe(windowed=False):
    dos = bytearray(64)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 60, 64)
    header = bytearray(24)
    header[:6] = b"PE\0\0\x64\x86"
    struct.pack_into("<H", header, 20, 70)
    optional = bytearray(70)
    optional[:2] = b"\x0b\x02"
    struct.pack_into("<H", optional, 68, 2 if windowed else 3)
    return dos + header + optional + b"Qt5Core.dll\0"


def macho():
    return struct.pack("<8I", 0xfeedfacf, 0x0100000c, 0, 2, 0, 0, 0, 0)


def windows_inputs(root):
    streamer, sunshine, tailscale = (root / name for name in ("streamer", "sunshine", "tailscale"))
    main = put(root, "launcher.exe", pe(windowed=True))
    put(streamer, "AuroraStream.exe", pe())
    put(streamer, "AuroraQtShell.exe", b"obsolete companion")
    put(streamer, "portable.dat")
    put(streamer, "QtPreview/stale.txt")
    for module in ("Core", "Gui", "Network", "Qml", "Quick", "QuickControls2", "Widgets"):
        put(streamer, f"Qt5{module}.dll")
    put(streamer, "platforms/qwindows.dll")
    put(streamer, "QtQuick/Controls.2/qmldir")
    put(sunshine, "AuroraHost.exe", pe())
    put(sunshine, "assets/apps.json")
    put(tailscale, "AuroraTailnet.exe", pe())
    for directory in (streamer, sunshine, tailscale):
        put(directory, "licenses/SOURCE.txt")
    return main, streamer, sunshine, tailscale


class AssemblyTest(unittest.TestCase):
    def setUp(self):
        self.workspace = TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name)

    def test_windows_fixture_uses_one_deployed_qt_tree(self):
        output = self.root / "Windows portable fixture"
        inputs = windows_inputs(self.root)
        assembly.assemble(output, lambda target: assembly.stage_windows(target, *inputs))
        self.assertTrue((output / "Aurora.exe").is_file())
        self.assertTrue((output / "QtPreview/desktop/qt-shell/bridge/WindowsSessionPopup.qml").is_file())
        self.assertFalse((output / "QtPreview/stale.txt").exists())
        streamer = output / "Components/Moonlight"
        self.assertFalse((streamer / "AuroraQtShell.exe").exists())
        self.assertFalse((streamer / "portable.dat").exists())
        self.assertTrue(os.path.samefile(output / "Qt5Core.dll", streamer / "Qt5Core.dll"))
        self.assertIn("Prefix = Components/Moonlight", (output / "qt.conf").read_text())
        self.assertEqual((inputs[1] / "AuroraQtShell.exe").read_bytes(), b"obsolete companion")
        with self.assertRaises(FileExistsError):
            assembly.assemble(output, lambda _: self.fail("Existing output was passed to the stager"))

    def test_managed_payload_rejects_publication(self):
        inputs = windows_inputs(self.root)
        put(inputs[1], "coreclr.dll")
        output = self.root / "Rejected fixture"
        with self.assertRaises(subprocess.CalledProcessError):
            assembly.assemble(output, lambda target: assembly.stage_windows(target, *inputs))
        self.assertFalse(output.exists())

    def test_mac_fixture_stages_host_after_qt_deployment(self):
        donor = self.root / "Prior Aurora.app"
        contents = donor / "Contents"
        launcher = put(contents, "MacOS/AuroraQtShell", macho())
        put(contents, "MacOS/Aurora", b"obsolete managed launcher")
        for path in ("MacOS/AuroraStream", "MacOS/AuroraTailnet", "Helpers/AuroraHost.app/Contents/MacOS/AuroraHost"):
            put(contents, path, macho())
        put(contents, "Frameworks/QtCore.framework/Versions/A/QtCore")
        if os.name != "nt":
            (contents / "Frameworks/QtCore.framework/QtCore").symlink_to("Versions/A/QtCore")
        for path in ("PlugIns/platforms/libqcocoa.dylib", "Resources/qml/QtQuick/qmldir",
                     "Resources/qt.conf", "Resources/AuroraStreamBrand.png", "Resources/Tailscale/licenses/SOURCE.txt",
                     "Resources/ThirdParty/Moonlight/SOURCE.txt", "Helpers/AuroraHost.app/Contents/Resources/assets/apps.json",
                     "Helpers/AuroraHost.app/Contents/Resources/assets/web/index.html",
                     "Helpers/AuroraHost.app/Contents/Resources/licenses/SOURCE.txt"):
            put(contents, path)
        host_id = {"CFBundleIdentifier": "org.aurora-remote.desktop.host", "CFBundleExecutable": "AuroraHost"}
        put(contents, "Helpers/AuroraHost.app/Contents/Info.plist", plistlib.dumps(host_id))

        def mock_macdeployqt(bundle):
            staged = bundle / "Contents"
            self.assertEqual((staged / "MacOS/Aurora").read_bytes(), macho())
            self.assertFalse((staged / "Helpers").exists())
            self.assertTrue((staged / "Resources/QtPreview/desktop/qt-shell/bridge/Live.qml").exists())

        output = self.root / "Aurora.app"
        assembly.assemble(output, lambda bundle: assembly.stage_macos(bundle, launcher, donor, "arm64", mock_macdeployqt))
        result = output / "Contents"
        self.assertEqual((result / "Info.plist").read_bytes(), (DESKTOP / "Info.plist").read_bytes())
        self.assertTrue((result / "Helpers/AuroraHost.app/Contents/MacOS/AuroraHost").is_file())
        self.assertFalse((result / "MacOS/AuroraQtShell").exists())
        self.assertEqual((contents / "MacOS/Aurora").read_bytes(), b"obsolete managed launcher")
        if os.name != "nt":
            self.assertTrue((result / "Frameworks/QtCore.framework/QtCore").is_symlink())


if __name__ == "__main__":
    unittest.main()
