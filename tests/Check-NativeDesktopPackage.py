# SPDX-License-Identifier: GPL-3.0-only
"""Detect managed desktop dependencies in an extracted release or ZIP."""

from argparse import ArgumentParser
from pathlib import Path, PurePosixPath
import struct
import sys
from zipfile import BadZipFile, ZipFile


# Source: dotnet/runtime, Microsoft.NET.HostModel.Bundle.Bundler.
# The apphost signature exists in ordinary apphosts and single-file bundles.
APPHOST = bytes.fromhex("8b1202b96a612038727b930214d7a03213f5b9e6efae3318ee3b2dce24b36aae")
RUNTIME_FILES = {
    "aurora.dll", "coreclr.dll", "libcoreclr.dylib", "libcoreclr.so",
    "hostfxr.dll", "libhostfxr.dylib", "libhostfxr.so",
    "hostpolicy.dll", "libhostpolicy.dylib", "libhostpolicy.so",
    "system.private.corelib.dll", "mscoree.dll", "mono-2.0-sgen.dll",
    "libmonosgen-2.0.dylib",
}
MACHO = {
    b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}


def has_clr_header(stream, first):
    if first[:2] != b"MZ" or len(first) < 64:
        return False
    pe_offset = struct.unpack_from("<I", first, 60)[0]
    stream.seek(pe_offset)
    header = stream.read(24)
    if len(header) != 24 or header[:4] != b"PE\0\0":
        return False
    optional_size = struct.unpack_from("<H", header, 20)[0]
    optional = stream.read(optional_size)
    magic = optional[:2]
    first_directory = {b"\x0b\x01": 96, b"\x0b\x02": 112}.get(magic)
    if not first_directory or len(optional) < first_directory + 15 * 8:
        return False
    directory_count = struct.unpack_from("<I", optional, first_directory - 4)[0]
    return directory_count >= 15 and any(struct.unpack_from("<II", optional, first_directory + 14 * 8))


def manages_dotnet(stream):
    first = stream.read(64)
    if first[:2] != b"MZ" and first[:4] not in MACHO:
        return False
    if has_clr_header(stream, first):
        return True
    stream.seek(0)
    overlap = b""
    while block := stream.read(1024 * 1024):
        search = overlap + block
        if APPHOST in search:
            return True
        overlap = search[-len(APPHOST) + 1:]
    return False


def check_package(entries, open_entry, main):
    names = set(entries)
    if main not in names:
        raise ValueError(f"Missing the desktop launcher: {main}")
    failures = []
    for name in sorted(names):
        filename = PurePosixPath(name).name.lower()
        if filename in RUNTIME_FILES or filename.startswith("avalonia.") or filename.endswith(
            (".deps.json", ".runtimeconfig.json")
        ):
            failures.append(name)
            continue
        with open_entry(name) as entry:
            if manages_dotnet(entry):
                failures.append(f"{name} (managed assembly or .NET apphost)")
    return failures


def run():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    package = args.package
    try:
        if package.is_file() and package.suffix.lower() == ".zip":
            with ZipFile(package) as archive:
                files = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
                main = "Aurora.exe" if "Aurora.exe" in files else "Aurora.app/Contents/MacOS/Aurora"
                failures = check_package(files, archive.open, main)
        elif package.is_dir():
            mac = package.suffix == ".app"
            main = "Contents/MacOS/Aurora" if mac else "Aurora.exe"
            files = [path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file()]
            failures = check_package(files, lambda name: (package / name).open("rb"), main)
        else:
            raise ValueError("Pass a Windows portable folder, Aurora.app, or a release ZIP.")
    except (BadZipFile, OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"{package.name}: no managed desktop files, CLR headers, or .NET apphosts found.")
    return 0


if __name__ == "__main__":
    sys.exit(run())
