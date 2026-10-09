"""Fail if a component stylesheet carries a value that belongs in the token file.

Phase 24's verification says "no hardcoded color, size, spacing, radius, duration, breakpoint or
z-index in any component stylesheet". That is the kind of rule everyone agrees with and nobody
keeps, because breaking it is one convenient line at a time and nothing complains. So it is
checked rather than trusted.

tokens.rcss is the one file allowed to hold literals; that is what it is for. Every other
stylesheet may only reference them through var().

    python tools/check_tokens.py assets/ui
"""

import io
import os
import re
import sys

TOKEN_FILE = "tokens.rcss"

# What counts as a value that should have been a token.
PATTERNS = [
    (re.compile(r"#[0-9A-Fa-f]{3,8}\b"), "a hex color"),
    (re.compile(r"\brgba?\s*\("), "an rgb or rgba color"),
    (re.compile(r"\bhsla?\s*\("), "an hsl color"),
    (re.compile(r"\b\d+(?:\.\d+)?(?:px|dp|em|rem|pt)\b"), "a length"),
    (re.compile(r"\b\d+(?:\.\d+)?m?s\b"), "a duration"),
    (re.compile(r"\bz-index\s*:\s*-?\d+"), "a z-index"),
]

# Values that are not design decisions and would be noise to tokenize. 0 has no units to get
# wrong, 100% and 1 are structural, and a font weight keyword is not a measurement.
ALLOWED = re.compile(r"^(0|0px|0dp|100%|1|auto|none|inherit|transparent)$", re.IGNORECASE)


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def check_file(path):
    problems = []
    raw = io.open(path, encoding="utf-8").read()
    text = strip_comments(raw)

    for number, line in enumerate(text.splitlines(), start=1):
        # A var() reference may legitimately contain a fallback, and a line that is only a var()
        # reference is exactly what this tool wants to see.
        stripped = line.strip()
        if not stripped or stripped.startswith("/*"):
            continue

        for pattern, what in PATTERNS:
            for match in pattern.finditer(stripped):
                value = match.group(0)
                if ALLOWED.match(value):
                    continue
                # Ignore anything inside a var() fallback, which is still a token reference.
                before = stripped[:match.start()]
                if before.count("var(") > before.count(")"):
                    continue
                problems.append((number, what, value, stripped))

    return problems


def main(directory):
    if not os.path.isdir(directory):
        print("check_tokens: %s is not a directory" % directory)
        return 2

    total = 0
    checked = 0
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".rcss") or name == TOKEN_FILE:
            continue
        path = os.path.join(directory, name)
        checked += 1
        problems = check_file(path)
        for number, what, value, line in problems:
            total += 1
            print("%s:%d  %s literal '%s' belongs in %s" % (name, number, what, value, TOKEN_FILE))
            print("    %s" % line)

    if checked == 0:
        print("check_tokens: no component stylesheets found in %s" % directory)
        return 1

    print()
    if total:
        print("FAIL  %d literal value(s) across %d component stylesheet(s)" % (total, checked))
        return 1
    print("PASS  %d component stylesheet(s), every value comes from %s" % (checked, TOKEN_FILE))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "assets/ui"))
