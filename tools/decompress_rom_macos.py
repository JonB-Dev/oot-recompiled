#!/usr/bin/env python3
"""Decompress a verified NTSC-U 1.0 ROM using pinned zeldaret/oot tooling."""

import hashlib
import subprocess
import sys
from pathlib import Path


DECOMP_PIN = "14759abf9abb4295de142cdc1452998630ea70fb"
INPUT_MD5 = "5bd1fe107bf8106b2ab6650abecd54d6"
OUTPUT_MD5 = "6829a16db1a34e8ce989847cd8da8d9a"


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: decompress_rom_macos.py /path/to/your/USA-1.0.z64", file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parent.parent
    decomp = root / "upstream/oot"
    source = Path(sys.argv[1])
    output = root / "build/oot.ntsc-1.0.rom_uncompressed.z64"
    if not (decomp / "tools/decompress_baserom.py").is_file():
        print("DECOMP_MISSING: clone zeldaret/oot into ignored upstream/oot", file=sys.stderr)
        return 1
    actual = subprocess.check_output(["git", "-C", str(decomp), "rev-parse", "HEAD"], text=True).strip()
    if actual != DECOMP_PIN:
        print(f"DECOMP_PIN_MISMATCH: expected {DECOMP_PIN}, found {actual}", file=sys.stderr)
        return 1
    if not source.is_file():
        print("ROM_MISSING", file=sys.stderr)
        return 1
    data = source.read_bytes()
    if hashlib.md5(data).hexdigest() != INPUT_MD5:
        print("ROM_CHECKSUM_MISMATCH", file=sys.stderr)
        return 1
    if output.is_file() and hashlib.md5(output.read_bytes()).hexdigest() == OUTPUT_MD5:
        print("ROM_DECOMPRESSED_VERIFIED")
        return 0

    sys.path.insert(0, str(root / "build/pydeps"))
    sys.path.insert(0, str(decomp / "tools"))
    try:
        import decompress_baserom  # type: ignore[import-not-found]
        import dmadata  # type: ignore[import-not-found]
        import version_config  # type: ignore[import-not-found]
    except ImportError as exc:
        print(f"DECOMP_DEPENDENCY_MISSING: {exc}", file=sys.stderr)
        return 1
    config = version_config.load_version_config("ntsc-1.0")
    entries = dmadata.read_dmadata(data, config.dmadata_start)
    decompressed = decompress_baserom.decompress_rom(bytearray(data), config.dmadata_start, entries, False)
    if hashlib.md5(decompressed).hexdigest() != OUTPUT_MD5:
        print("DECOMP_OUTPUT_CHECKSUM_MISMATCH", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".tmp")
    temporary.write_bytes(decompressed)
    temporary.replace(output)
    print("ROM_DECOMPRESSED_VERIFIED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
