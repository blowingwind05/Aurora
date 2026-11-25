#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check out only the Git submodules used by Aurora's runtime builds."""

import argparse
from pathlib import Path
import subprocess


MOONLIGHT_COMMON = "moonlight-common-c/moonlight-common-c"
SUNSHINE_COMMON = "third-party/moonlight-common-c"
LIZARDBYTE_COMMON = "third-party/lizardbyte-common"
VIRTUAL_HID = "third-party/libvirtualhid"

MOONLIGHT = (
    "app/SDL_GameControllerDB",
    "h264bitstream/h264bitstream",
    "libs",
    MOONLIGHT_COMMON,
    "soundio/libsoundio",
)

SUNSHINE = (
    "third-party/Simple-Web-Server",
    "third-party/build-deps",
    "third-party/libdisplaydevice",
    VIRTUAL_HID,
    LIZARDBYTE_COMMON,
    SUNSHINE_COMMON,
)

SUNSHINE_PLATFORM = {
    "windows": ("third-party/ViGEmClient", "third-party/nvapi"),
    "macos": ("third-party/TPCircularBuffer",),
}


def initialize(git, source, paths):
    command = [
        git, "-C", str(source), "-c", "core.longpaths=true",
        "-c", "credential.helper=", "-c", "submodule.recurse=false",
    ]
    subprocess.run(
        [*command, "submodule", "update", "--init", "--checkout", "--depth", "1",
         "--jobs", "8", "--", *paths],
        check=True,
    )
    status = subprocess.check_output(
        [*command, "submodule", "status", "--", *paths], encoding="utf-8"
    ).splitlines()
    if len(status) != len(paths) or any(not entry.startswith(" ") for entry in status):
        raise RuntimeError(f"Submodules do not match the pinned commits in {source}")


def pinned_revision(git, source, submodule):
    return subprocess.check_output(
        [git, "-C", str(source), "rev-parse", "--verify", f"HEAD:{submodule}"],
        encoding="utf-8",
    ).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=("moonlight", "sunshine"))
    parser.add_argument("platform", choices=SUNSHINE_PLATFORM)
    parser.add_argument("source", type=Path)
    parser.add_argument("--git", default="git", help="Git executable used by the build")
    args = parser.parse_args()

    if args.component == "moonlight":
        initialize(args.git, args.source, MOONLIGHT)
        initialize(args.git, args.source / MOONLIGHT_COMMON, ("enet",))
    else:
        initialize(args.git, args.source, SUNSHINE + SUNSHINE_PLATFORM[args.platform])
        initialize(args.git, args.source / SUNSHINE_COMMON, ("enet", "nanors"))
        if args.platform == "windows":
            host_revision = pinned_revision(args.git, args.source, LIZARDBYTE_COMMON)
            hid_revision = pinned_revision(args.git, args.source / VIRTUAL_HID, LIZARDBYTE_COMMON)
            if host_revision != hid_revision:
                raise RuntimeError("Sunshine and libvirtualhid pin different lizardbyte-common revisions")


if __name__ == "__main__":
    main()
