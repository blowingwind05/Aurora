#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fetch a locked Aurora engine build tree and record its source provenance."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]
METADATA = HERE / "EngineSources.json"


def engine_submodule(component, git):
    command = [git, "-C", str(REPOSITORY)]
    key = f"submodule.{component}"
    config = [*command, "config", "--file", ".gitmodules", "--get"]
    relative = subprocess.check_output([*config, f"{key}.path"], encoding="utf-8").strip()
    repository = subprocess.check_output([*config, f"{key}.url"], encoding="utf-8").strip()
    entries = subprocess.check_output([*command, "ls-files", "--stage", "--", relative],
                                      encoding="utf-8").splitlines()
    if len(entries) != 1:
        raise RuntimeError(f"Expected one Git submodule entry at {relative}")
    mode, revision, stage = entries[0].split("\t", 1)[0].split()
    if mode != "160000" or stage != "0":
        raise RuntimeError(f"Resolve the Git submodule entry for {relative} before building")
    return REPOSITORY / relative, repository, revision


def checkout(component, platform, source, git, origin):
    metadata = json.loads(METADATA.read_text(encoding="utf-8"))[component]
    local_checkout, repository, locked_revision = engine_submodule(component, git)
    environment = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    # The parent index selects the revision. Each component checkout owns its index.
    environment.pop("GIT_INDEX_FILE", None)
    if not origin and (local_checkout / ".git").exists():
        has_revision = subprocess.run(
            [git, "-C", str(local_checkout), "cat-file", "-e", f"{locked_revision}^{{commit}}"],
            env=environment, capture_output=True, check=False,
        )
        if has_revision.returncode == 0:
            origin = local_checkout

    source.mkdir(parents=True)
    command = [git, "-C", str(source), "-c", "core.longpaths=true",
               "-c", "core.autocrlf=false", "-c", "credential.helper=",
               "-c", "submodule.recurse=false"]
    subprocess.run([*command, "init", "--quiet"], env=environment, check=True)
    subprocess.run([*command, "remote", "add", "origin", repository],
                   env=environment, check=True)
    subprocess.run([*command, "fetch", "--quiet", "--no-tags", "--depth=1",
                    str(origin) if origin else "origin", locked_revision], env=environment, check=True)
    subprocess.run([*command, "checkout", "--quiet", "--detach", "FETCH_HEAD"],
                   env=environment, check=True)
    revision = subprocess.check_output([*command, "rev-parse", "HEAD"],
                                       encoding="ascii", env=environment).strip()
    if revision != locked_revision:
        raise RuntimeError(f"Unexpected {component} revision {revision}")

    subprocess.run([sys.executable, str(HERE / "Initialize-BuildSubmodules.py"),
                    component, platform, str(source), "--git", git], env=environment, check=True)

    submodules = subprocess.check_output([*command, "submodule", "status", "--recursive"],
                                         encoding="utf-8", env=environment)
    checked_out = [line for line in submodules.splitlines() if line.startswith(" ")]
    (source / "AURORA-SOURCE.txt").write_text("\n".join([
        f"Repository: {repository}",
        f"Revision: {revision}",
        f"Upstream repository: {metadata['upstreamRepository']}",
        f"Upstream release: {metadata['upstreamRelease']}",
        f"Upstream revision: {metadata['upstreamRevision']}",
        "",
        "Submodules:",
        *checked_out,
        "",
    ]), encoding="utf-8")


def publish_index(index, source):
    if not (source / "AURORA-SOURCE.txt").is_file():
        raise RuntimeError(f"Cannot publish an unprepared engine source tree: {source}")
    index.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=index, encoding="utf-8", delete=False) as stream:
        pointer = Path(stream.name)
        stream.write(f"{source}\n")
    try:
        os.replace(pointer, index / "latest-source.txt")
    finally:
        pointer.unlink(missing_ok=True)
    print(f"Aurora upstream source: {source}")


def verify_manifest(component, manifest, git):
    _, repository, revision = engine_submodule(component, git)
    lines = manifest.read_text(encoding="utf-8").splitlines()
    for key, expected in (("Repository: ", repository), ("Revision: ", revision)):
        values = [line[len(key):] for line in lines if line.startswith(key)]
        if values != [expected]:
            raise RuntimeError(f"{manifest}: expected exactly one {key}{expected} from the Aurora submodule")
    print(f"{component}: verified {revision}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    prepare = actions.add_parser("checkout")
    prepare.add_argument("component", choices=("moonlight", "sunshine"))
    prepare.add_argument("platform", choices=("windows", "macos"))
    prepare.add_argument("source", type=Path)
    prepare.add_argument("--git", default="git")
    prepare.add_argument("--origin", type=Path, help="Checkout containing the exact locked revision")
    index = actions.add_parser("index")
    index.add_argument("index", type=Path)
    index.add_argument("source", type=Path)
    verify = actions.add_parser("verify", help="Verify a deployed engine's source manifest against the Git submodule")
    verify.add_argument("component", choices=("moonlight", "sunshine"))
    verify.add_argument("manifest", type=Path)
    verify.add_argument("--git", default="git")
    args = parser.parse_args()

    if args.action == "checkout":
        checkout(args.component, args.platform, args.source.resolve(), args.git,
                 args.origin.resolve(strict=True) if args.origin else None)
    elif args.action == "index":
        publish_index(args.index.resolve(), args.source.resolve(strict=True))
    else:
        verify_manifest(args.component, args.manifest, args.git)


if __name__ == "__main__":
    main()
