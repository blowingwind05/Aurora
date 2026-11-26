# SPDX-License-Identifier: GPL-3.0-only
"""Replay Windows image decoding and painting with generated input files."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def png_chunk(name, contents):
    return (struct.pack(">I", len(contents)) + name + contents
            + struct.pack(">I", zlib.crc32(name + contents) & 0xFFFFFFFF))


def png(width, height, color_type, data):
    return (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(data))
            + png_chunk(b"IEND", b""))


def put(profile, label, data):
    directory = profile / label
    directory.mkdir()
    (directory / "privacy-screensaver.png").write_bytes(data)


def run(executable, directory):
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=directory) as isolated:
        root = Path(isolated)
        rows = b"".join(b"\x00" + b"".join(bytes((x * 30, y * 40, 77)) for x in range(8))
                        for y in range(6))
        opaque = png(8, 6, 2, rows)
        put(root, "opaque", opaque)
        put(root, "opaque-rgba", png(1, 1, 6, bytes((0, 8, 20, 40, 255))))
        put(root, "transparent", png(1, 1, 6, bytes((0, 8, 20, 40, 80))))
        # A transparency key in an RGB PNG must also be rejected.
        put(root, "transparent-key", opaque[:33]
            + png_chunk(b"tRNS", struct.pack(">HHH", 0, 0, 77)) + opaque[33:])
        put(root, "corrupt", b"\x89PNG\r\n\x1a\n")
        put(root, "oversized", png(8193, 1, 2, b"\x00"))
        put(root, "not-png", b"GIF89a")
        (root / "fixed-name").mkdir()
        (root / "fixed-name" / "other.png").write_bytes(opaque)
        return subprocess.run([str(executable), str(root)], check=False, timeout=20).returncode


if __name__ == "__main__":
    sys.exit(run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()))
