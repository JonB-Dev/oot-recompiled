"""Draw the contour survey: the website's faint contour lines, redrawn by our own tool.

The site (BRAND.md, "Texture and grain") lays a survey of contour lines at low alpha in its dim
color over slate grounds, never over a picture. The menus carry the same idea on their panels
(the Medallion direction's panel, `.scaffold/menus/design/decision/`). This tool draws it from a
smooth field that repeats exactly across the drawing's width and height (a sum of waves with whole
numbers of periods), traced at evenly spaced heights by marching squares, so two copies set side by
side or one above the other meet without a seam.

RmlUi can only repeat a bitmap image, and the program's renderer reads no bitmap format but its
own, so the drawing is made at the size of the largest panel rather than as a small tile; it is an
SVG, drawn sharp at any interface size.

Seeded, so it regenerates byte for byte.

    python tools/generate_contours.py           write assets/ui/textures/contours.svg
    python tools/generate_contours.py --check   regenerate in memory; fail if the file differs or
                                                 the field does not meet itself across its edges
"""

import math
import os
import random
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "ui", "textures", "contours.svg")

W, H = 1280, 1080        # the largest panel's size in dp
STEP = 8                 # the sampling grid
LEVELS = 9               # contour heights across the field's range
SEED = 20261012
COLOR = "#a7b8b5"        # the site's --color-dim
OPACITY = "0.07"         # "at low alpha"


def field_terms():
    rng = random.Random(SEED)
    terms = []
    for _ in range(7):
        kx, ky = rng.randint(1, 3), rng.randint(-2, 2)
        terms.append((rng.uniform(0.5, 1.0), kx, ky, rng.uniform(0, 2 * math.pi)))
    return terms


def make_field(terms):
    def f(x, y):
        return sum(a * math.sin(2 * math.pi * (kx * x / W + ky * y / H) + p) for a, kx, ky, p in terms)
    return f


def trace(f):
    """Marching squares over the grid: one line segment per crossing cell, per level."""
    nx, ny = W // STEP, H // STEP
    grid = [[f(i * STEP, j * STEP) for i in range(nx + 1)] for j in range(ny + 1)]
    lo = min(min(r) for r in grid)
    hi = max(max(r) for r in grid)
    levels = [lo + (hi - lo) * (k + 1) / (LEVELS + 1) for k in range(LEVELS)]
    paths = []
    for level in levels:
        segs = []
        for j in range(ny):
            for i in range(nx):
                v = (grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i])
                pts = ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))
                cross = []
                for e in range(4):
                    a, b = v[e], v[(e + 1) % 4]
                    if (a < level) != (b < level):
                        t = (level - a) / (b - a)
                        (x0, y0), (x1, y1) = pts[e], pts[(e + 1) % 4]
                        cross.append(((x0 + (x1 - x0) * t) * STEP, (y0 + (y1 - y0) * t) * STEP))
                if len(cross) == 2:
                    segs.append(cross)
                elif len(cross) == 4:
                    segs.append([cross[0], cross[1]])
                    segs.append([cross[2], cross[3]])
        paths.append("".join("M%.1f %.1fL%.1f %.1f" % (a[0], a[1], b[0], b[1]) for a, b in segs))
    return paths


def drawing():
    paths = trace(make_field(field_terms()))
    body = "".join('<path d="%s"/>' % d for d in paths if d)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
            '<g fill="none" stroke="%s" stroke-opacity="%s" stroke-width="1">%s</g></svg>\n') % (
        W, H, W, H, COLOR, OPACITY, body)


def seamless():
    f = make_field(field_terms())
    worst = 0.0
    for k in range(64):
        y, x = H * k / 64, W * k / 64
        worst = max(worst, abs(f(0, y) - f(W, y)), abs(f(x, 0) - f(x, H)))
    return worst < 1e-9


def main(argv):
    text = drawing()
    if "--check" in argv:
        if not seamless():
            print("CONTOURS_FAIL the field does not meet itself across its edges")
            return 1
        if not os.path.exists(OUT) or open(OUT, encoding="utf-8").read() != text:
            print("CONTOURS_FAIL assets/ui/textures/contours.svg differs from what the tool draws")
            return 1
        print("CONTOURS_OK")
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote assets/ui/textures/contours.svg (%d bytes)" % len(text))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
