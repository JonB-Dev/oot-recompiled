#!/usr/bin/env python3
"""Hash a ROM and refuse to agree that it is the right one unless it actually is.

Every address in every recompiler config downstream derives from the uncompressed ROM, so a
near-miss here does not fail here. It fails thousands of functions later, as a wrong jump target or
a section that does not resolve, with nothing pointing back at this file. That is the whole reason
this exists as a gate rather than as a comment in a README.

    python tools/verify_rom.py --file "<rom>" --expect-compressed
    python tools/verify_rom.py --file "<rom>" --expect-md5 6829a16db1a34e8ce989847cd8da8d9a
    python tools/verify_rom.py --file "<rom>"                 # just report, exit 0

Exit codes: 0 verified or reporting only, 1 mismatch, 2 the file is unusable.

Nothing here prints ROM CONTENT, only hashes, sizes and the header magic. A verification tool that
dumps bytes into a log is a way to leak the thing the whole project is careful about.
"""

import argparse
import hashlib
import sys
from pathlib import Path

# NTSC-U 1.0, the only revision this project supports. Everything is specific to it.
KNOWN = {
    "5bd1fe107bf8106b2ab6650abecd54d6": "OoT NTSC-U 1.0, compressed retail ROM",
    "6829a16db1a34e8ce989847cd8da8d9a": "OoT NTSC-U 1.0, decompressed",
}
COMPRESSED_MD5 = "5bd1fe107bf8106b2ab6650abecd54d6"
COMPRESSED_SHA1 = "ad69c91157f6705e8ab06c79fe08aad47bb57ba7"

# The first four bytes say how the dumper wrote it. Getting this wrong is the classic way somebody
# spends an afternoon on a hash that is "nearly right": the bytes are all present, in the wrong
# order, so the file is the correct size and hashes to nothing recognizable.
MAGIC = {
    bytes([0x80, 0x37, 0x12, 0x40]): ("z64", "big endian, what this project needs"),
    bytes([0x37, 0x80, 0x40, 0x12]): ("v64", "byte swapped, convert it first"),
    bytes([0x40, 0x12, 0x37, 0x80]): ("n64", "little endian, convert it first"),
}


def digest(path: Path) -> tuple[str, str, int, bytes]:
    md5, sha1, size = hashlib.md5(), hashlib.sha1(), 0
    head = b""
    with path.open("rb") as fh:
        while chunk := fh.read(1 << 20):
            if not head:
                head = chunk[:4]
            md5.update(chunk)
            sha1.update(chunk)
            size += len(chunk)
    return md5.hexdigest(), sha1.hexdigest(), size, head


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify a ROM by hash before anything parses it.")
    ap.add_argument("--file", required=True, help="path to the ROM")
    ap.add_argument("--expect-compressed", action="store_true",
                    help="expect the compressed retail NTSC-U 1.0 ROM, md5 and sha1 both")
    ap.add_argument("--expect-md5", metavar="MD5", help="expect this md5")
    ap.add_argument("--expect-sha1", metavar="SHA1", help="expect this sha1")
    args = ap.parse_args()

    path = Path(args.file)
    if not path.is_file():
        print(f"  FAIL  not a file: {path}")
        return 2

    try:
        md5, sha1, size, head = digest(path)
    except OSError as exc:
        print(f"  FAIL  could not read {path}: {exc}")
        return 2

    fmt, note = MAGIC.get(head, ("unknown", "not a recognized N64 ROM header"))

    print()
    print(f"  file    {path.name}")
    print(f"  size    {size} bytes ({size / (1 << 20):.0f} MiB)")
    print(f"  format  {fmt}  ({note})")
    print(f"  md5     {md5}")
    print(f"  sha1    {sha1}")
    if md5 in KNOWN:
        print(f"  known   {KNOWN[md5]}")
    print()

    expect_md5 = COMPRESSED_MD5 if args.expect_compressed else args.expect_md5
    expect_sha1 = COMPRESSED_SHA1 if args.expect_compressed else args.expect_sha1

    if not expect_md5 and not expect_sha1:
        print("  reporting only, nothing was asserted.")
        print()
        return 0

    ok = True
    if expect_md5:
        if md5 == expect_md5.lower():
            print(f"  MATCH   md5 {md5}")
        else:
            ok = False
            print(f"  FAIL    md5 expected {expect_md5.lower()}")
            print(f"          md5 actual   {md5}")
    if expect_sha1:
        if sha1 == expect_sha1.lower():
            print(f"  MATCH   sha1 {sha1}")
        else:
            ok = False
            print(f"  FAIL    sha1 expected {expect_sha1.lower()}")
            print(f"          sha1 actual   {sha1}")

    if not ok and fmt in ("v64", "n64"):
        print()
        print(f"  The header says this is a {fmt} dump. The bytes are probably all here in the")
        print("  wrong order rather than wrong, which is why the size looks right.")

    print()
    print("  VERIFIED" if ok else "  MISMATCH. Do not proceed: every address downstream derives from this file.")
    print()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
