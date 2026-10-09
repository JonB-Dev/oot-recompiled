#!/usr/bin/env python3
"""Replace dots inside section names in an exported symbol table.

WHY THIS EXISTS, because it looks like meddling until you see the failure.

The recompiler builds C identifiers out of section names: a section called `..boot` becomes
`section_1_boot_funcs`. It strips the leading dots and does not touch the rest, so a section whose
name contains a dot produces an identifier with a dot in it:

    static FuncEntry section_0_makerom.ent_funcs[] = { ... };

which is not valid C++. The compiler reads `section_0_makerom` as a variable of type FuncEntry and
`.ent_funcs` as a member access, and reports

    error: no member named 'ent_funcs' in 'FuncEntry'

in a sixty thousand line generated file, pointing at the use rather than the declaration.

This game's decompilation has one such section, `..makerom.ent`, holding the entry point. Majora's
Mask, which the reference project recompiles, calls the equivalent section `..makerom` with no dot,
which is why the reference never hits this and why its config is no help.

The section name is a LABEL. Nothing matches on it except the recompiler's own identifier
generation: the overlay list names only `..ovl_*` sections, none of which contain dots. Renaming it
changes no address and no behavior.

    python tools/sanitize_section_names.py OoTRecompSyms/oot.ntsc-1.0.syms.toml

This is a scripted step rather than a hand edit ON PURPOSE. Phase 31 regenerates everything from
scratch to prove the pipeline reproduces, and a manual fix would silently not be part of that.
"""

import argparse
import re
import sys
from pathlib import Path

# A section name line: name = "..something". Only the part after the leading dots is touched.
NAME_LINE = re.compile(r'^(name = ")(\.*)([^"]*)(")$', re.M)


def sanitize(text: str) -> tuple[str, list[tuple[str, str]]]:
    changes: list[tuple[str, str]] = []

    def repl(m: re.Match) -> str:
        prefix, dots, body, suffix = m.groups()
        if "." not in body:
            return m.group(0)
        fixed = body.replace(".", "_")
        changes.append((dots + body, dots + fixed))
        return f"{prefix}{dots}{fixed}{suffix}"

    return NAME_LINE.sub(repl, text), changes


def main() -> int:
    ap = argparse.ArgumentParser(description="Make section names valid C identifiers.")
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--check", action="store_true", help="report and change nothing")
    args = ap.parse_args()

    total = 0
    for path in args.files:
        if not path.is_file():
            print(f"  FAIL  no such file: {path}")
            return 2

        text = path.read_text(encoding="utf-8")
        fixed, changes = sanitize(text)

        print()
        print(f"  {path.name}")
        if not changes:
            print("    no section name contains a dot")
            continue

        for before, after in changes:
            print(f"    {before}  ->  {after}")
        total += len(changes)

        if not args.check:
            path.write_text(fixed, encoding="utf-8", newline="\n")
            print(f"    rewrote {len(changes)} name(s)")

    print()
    if args.check and total:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
