import stat
import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def package(bundle: Path, archive: Path) -> None:
    executable = bundle / "Contents" / "MacOS" / "Aurora"
    if not executable.is_file():
        raise FileNotFoundError(executable)

    partial = archive.with_suffix(".tmp")
    with ZipFile(partial, "w", compression=ZIP_DEFLATED, compresslevel=6) as output:
        for path in sorted(bundle.rglob("*")):
            if not path.is_file():
                continue
            entry = ZipInfo.from_file(path, path.relative_to(bundle.parent).as_posix())
            entry.create_system = 3
            mode = 0o755 if path.parent == executable.parent else 0o644
            entry.external_attr = (stat.S_IFREG | mode) << 16
            entry.compress_type = ZIP_DEFLATED
            output.writestr(entry, path.read_bytes())
    partial.replace(archive)


if __name__ == "__main__":
    package(Path(sys.argv[1]), Path(sys.argv[2]))
