#!/usr/bin/env python3
"""Read an RSP microcode's dispatch table out of its own DMEM data blob.

WHY. RSPRecomp resolves the jump targets it can see statically. An audio microcode dispatches its
commands through a table of halfword IMEM addresses held in DMEM, and those targets are invisible
to static analysis: the recompiler emits a switch over the targets it found and a default that
prints "Unhandled jump target" AT RUNTIME. So a missing entry is not a build error, it is silence
until the game plays a sound that happens to use that command.

The reference project's list of extra targets belongs to a different build of the microcode (its
text is 0x1000 bytes where ours is 0xFB0), so its numbers are all wrong here, and wrong in the
worst way: plausible offsets that would branch into the middle of unrelated instructions.

    python tools/extract_ucode_jumptable.py --rom build/oot.ntsc-1.0.rom_uncompressed.z64 \
        --data-offset 0xB8A240 --table-offset 0x10 --imem-base 0x1000 --imem-size 0xFB0

The dispatch in our aspMain is:

    srl  $1, $26, 23      take the command byte from the top of the command word
    andi $1, $1, 0xFE     mask to an even halfword index
    add  $2, $zero, $1
    lh   $2, 0x10($2)     load a halfword from DMEM at 0x10 + index
    jr   $2

so the table starts at DMEM 0x10 and holds one halfword per command.
"""

import argparse
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract an RSP microcode dispatch table.")
    ap.add_argument("--rom", required=True, type=Path)
    ap.add_argument("--data-offset", required=True, help="rom offset of the microcode's data blob")
    ap.add_argument("--table-offset", default="0x10", help="offset of the table within DMEM")
    ap.add_argument("--imem-base", default="0x1000", help="lowest valid IMEM address")
    ap.add_argument("--imem-size", required=True, help="length of the microcode text")
    ap.add_argument("--max-entries", type=int, default=64)
    args = ap.parse_args()

    data_off = int(args.data_offset, 0)
    tbl_off = int(args.table_offset, 0)
    base = int(args.imem_base, 0)
    size = int(args.imem_size, 0)
    top = base + size

    rom = args.rom.read_bytes()
    start = data_off + tbl_off

    print()
    print(f"  rom offset of table   0x{start:X}")
    print(f"  valid IMEM range      0x{base:X} to 0x{top:X}")
    print()

    good, rejected = [], []
    for i in range(args.max_entries):
        at = start + i * 2
        if at + 2 > len(rom):
            break
        val = int.from_bytes(rom[at:at + 2], "big")
        cmd = i
        if base <= val < top and val % 4 == 0:
            good.append((cmd, val))
        else:
            rejected.append((cmd, val))

    # The table ends where the entries stop looking like IMEM addresses. Report the run of valid
    # ones from the start, and show what came after it so the boundary is a judgment the reader
    # can check rather than one this script made quietly.
    run = []
    for cmd, val in good:
        if run and cmd != run[-1][0] + 1:
            break
        run.append((cmd, val))

    print(f"  {len(run)} consecutive valid entries from command 0:")
    for cmd, val in run:
        print(f"    cmd 0x{cmd:02X}   ->  0x{val:04X}")

    after = [(c, v) for c, v in rejected if run and c == run[-1][0] + 1]
    if after:
        c, v = after[0]
        print(f"\n  first entry that is NOT a valid IMEM address: cmd 0x{c:02X} = 0x{v:04X}")
        print("  which is where the table is taken to end.")

    uniq = sorted({v for _, v in run})
    print(f"\n  {len(uniq)} distinct targets:")
    print("extra_indirect_branch_targets = [")
    for i in range(0, len(uniq), 8):
        row = ", ".join(f"0x{v:04X}" for v in uniq[i:i + 8])
        print(f"    {row},")
    print("]")
    print()
    return 0 if run else 1


if __name__ == "__main__":
    sys.exit(main())
