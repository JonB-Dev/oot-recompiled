#!/usr/bin/env python3
"""Turn a microcode symbol's vram address into the ROM file offset RSPRecomp needs.

RSPRecomp is given a raw file offset, a length and the RSP address the code runs at. It does no
symbol lookup of its own, so a wrong offset does not fail: it recompiles whatever bytes happen to
be there into a plausible looking C++ function that does the wrong thing.

    python tools/calc_ucode_offset.py --syms OoTRecompSyms/oot.ntsc-1.0.datasyms.toml \
        --rom build/oot.ntsc-1.0.rom_uncompressed.z64 --vram 0x800E2FC0 --size 0xFB0

The offset is resolved through the section table: every section carries both its rom offset and its
vram, so the file offset of any address inside it is (vram - section.vram) + section.rom.

It then READS THE ROM at that offset and checks the first words decode as plausible RSP
instructions, because the whole failure mode here is silence.
"""

import argparse
import re
import sys
from pathlib import Path

SECTION = re.compile(
    r"\[\[section\]\]\s*\n"
    r"(?:(?!\[\[section\]\]).)*?",
    re.S,
)


def sections(toml_text: str):
    """Yield (name, rom, vram, size) for every section that declares all of them."""
    for block in toml_text.split("[[section]]")[1:]:
        head = block.split("symbols", 1)[0].split("functions", 1)[0]

        def grab(key):
            m = re.search(rf"^{key} = (0x[0-9A-Fa-f]+|\d+)", head, re.M)
            return int(m.group(1), 0) if m else None

        name_m = re.search(r'^name = "([^"]*)"', head, re.M)
        rom, vram, size = grab("rom"), grab("vram"), grab("size")
        if name_m and rom is not None and vram is not None and size is not None:
            yield name_m.group(1), rom, vram, size


def main() -> int:
    ap = argparse.ArgumentParser(description="vram to ROM offset for an RSP microcode blob.")
    ap.add_argument("--syms", required=True, type=Path, help="a symbols toml carrying section rom and vram")
    ap.add_argument("--rom", type=Path, help="the uncompressed ROM, to sanity check the result")
    ap.add_argument("--vram", required=True, help="start address, e.g. 0x800E2FC0")
    ap.add_argument("--size", required=True, help="length in bytes, e.g. 0xFB0")
    args = ap.parse_args()

    vram = int(args.vram, 0)
    size = int(args.size, 0)

    text = args.syms.read_text(encoding="utf-8", errors="replace")
    hit = None
    for name, rom, sec_vram, sec_size in sections(text):
        if sec_vram <= vram < sec_vram + sec_size:
            # Prefer the tightest enclosing section if several overlap.
            if hit is None or sec_size < hit[3]:
                hit = (name, rom, sec_vram, sec_size)

    print()
    print(f"  vram      0x{vram:08X}")
    print(f"  size      0x{size:X} ({size} bytes, {size // 4} instructions)")

    if hit is None:
        print("  FAIL      no section in the symbol table contains that address")
        print()
        return 1

    name, rom, sec_vram, sec_size = hit
    offset = vram - sec_vram + rom
    print(f"  section   {name}  rom 0x{rom:08X}  vram 0x{sec_vram:08X}  size 0x{sec_size:X}")
    print(f"  OFFSET    0x{offset:X}")

    if vram + size > sec_vram + sec_size:
        print("  FAIL      the blob runs past the end of that section")
        print()
        return 1

    if args.rom:
        data = args.rom.read_bytes()
        if offset + size > len(data):
            print(f"  FAIL      offset + size is past the end of the rom ({len(data)} bytes)")
            print()
            return 1
        words = [int.from_bytes(data[offset + i * 4: offset + i * 4 + 4], "big") for i in range(4)]
        print("  first     " + " ".join(f"{w:08X}" for w in words))
        # RSP code is not all zeroes and not all the same word. Both are what a wrong offset in
        # padding looks like, and both are worth refusing rather than reporting cheerfully.
        if all(w == 0 for w in words):
            print("  FAIL      the first four words are zero, which is padding, not microcode")
            print()
            return 1
        if len(set(words)) == 1:
            print("  FAIL      the first four words are identical, which is not plausible code")
            print()
            return 1
        print("  looks     plausible: non-zero and not uniform")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
