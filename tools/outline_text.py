"""Turns runs of text into SVG outlines, shaped by HarfBuzz the way a browser shapes them, so the
exported SVGs need no fonts. Reads a JSON list of runs on stdin, writes a JSON object on stdout.

A run: {"id", "font", "axes": {"wght": 800, ...}, "size", "text", "spacing" (px after every
character, as CSS letter-spacing), "x", "y" (the baseline), "anchor": "start" | "middle" | "end"}.
Out, per id: {"d", "x0", "x1", "top", "bottom"}: the path, the run's advance from x0 to x1, and its
line cell (ascent to descent) for gradients that a browser would size to the text's box.
"""
import io
import json
import sys

import uharfbuzz as hb
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

_cache = {}


def instance(path, axes):
    key = (path, tuple(sorted(axes.items())))
    if key not in _cache:
        font = TTFont(path)
        if "fvar" in font:
            known = {a.axisTag for a in font["fvar"].axes}
            font = instancer.instantiateVariableFont(font, {k: v for k, v in axes.items() if k in known})
        buf = io.BytesIO()
        font.save(buf)
        data = buf.getvalue()
        _cache[key] = (TTFont(io.BytesIO(data)), hb.Font(hb.Face(data)))
    return _cache[key]


def outline(run):
    font, hbfont = instance(run["font"], run.get("axes", {}))
    upem = font["head"].unitsPerEm
    scale = run["size"] / upem
    buf = hb.Buffer()
    buf.add_str(run["text"])
    buf.guess_segment_properties()
    hb.shape(hbfont, buf, {"kern": True, "liga": True})
    spacing = run.get("spacing", 0)
    total = sum(p.x_advance for p in buf.glyph_positions) * scale + spacing * len(buf.glyph_infos)
    anchor = run.get("anchor", "start")
    x0 = run["x"] - (total / 2 if anchor == "middle" else total if anchor == "end" else 0)
    glyphs = font.getGlyphSet()
    order = font.getGlyphOrder()
    pen = SVGPathPen(glyphs, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip("."))
    pen_x = x0
    for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
        name = order[info.codepoint]
        gx = pen_x + pos.x_offset * scale
        gy = run["y"] - pos.y_offset * scale
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, gx, gy)))
        pen_x += pos.x_advance * scale + spacing
    hhea = font["hhea"]
    return {
        "d": pen.getCommands(),
        "x0": round(x0, 2),
        "x1": round(x0 + total, 2),
        "top": round(run["y"] - hhea.ascent * scale, 2),
        "bottom": round(run["y"] - hhea.descent * scale, 2),
    }


if __name__ == "__main__":
    runs = json.load(sys.stdin)
    json.dump({r["id"]: outline(r) for r in runs}, sys.stdout)
