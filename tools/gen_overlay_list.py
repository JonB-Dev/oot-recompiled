#!/usr/bin/env python3
"""Produce the relocatable-sections list the recompiler reads, from our own ELF.

N64Recomp needs to know which sections are overlays, because an overlay is loaded at a different
address than it was linked at and its relocations have to be applied at load time. A section that
should be on this list and is not gets its relocations silently never applied, which surfaces much
later as a crash in code that looks fine.

    python tools/gen_overlay_list.py --elf build/oot-ntsc-1.0.elf --out config/overlays.ntsc-1.0.txt
    python tools/gen_overlay_list.py --elf ... --out ... --spec upstream/oot/spec   # cross-check

WHY THE ELF AND NOT THE SPEC. The plan's commands.md section 7 derives this from the decomp's spec
files. Deriving it from the ELF instead means every name on the list is a section that provably
exists in the exact binary every other address comes from, so the "does every entry resolve"
check is satisfied by construction rather than by a second tool agreeing with the first. The spec
is still worth reading, but as a cross-check in the other direction: an overlay the spec declares
and the ELF does not contain is a real problem, and this finds it.
"""

import argparse
import re
import sys
from pathlib import Path

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    sys.exit("pyelftools is required:  python -m pip install pyelftools")

# Overlay sections are named "..ovl_<Name>", each with a matching "..ovl_<Name>.bss".
OVERLAY = re.compile(r"^\.\.ovl_[A-Za-z0-9_]+$")


def sections_from_elf(elf_path: Path) -> tuple[list[str], list[str]]:
    """Return (overlays with code, overlays that are data only).

    THE LIST IS FOR CODE OVERLAYS ONLY, and this is not a detail. The recompiler resolves each name
    here to a section it has WRITTEN, and it only writes sections that contain functions. Naming a
    data-only overlay produces "Failed to find written section index of relocatable section", which
    does not mention data and reads like the section is missing entirely.

    `..ovl_map_mark_data` is the one in this game: a real overlay, with real relocations, holding
    nothing but the minimap's mark data. Its relocations are applied by the game's own overlay
    loading code at runtime, which is why leaving it out of this list loses nothing.
    """
    with elf_path.open("rb") as fh:
        elf = ELFFile(fh)
        sections = list(elf.iter_sections())
        overlay_idx = {i: s.name for i, s in enumerate(sections) if OVERLAY.match(s.name)}

        has_code: set[str] = set()
        symtab = elf.get_section_by_name(".symtab")
        if symtab is not None:
            for sym in symtab.iter_symbols():
                shndx = sym["st_shndx"]
                if isinstance(shndx, int) and shndx in overlay_idx:
                    if sym["st_info"]["type"] == "STT_FUNC":
                        has_code.add(overlay_idx[shndx])

    every = set(overlay_idx.values())
    return sorted(has_code), sorted(every - has_code)


def overlays_from_spec(spec_dir: Path) -> set[str]:
    """Every `beginseg`/`name "ovl_X"` pair the decomp's spec declares.

    The spec is a set of .inc files included by one `spec` file. Rather than implement its include
    resolution, read every .inc in the directory: a name that appears in any of them is declared
    somewhere, which is all this cross-check needs.
    """
    found: set[str] = set()
    for f in list(spec_dir.glob("*.inc")) + [spec_dir / "spec"]:
        if not f.is_file():
            continue
        for m in re.finditer(r'name\s+"(ovl_[A-Za-z0-9_]+)"', f.read_text(errors="replace")):
            found.add(".." + m.group(1))
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the relocatable sections list from the ELF.")
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--spec", type=Path, help="decomp spec directory, for the cross-check")
    ap.add_argument("--expect", type=int, help="fail unless exactly this many overlays are found")
    args = ap.parse_args()

    if not args.elf.is_file():
        print(f"  FAIL  no such elf: {args.elf}")
        return 2

    overlays, data_only = sections_from_elf(args.elf)
    print()
    print(f"  elf       {args.elf.name}")
    print(f"  overlays  {len(overlays)} with code")
    if data_only:
        print(f"  excluded  {len(data_only)} data only, relocated by the game's own overlay loader:")
        for n in data_only:
            print(f"              {n}")

    if not overlays:
        print("  FAIL  no sections matched the overlay pattern. Wrong elf, or the naming changed.")
        return 1

    if args.spec:
        if not args.spec.is_dir():
            print(f"  FAIL  no such spec directory: {args.spec}")
            return 2
        declared = overlays_from_spec(args.spec)
        in_elf = set(overlays) | set(data_only)
        missing = sorted(declared - in_elf)
        extra = sorted(in_elf - declared)
        print(f"  spec      {len(declared)} overlays declared")
        if missing:
            print(f"  FAIL      {len(missing)} declared in the spec but absent from the elf:")
            for n in missing[:10]:
                print(f"              {n}")
            return 1
        if extra:
            # Not fatal: the ELF is the authority, and the spec's includes are version-conditional.
            print(f"  note      {len(extra)} in the elf and not matched in the spec, first few:")
            for n in extra[:5]:
                print(f"              {n}")
        else:
            print("  match     every spec-declared overlay is present in the elf")

    if args.expect is not None and len(overlays) != args.expect:
        print(f"  FAIL      expected exactly {args.expect} overlays, found {len(overlays)}")
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(overlays) + "\n", encoding="utf-8", newline="\n")
    print(f"  wrote     {args.out}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
