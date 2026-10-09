#!/usr/bin/env python3
"""Find function symbols whose size runs past where the next symbol begins.

WHY THIS EXISTS. When the elf gives a function a size larger than the function really is, the
recompiler decodes whatever follows it as instructions and stops hard at the first word that is not
a valid one. The message names the function, so the failure reads like a problem with that
function's code rather than with its recorded length. Fixing them one at a time means one full
recompiler run per function, and there is no reason to discover them serially when the elf already
knows every one of them.

    python tools/check_func_sizes.py --elf build/oot-ntsc-1.0.elf
    python tools/check_func_sizes.py --elf build/oot-ntsc-1.0.elf --toml   # paste-ready overrides

An overrun is a function whose address plus size passes the address of the next symbol in the same
section. The corrected size is the distance to that next symbol, which is the largest the function
could possibly be.

Note the direction of the check: a symbol that is SMALLER than the real function is not detectable
this way and does not break the build, it just recompiles less than it should. This finds the case
that actually stops the build.
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    sys.exit("pyelftools is required:  python -m pip install pyelftools")


def main() -> int:
    ap = argparse.ArgumentParser(description="Find function symbols that overrun the next symbol.")
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--toml", action="store_true", help="emit paste-ready [[input.function_sizes]]")
    args = ap.parse_args()

    if not args.elf.is_file():
        print(f"  FAIL  no such elf: {args.elf}")
        return 2

    with args.elf.open("rb") as fh:
        elf = ELFFile(fh)
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            print("  FAIL  no .symtab in this elf")
            return 2

        # Group every symbol by the section it lives in, because addresses only order meaningfully
        # within a section: two sections can occupy the same vram at different times.
        by_section = defaultdict(list)
        for sym in symtab.iter_symbols():
            info = sym["st_info"]
            shndx = sym["st_shndx"]
            if not isinstance(shndx, int):
                continue  # SHN_ABS, SHN_UNDEF and friends
            if not sym.name:
                continue
            by_section[shndx].append(
                (sym["st_value"], sym["st_size"], info["type"], sym.name)
            )

    overruns = []
    for shndx, syms in by_section.items():
        syms.sort(key=lambda s: (s[0], -s[1]))
        for i, (addr, size, stype, name) in enumerate(syms):
            if stype != "STT_FUNC" or size == 0:
                continue
            # The next symbol at a strictly higher address is the ceiling for this one.
            nxt = None
            for j in range(i + 1, len(syms)):
                if syms[j][0] > addr:
                    nxt = syms[j]
                    break
            if nxt is None:
                continue
            limit = nxt[0] - addr
            if size > limit:
                overruns.append((addr, name, size, limit, nxt[3], nxt[2]))

    overruns.sort()

    print()
    print(f"  elf        {args.elf.name}")
    print(f"  overruns   {len(overruns)}")
    print()

    if not overruns:
        print("  Every function symbol ends at or before the next symbol. Nothing to correct.")
        print()
        return 0

    if args.toml:
        for addr, name, size, limit, nxt_name, nxt_type in overruns:
            kind = "data" if nxt_type != "STT_FUNC" else "function"
            print("[[input.function_sizes]]")
            print(f"# elf says 0x{size:X}, which runs 0x{size - limit:X} bytes past {nxt_name}")
            print(f"# ({kind}) at 0x{addr + limit:08X}. 0x{limit:X} is the distance to it.")
            print(f'name = "{name}"')
            print(f"size = 0x{limit:X}")
            print()
    else:
        print(f"  {'function':<44} {'vram':>10} {'elf size':>9} {'real':>7}   next symbol")
        for addr, name, size, limit, nxt_name, nxt_type in overruns:
            print(f"  {name:<44} 0x{addr:08X} {size:>9} 0x{limit:<5X}  {nxt_name}")
        print()
        print("  Run again with --toml for paste-ready overrides.")
        print()

    return 1


if __name__ == "__main__":
    sys.exit(main())
