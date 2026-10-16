"""Draw the Leaf fan medallion's layers, from the design decision's own numbers.

The decision (.scaffold/menus/design/medallion/DESIGN-DECISION.md, design canvas b3018d5b, option
A, "exactly as first drawn") is one SVG on the canvas. The program's renderer rasterizes an SVG as
ONE image, so a medallion whose leaves breathe when it is selected has to be built from layers:
each leaf its own image, rotated about its own base by RCSS. This tool writes those layers, every
one on the same 200 by 190 canvas so they stack exactly, and the RCSS that places and animates
them. The numbers below are the canvas's, copied from its source (source/medallions.mjs, medA,
FAN, goldDefs, goldRim); change them there and here together, never here alone.

Shadows are shapes, never blur: the renderer has no filters.

    python tools/medallion.py           write assets/ui/medallion/ and assets/ui/medallion.rcss
    python tools/medallion.py --check   regenerate in memory and fail if anything on disk differs
"""

import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "ui", "medallion")
RCSS = os.path.join(ROOT, "assets", "ui", "medallion.rcss")

W, H = 200, 190
CX, CY = 100, 90

# The fan, per side, in draw order: the outer pair behind, the inner pair, the center in front.
# th: degrees clockwise from straight up, around the center; r: the base's distance from it;
# s: size; phi: the leaf's own rotation, clockwise from pointing up. The left side mirrors.
FAN = [
    {"th": 70, "r": 56, "s": 0.62, "phi": 6, "cls": "o1"},
    {"th": 148, "r": 56, "s": 0.62, "phi": 106, "cls": "o2"},
    {"th": 90, "r": 50, "s": 0.82, "phi": 21, "cls": "i1"},
    {"th": 129, "r": 50, "s": 0.82, "phi": 58, "cls": "i2"},
    {"th": 109, "r": 46, "s": 1.0, "phi": 37, "cls": "c"},
]
LEAF_LENGTH, LEAF_WIDTH = 52, 15
# When selected: the outer pair first, the inner 120 ms later, the center 240 ms later.
# Each leaf's start in the fan, as the motion tokens name them (tokens.rcss: --fan-step and
# --fan-step-2, 0.12 s and 0.24 s), so the stylesheet carries no literal duration (phase 114).
DELAY = {"o1": "", "o2": "", "i1": " var(--fan-step)", "i2": " var(--fan-step)", "c": " var(--fan-step-2)"}


def f(n):
    """The canvas's rounding: to a tenth, half up, printed like JavaScript prints a number."""
    v = math.floor(n * 10 + 0.5) / 10
    return str(int(v)) if v == int(v) else ("%.1f" % v)


def at(r, th):
    return CX + r * math.sin(math.radians(th)), CY - r * math.cos(math.radians(th))


def leaf_d(L, w):
    return "M0 0C%s %s %s %s 0 %s C%s %s %s %s 0 0Z" % (
        f(-w), f(-L * .26), f(-w * .92), f(-L * .72), f(-L),
        f(w * .92), f(-L * .72), f(w), f(-L * .26))


def svg(body, defs=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
            '%s%s</svg>\n') % (W, H, W, H, ("<defs>%s</defs>" % defs) if defs else "", body)


LEAF_GRADIENT = ('<linearGradient id="leaf" x1="0" y1="0" x2="1" y2="0">'
                 '<stop offset="0" stop-color="#8fd486"/><stop offset=".48" stop-color="#4c9a58"/>'
                 '<stop offset=".52" stop-color="#3a8048"/><stop offset="1" stop-color="#1f4f2b"/></linearGradient>')
RIM_GRADIENTS = {
    "gold": ('<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fbe9a6"/>'
             '<stop offset=".42" stop-color="#d6b03a"/><stop offset=".75" stop-color="#a07c1b"/>'
             '<stop offset="1" stop-color="#5e440b"/></linearGradient>'),
    "lime": ('<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f1ffc4"/>'
             '<stop offset=".45" stop-color="#c6ff3d"/><stop offset="1" stop-color="#5f8a12"/></linearGradient>'),
}
INNER_RING = ('<linearGradient id="goldIn" x1="0" y1="1" x2="0" y2="0"><stop offset="0" stop-color="#f2d675"/>'
              '<stop offset="1" stop-color="#6b4f10"/></linearGradient>')
DISC = ('<radialGradient id="disc" cx=".42" cy=".3" r=".8"><stop offset="0" stop-color="#2b3e45"/>'
        '<stop offset=".6" stop-color="#172126"/><stop offset="1" stop-color="#0a0f11"/></radialGradient>')
GROUND = ('<radialGradient id="ground" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#000" stop-opacity=".55"/>'
          '<stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>')


def leaves(side):
    """Each leaf of one side: (name, base x, base y, its layer's SVG)."""
    out = []
    for l in FAN:
        th = l["th"] if side > 0 else 360 - l["th"]
        phi = side * l["phi"]
        x, y = at(l["r"], th)
        x, y = float(f(x)), float(f(y))
        L, w = LEAF_LENGTH * l["s"], LEAF_WIDTH * l["s"]
        body = ('<g transform="translate(%s %s) rotate(%s)">'
                '<path d="%s" fill="url(#leaf)" stroke="#1d4426" stroke-width=".9"/>'
                '<path d="M0 %sQ%s %s 0 %s" fill="none" stroke="#b9e6a0" stroke-opacity=".75" stroke-width="1.1" stroke-linecap="round"/>'
                '<path d="M%s %sQ%s %s %s %s" fill="none" stroke="#e4ffd0" stroke-opacity=".35" stroke-width="1.4" stroke-linecap="round"/>'
                '</g>') % (
            f(x), f(y), f(phi), leaf_d(L, w),
            f(-L * .06), f(w * .14), f(-L * .5), f(-L * .9),
            f(-w * .55), f(-L * .3), f(-w * .7), f(-L * .6), f(-w * .15), f(-L * .88))
        name = "leaf-%s-%s" % ("r" if side > 0 else "l", l["cls"])
        out.append((name, l["cls"], x, y, svg(body, LEAF_GRADIENT)))
    return out


def shadows():
    parts = []
    for side in (1, -1):
        for l in FAN:
            th = l["th"] if side > 0 else 360 - l["th"]
            x, y = at(l["r"], th)
            x, y = float(f(x)), float(f(y))
            L, w = LEAF_LENGTH * l["s"], LEAF_WIDTH * l["s"]
            parts.append('<path transform="translate(%s %s) rotate(%s)" d="%s"/>' % (
                f(x + 2.5), f(y + 4.5), f(side * l["phi"]), leaf_d(L, w)))
    ground = '<ellipse cx="%d" cy="168" rx="75" ry="9" fill="url(#ground)"/>' % CX
    return svg(ground + '<g fill="#000" opacity=".34">%s</g>' % "".join(parts), GROUND)


def rim(kind, r=62, inner=53):
    body = (
        '<circle cx="%d" cy="%d" r="%d" fill="#000" opacity=".38"/>' % (CX + 2, CY + 5, r) +
        '<circle cx="%d" cy="%d" r="%d" fill="url(#rim)"/>' % (CX, CY, r) +
        '<circle cx="%d" cy="%d" r="%d" fill="url(#goldIn)"/>' % (CX, CY, r - 4) +
        '<circle cx="%d" cy="%d" r="%s" fill="#3d2c06" opacity=".7"/>' % (CX, CY, f(inner + 1.5)) +
        '<circle cx="%d" cy="%d" r="%d" fill="url(#disc)"/>' % (CX, CY, inner) +
        '<path d="M%s %s A%d %d 0 0 1 %s %s" fill="none" stroke="#fff" stroke-opacity=".12" stroke-width="3" stroke-linecap="round"/>' % (
            f(CX - inner + 6), f(CY - 18), inner - 4, inner - 4, f(CX + 14), f(CY - inner + 6)) +
        '<path d="M%s %s A%d %d 0 0 1 %s %s" fill="none" stroke="#fffbe6" stroke-opacity=".7" stroke-width="2.2" stroke-linecap="round"/>' % (
            f(CX - r + 5), f(CY - 14), r - 3, r - 3, f(CX + 10), f(CY - r + 3)))
    return svg(body, RIM_GRADIENTS[kind] + INNER_RING + DISC)


def glint():
    return svg('<path d="M%s %s A61 61 0 0 1 %s %s" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/>' % (
        f(CX + 30), f(CY - 53), f(CX + 56), f(CY - 25)))


def pct(v, total):
    return "%s%%" % f(v / total * 100)


def rcss(all_leaves):
    """Where each layer sits and how a selected medallion moves. Generated with the layers, so a
    leaf's pivot can never drift from where its layer draws it."""
    lines = [
        "/* GENERATED by tools/medallion.py from the design decision's numbers. Do not edit by hand:",
        " * change the tool and run it. The Leaf fan medallion as stacked layers on one 200 by 190",
        " * canvas: the ground and leaf shadows, ten leaves, the rim (gold, or lime when selected), the",
        " * glint, the glyph. Selected (.is-f on any ancestor), each side's fan breathes open and a glint",
        " * runs round the rim (design/medallion/DESIGN-DECISION.md). */",
        "",
        ".md { position: relative; display: inline-block; width: var(--md-size); height: var(--md-height); }",
        ".md .ly { position: absolute; left: 0; top: 0; width: 100%; height: 100%; }",
        ".md .ly.shadows { decorator: svg(medallion/shadows.svg); }",
        ".md .ly.rim { decorator: svg(medallion/rim-gold.svg); }",
        ".is-f .md .ly.rim, .md.is-f .ly.rim { decorator: svg(medallion/rim-lime.svg); }",
        ".md .ly.glint { decorator: svg(medallion/glint.svg); opacity: 0; transform-origin: 50%% %s; }" % pct(CY, H),
        ".md .glyph { position: absolute; left: 38.6%; top: 35.5%; width: 22.8%; height: 24%; image-color: var(--md-glyph); }",
        ".is-f .md .glyph, .md.is-f .glyph { image-color: var(--md-glyph-on); }",
    ]
    for name, cls, x, y, _ in all_leaves:
        side = "r" if "-r-" in name else "l"
        lines.append(".md .ly.%s { decorator: svg(medallion/%s.svg); transform-origin: %s %s; }" % (
            name, name, pct(x, W), pct(y, H)))
    for name, cls, x, y, _ in all_leaves:
        side = "r" if "-r-" in name else "l"
        lines.append(".is-f .md .ly.%s, .md.is-f .ly.%s { animation: var(--fan-cycle)%s var(--ease) infinite fan-%s; }" % (
            name, name, DELAY[cls], side))
    lines += [
        ".is-f .md .ly.glint, .md.is-f .ly.glint { animation: var(--glint-cycle) var(--ease) infinite glint; }",
        # A document fading in or out (ui_shell.cpp, phase 114): the glint's own opacity would
        # ignore the fade, so it is hidden for its length.
        ".is-fading .md .ly.glint { visibility: hidden; }",
        "@keyframes fan-r { 0% { transform: rotate(0deg); } 50% { transform: rotate(5deg); } 100% { transform: rotate(0deg); } }",
        "@keyframes fan-l { 0% { transform: rotate(0deg); } 50% { transform: rotate(-5deg); } 100% { transform: rotate(0deg); } }",
        "@keyframes glint { 0% { opacity: 0; transform: rotate(-40deg); } 30% { opacity: 0.85; } 60% { opacity: 0.85; } 100% { opacity: 0; transform: rotate(250deg); } }",
    ]
    return "\n".join(lines) + "\n"


def files():
    out = {}
    all_leaves = leaves(-1) + leaves(1)   # the left side drawn first, as on the canvas
    out["shadows.svg"] = shadows()
    for name, cls, x, y, body in all_leaves:
        out[name + ".svg"] = body
    out["rim-gold.svg"] = rim("gold")
    out["rim-lime.svg"] = rim("lime")
    out["glint.svg"] = glint()
    return out, rcss(all_leaves), all_leaves


def main(argv):
    out, sheet, _ = files()
    if "--check" in argv:
        bad = []
        for name, body in out.items():
            p = os.path.join(OUT_DIR, name)
            if not os.path.exists(p) or open(p, encoding="utf-8").read() != body:
                bad.append("medallion/" + name)
        if not os.path.exists(RCSS) or open(RCSS, encoding="utf-8").read() != sheet:
            bad.append("medallion.rcss")
        if bad:
            print("MEDALLION_FAIL differs from the decision's numbers: " + ", ".join(bad))
            return 1
        print("MEDALLION_OK %d layers" % len(out))
        return 0
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, body in out.items():
        with open(os.path.join(OUT_DIR, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body)
    with open(RCSS, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sheet)
    print("wrote %d layers to assets/ui/medallion/ and assets/ui/medallion.rcss" % len(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
