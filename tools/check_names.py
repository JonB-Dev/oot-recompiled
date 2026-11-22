"""The names gate: what people download never carries the game's protected names.

The user, 2026-10-13: "I definitely don't want to have any of the copyrighted stuff on the actual
produced files that people are downloading." Scans every file it is given (a folder means every
file under it): the program, its DLLs, the uninstaller, the setup file and assets/. A name is
looked for in ASCII and UTF-16 either case, and in its Japanese spelling in UTF-8 and UTF-16.

The names are kept encoded here (rot13, and code points for the Japanese) so this file, which is
public with the rest of the source, does not carry them either. Prints NAMES_OK with the count of
files, or one NAMES_FAIL line per file and name (never the surrounding bytes) and exits 1.

    python tools/check_names.py <file or folder>...
"""
import codecs
import os
import sys

# The title, its first half and the stacked triangles' name.
ENCODED = ["mryqn", "bpnevan bs gvzr", "gevsbepr"]
JAPANESE = [(0x30BC, 0x30EB, 0x30C0),
            (0x30C8, 0x30E9, 0x30A4, 0x30D5, 0x30A9, 0x30FC, 0x30B9),
            (0x6642, 0x306E, 0x30AA, 0x30AB, 0x30EA, 0x30CA)]


def needles():
    """Each name as the byte strings it could be stored as, with a label that does not spell it."""
    out = []
    for i, word in enumerate(codecs.decode(w, "rot13") for w in ENCODED):
        for form in (word.lower(), word.upper(), word.title()):
            out.append(("name %d" % (i + 1), form.encode("ascii")))
            out.append(("name %d" % (i + 1), form.encode("utf-16-le")))
    for i, points in enumerate(JAPANESE):
        word = "".join(map(chr, points))
        out.append(("name %d (Japanese)" % (i + 1), word.encode("utf-8")))
        out.append(("name %d (Japanese)" % (i + 1), word.encode("utf-16-le")))
    return out


def files(paths):
    for path in paths:
        if os.path.isdir(path):
            for root, _, names in os.walk(path):
                for name in sorted(names):
                    yield os.path.join(root, name)
        else:
            yield path


def main(argv):
    if not argv:
        print("usage: check_names.py <file or folder>...")
        return 2
    forms = needles()
    checked, problems = 0, []
    for path in files(argv):
        if not os.path.exists(path):
            problems.append("%s is missing" % path)
            continue
        data = open(path, "rb").read()
        # ASCII either case: compare against a lowered copy as well, for a name in mixed case.
        lowered = data.lower()
        checked += 1
        found = set()
        for label, needle in forms:
            if label not in found and (needle in data or needle.lower() in lowered):
                found.add(label)
        for label in sorted(found):
            problems.append("%s carries %s" % (path, label))
    for p in problems:
        print("NAMES_FAIL " + p)
    if problems:
        return 1
    print("NAMES_OK %d files" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
