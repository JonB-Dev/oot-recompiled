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
    # A comment's newlines are kept, so every line number reported is the file's own line.
    return re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)


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


STYLE_BLOCK = re.compile(r"<style\b[^>]*>(.*?)</style>", re.DOTALL | re.IGNORECASE)
STYLE_ATTR = re.compile(r"\bstyle\s*=\s*\"([^\"]*)\"", re.IGNORECASE)


def check_text(text):
    """The same rules as a stylesheet, for CSS that lives inside a document."""
    problems = []
    for number, line in enumerate(strip_comments(text).splitlines(), start=1):
        stripped = line.strip()
        if not stripped:
            continue
        for pattern, what in PATTERNS:
            for match in pattern.finditer(stripped):
                value = match.group(0)
                if ALLOWED.match(value):
                    continue
                before = stripped[:match.start()]
                if before.count("var(") > before.count(")"):
                    continue
                problems.append((number, what, value, stripped))
    return problems


def check_document(path):
    """An .rml document's own <style> blocks and style="" attributes, at the document's lines.

    The stylesheets were checked and the documents were not, so a literal written inline (the
    letterbox's black, 2026-10-12) slipped past a rule that claimed to cover everything."""
    raw = io.open(path, encoding="utf-8").read()
    problems = []
    for block in STYLE_BLOCK.finditer(raw):
        offset = raw.count("\n", 0, block.start(1))
        for number, what, value, line in check_text(block.group(1)):
            problems.append((offset + number, what, value, line))
    for attr in STYLE_ATTR.finditer(raw):
        number = raw.count("\n", 0, attr.start()) + 1
        for _, what, value, line in check_text(attr.group(1)):
            problems.append((number, what, value, line))
    return problems


REFERENCE = re.compile(r"""(?:\bsrc\s*=\s*"([^"]*)"|\bsvg\(\s*([^)\s]+))""")


def outside_assets(directory, path):
    """A document or stylesheet may name an image only inside the install's assets/ (the menus
    plan, security/app-layer.md section 3): never an absolute path, a drive, or a climb out."""
    assets = os.path.abspath(os.path.join(directory, os.pardir))
    problems = []
    raw = io.open(path, encoding="utf-8").read()
    for match in REFERENCE.finditer(raw):
        ref = match.group(1) or match.group(2)
        if not ref or ref.startswith("{") or ref.startswith("#"):
            continue
        number = raw.count("\n", 0, match.start()) + 1
        target = os.path.abspath(os.path.join(directory, ref))
        if ":" in ref or ref.startswith("/") or not target.startswith(assets + os.sep):
            problems.append((number, "a reference outside assets/", ref, match.group(0)))
    return problems


def main(directory):
    if not os.path.isdir(directory):
        print("check_tokens: %s is not a directory" % directory)
        return 2

    total = 0
    checked = 0
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        # A stylesheet a tool generates from a decision's numbers (medallion.rcss) is checked by
        # that tool's own --check, which compares it with the numbers; skipped here.
        if name.endswith(".rcss") and io.open(path, encoding="utf-8").read(32).startswith("/* GENERATED by tools/"):
            continue
        if name.endswith(".rcss") and name != TOKEN_FILE:
            problems = check_file(path)
        elif name.endswith(".rml"):
            problems = check_document(path)
        else:
            continue
        problems += outside_assets(directory, path)
        checked += 1
        for number, what, value, line in problems:
            total += 1
            if what.startswith("a reference"):
                print("TOKENS_FAIL %s:%d %s: '%s'" % (name, number, what, value))
            else:
                print("TOKENS_FAIL %s:%d %s literal '%s' belongs in %s" % (name, number, what, value, TOKEN_FILE))
            print("    %s" % line)

    if checked == 0:
        print("check_tokens: no stylesheets or documents found in %s" % directory)
        return 1

    if total:
        print("TOKENS_FAIL %d literal value(s) across %d file(s)" % (total, checked))
        return 1
    print("TOKENS_OK %d files" % checked)
    return 0


def licenses(assets):
    """The bundle's provenance gate (the menus plan, phase 85). Every line of
    assets/licenses/MANIFEST.txt names a file that exists with exactly that SHA-256, and a license
    text that exists; every file under assets/fonts/ and assets/icons/ has a line. A byte changed in
    a bundled file, or a file dropped in without its source and license, fails the build."""
    import hashlib
    manifest = os.path.join(assets, "licenses", "MANIFEST.txt")
    if not os.path.exists(manifest):
        print("LICENSES_FAIL %s is missing" % manifest)
        return 1
    listed, problems = set(), []
    for number, line in enumerate(io.open(manifest, encoding="utf-8").read().splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != 6:
            problems.append("MANIFEST.txt:%d has %d columns, not 6" % (number, len(cols)))
            continue
        path, _source, _version, license_field, sha, _check = cols
        listed.add(path)
        full = os.path.join(assets, path)
        if not os.path.exists(full):
            problems.append("%s is listed and missing" % path)
            continue
        digest = hashlib.sha256(open(full, "rb").read()).hexdigest()
        if digest != sha:
            problems.append("%s hash %s, the manifest says %s" % (path, digest, sha))
        for word in license_field.split():
            if word.startswith("licenses/") and not os.path.exists(os.path.join(assets, word)):
                problems.append("%s names %s, which is missing" % (path, word))
    for folder in ("fonts", "icons"):
        root = os.path.join(assets, folder)
        for name in (sorted(os.listdir(root)) if os.path.isdir(root) else []):
            if "%s/%s" % (folder, name) not in listed:
                problems.append("%s/%s is bundled and not in the manifest" % (folder, name))
    for p in problems:
        print("LICENSES_FAIL " + p)
    if problems:
        return 1
    print("LICENSES_OK %d files" % len(listed))
    return 0


def unresolved(directory):
    """Every var(--name) in a stylesheet or a document must name a custom property defined
    somewhere in the interface (tokens.rcss, or a component that sets one for its children). A
    reference to nothing is not an error RmlUi reports loudly: the property simply falls back, and
    the rule silently does nothing (the menus plan, phase 89, when the old tokens were renamed)."""
    defined, used = set(), []
    decl = re.compile(r"--([a-z0-9-]+)\s*:")
    ref = re.compile(r"var\(\s*--([a-z0-9-]+)")
    for name in sorted(os.listdir(directory)):
        if not (name.endswith(".rcss") or name.endswith(".rml")):
            continue
        text = strip_comments(io.open(os.path.join(directory, name), encoding="utf-8").read())
        defined.update(decl.findall(text))
        for number, line in enumerate(text.splitlines(), start=1):
            for m in ref.finditer(line):
                used.append((name, number, m.group(1)))
    missing = [(n, l, v) for n, l, v in used if v not in defined]
    for n, l, v in missing:
        print("UNRESOLVED_FAIL %s:%d var(--%s) is defined nowhere" % (n, l, v))
    if missing:
        return 1
    print("UNRESOLVED_OK %d references" % len(used))
    return 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--unresolved" in sys.argv:
        sys.exit(unresolved(args[0] if args else "assets/ui"))
    if "--licenses" in sys.argv:
        sys.exit(licenses(args[0] if args else "assets"))
    sys.exit(main(args[0] if args else "assets/ui"))
