#!/usr/bin/env python3
"""Apply the pinned RT64 patch series to an isolated build copy on macOS."""

import shutil
import subprocess
import sys
import tempfile
import hashlib
from pathlib import Path


RT64_PIN = "43373749dac9bbc1b653e6a02aed40a9e1783bed"
MARKER = ".oot-rt64-build-copy"


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.name == MARKER:
            continue
        digest.update(path.relative_to(root).as_posix().encode())
        with path.open("rb") as source:
            while chunk := source.read(1 << 20):
                digest.update(chunk)
    return digest.hexdigest()


def prepare(source: Path, destination: Path, series: Path, pin: str = RT64_PIN) -> int:
    source, destination, series = source.resolve(), destination.resolve(), series.resolve()
    if (source == destination or source in destination.parents or
            destination in source.parents or destination == series or
            destination in series.parents):
        raise RuntimeError("RT64_PATH_OVERLAP: keep source, patches, and build copy separate")
    if not (source / "CMakeLists.txt").is_file():
        raise RuntimeError(f"RT64 submodule is missing: {source}")
    actual = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != pin:
        raise RuntimeError(f"RT64_PIN_MISMATCH: expected {pin}, found {actual}")
    dirty = subprocess.check_output(
        ["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all"], text=True
    ).strip()
    if dirty:
        raise RuntimeError("RT64_SOURCE_DIRTY: inspect the pinned submodule before copying it")

    patches = sorted(series.glob("*.patch")) + sorted((series / "macos").glob("*.patch"))
    if not patches:
        raise RuntimeError("RT64_SERIES_EMPTY: no renderer patches were found")
    series_hash = hashlib.sha256(pin.encode())
    for patch in patches:
        series_hash.update(patch.relative_to(series).as_posix().encode())
        series_hash.update(patch.read_bytes())
    expected = series_hash.hexdigest()
    marker = destination / MARKER
    if marker.is_file():
        recorded = marker.read_text().splitlines()
        if len(recorded) == 2 and recorded[0] == expected and recorded[1] == tree_digest(destination):
            return len(patches)

    destination.parent.mkdir(parents=True, exist_ok=True)
    backup = destination.with_name(destination.name + ".previous")
    if backup.exists():
        raise RuntimeError(f"Previous RT64 backup needs inspection: {backup}")

    temporary = Path(tempfile.mkdtemp(prefix="rt64-src-", dir=destination.parent))
    try:
        shutil.rmtree(temporary)
        shutil.copytree(source, temporary, ignore=shutil.ignore_patterns(".git"))
        # A temporary repository anchors git apply here. Without it, git finds the
        # surrounding OoT checkout and silently skips every patch outside its prefix.
        subprocess.run(["git", "init", "-q", str(temporary)], check=True)
        for patch in patches:
            result = subprocess.run(
                ["git", "apply", "--whitespace=nowarn", str(patch)],
                cwd=temporary, capture_output=True, text=True,
            )
            if result.returncode:
                raise RuntimeError(f"RT64_PATCH_FAILED {patch.name}: {result.stderr.strip()}")
        shutil.rmtree(temporary / ".git")
        (temporary / MARKER).write_text(expected + "\n" + tree_digest(temporary) + "\n")
        if destination.exists():
            destination.rename(backup)
        try:
            temporary.rename(destination)
        except OSError:
            if backup.exists():
                backup.rename(destination)
            raise
        if backup.exists():
            shutil.rmtree(backup)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return len(patches)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    try:
        count = prepare(root / "lib/rt64", root / "build-cmake/rt64-src", root / "rt64-patches")
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        return 1
    print(f"RT64_SERIES_APPLIED {count} patches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
