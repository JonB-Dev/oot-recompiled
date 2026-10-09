"""Inventory every see-through draw in every room of the game, batched the way RT64 batches them.

Run inside WSL from the decomp root:
    python3 inventory_cutouts.py <decomp root> <out dir>

For each room display list (solid and see-through lists both), walks the commands with the
renderer's draw-call rule (a draw ends at any state command; vertex loads do not end one), and for
every draw in the translucent z mode records: its triangle count, depth flags, texture, whether the
texture's alpha is a cut-out (only fully clear and fully solid texels) or a fade (partial alpha),
the combiner's alpha inputs, which way its faces point, and where it is. A draw the current rule
keeps OUT of the traced scene is one in the translucent z mode with two triangles or fewer (or no
depth compare).

Reads extracted, ROM-derived data locally and writes only names, counts and numbers.
"""
import csv
import os
import re
import struct
import subprocess
import sys
import zlib
from collections import defaultdict

ROOT = sys.argv[1]
OUT = sys.argv[2]
SCENES = os.path.join(ROOT, "extracted/ntsc-1.0/assets/scenes")

Z_CMP, Z_UPD, ZMODE_MASK, ZMODE_XLU, ZMODE_DEC = 0x10, 0x20, 0xC00, 0x800, 0xC00
CVG_X_ALPHA, ALPHA_CVG_SEL, FORCE_BL = 0x1000, 0x2000, 0x4000

# Commands that do not end a draw.
NON_STATE = {"gsSPVertex", "gsSP1Triangle", "gsSP2Triangles", "gsSP1Quadrangle", "gsDPPipeSync",
             "gsDPTileSync", "gsDPLoadSync", "gsDPFullSync", "gsSPCullDisplayList",
             "gsSPEndDisplayList", "gsSPDisplayList", "gsSPBranchList", "gsDPNoOp", "gsSPNoOp"}


def parse_commands(text):
    """Yield (name, [args]) for each gs* macro, with balanced parentheses."""
    i = 0
    n = len(text)
    while True:
        m = re.compile(r"\b(gs[A-Z]\w*)\s*\(").search(text, i)
        if not m:
            return
        name = m.group(1)
        j = m.end()
        depth = 1
        start = j
        while j < n and depth:
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
            j += 1
        body = text[start:j - 1]
        args, depth, cur = [], 0, ""
        for ch in body:
            if ch == "," and depth == 0:
                args.append(cur.strip())
                cur = ""
                continue
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            cur += ch
        if cur.strip():
            args.append(cur.strip())
        yield name, args
        i = j


# ---- render mode values through the decomp's own gbi.h -------------------------------------
_rm_cache = {}


def eval_flags(exprs):
    todo = [e for e in exprs if e not in _rm_cache]
    if not todo:
        return
    src = "#include \"ultra64/gbi.h\"\n" + "".join(f"@@{k}@@ {e}\n" for k, e in enumerate(todo))
    out = subprocess.run(["cpp", "-P", "-I", os.path.join(ROOT, "include"), "-I",
                          os.path.join(ROOT, "include/ultra64"), "-I", ROOT, "-"],
                         input=src, capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"@@(\d+)@@\s*(.*)", line)
        if not m:
            continue
        expr = re.sub(r"\((?:unsigned\s+)?(?:int|u32|long)\)", "", m.group(2))
        try:
            _rm_cache[todo[int(m.group(1))]] = int(eval(expr)) & 0xFFFFFFFF
        except Exception:
            _rm_cache[todo[int(m.group(1))]] = None


# ---- PNG alpha, without a library ----------------------------------------------------------
def png_alpha_kind(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return "missing"
    pos, chunks = 8, {}
    idat = b""
    while pos < len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IDAT":
            idat += body
        else:
            chunks.setdefault(ctype, body)
        pos += 12 + length
    w, h, depth, ctype = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    raw = zlib.decompress(idat)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp_bits = channels * depth
    stride = (w * bpp_bits + 7) // 8
    bpp = max(1, bpp_bits // 8)
    rows, prev, k = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[k]
        line = bytearray(raw[k + 1:k + 1 + stride])
        k += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    alphas = set()
    if ctype == 6 and depth == 8:
        for line in rows:
            alphas.update(line[3::4])
    elif ctype == 4 and depth == 8:
        for line in rows:
            alphas.update(line[1::2])
    elif ctype == 3:
        trns = chunks.get(b"tRNS", b"")
        used = set()
        for line in rows:
            if depth == 8:
                used.update(line)
            else:
                per = 8 // depth
                for byte in line:
                    for s in range(per):
                        used.add((byte >> (8 - depth * (s + 1))) & ((1 << depth) - 1))
        alphas = {trns[i] if i < len(trns) else 255 for i in used}
    else:
        return "opaque"
    if alphas <= {255}:
        return "opaque"
    if alphas <= {0, 255}:
        return "cutout"
    partial = sum(1 for a in alphas if 0 < a < 255)
    return "fade" if partial > 2 else "cutout~"


# ---- the walk ------------------------------------------------------------------------------
rows_out = []


def walk_scene(scene_dir, scene):
    files = {f[:-6]: os.path.join(scene_dir, f) for f in os.listdir(scene_dir) if f.endswith(".inc.c")}
    pngs = [f for f in os.listdir(scene_dir) if f.endswith(".png")]
    vtx_cache = {}

    def vertices(name):
        if name not in vtx_cache:
            text = open(files[name]).read() if name in files else ""
            vtx_cache[name] = [tuple(map(int, m)) for m in re.findall(r"VTX\(\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)", text)]
        return vtx_cache[name]

    lists = []
    for name, path in files.items():
        if "Entries" not in name:
            continue
        for m in re.finditer(r"(\w+),\s*//\s*(opa|xlu)", open(path).read()):
            if m.group(1) != "NULL":
                lists.append((m.group(1), m.group(2)))

    for dl, kind in lists:
        room = re.match(r"(\w+?_room_\d+|\w+?_scene)", dl)
        room = room.group(1) if room else dl
        state = {"rm": "G_RM_FOG_SHADE_A | G_RM_AA_ZB_OPA_SURF2", "tex": "", "fmt": "", "comb": "",
                 "prim_a": 255}
        slots = [None] * 64
        run = {"tris": []}

        def end_run():
            if run["tris"]:
                rows_out.append(make_row(scene, room, kind, dl, dict(state), run["tris"], pngs))
            run["tris"] = []

        def walk(name, depth=0):
            if name not in files or depth > 8:
                return
            text = open(files[name]).read()
            for cmd, args in parse_commands(text):
                if cmd in ("gsSPDisplayList", "gsSPBranchList"):
                    walk(args[0].lstrip("&"), depth + 1)
                    if cmd == "gsSPBranchList":
                        return
                    continue
                if cmd == "gsSPEndDisplayList":
                    return
                if cmd == "gsSPVertex":
                    m = re.match(r"&?(\w+)(?:\[(\d+)\])?", args[0])
                    arr, start = m.group(1), int(m.group(2) or 0)
                    count, dst = int(args[1], 0), int(args[2], 0)
                    vs = vertices(arr)
                    for k in range(count):
                        slots[dst + k] = vs[start + k] if start + k < len(vs) else None
                    continue
                if cmd in ("gsSP1Triangle", "gsSP2Triangles", "gsSP1Quadrangle"):
                    idx = [int(a, 0) for a in args]
                    tris = []
                    if cmd == "gsSP1Triangle":
                        tris = [idx[0:3]]
                    elif cmd == "gsSP2Triangles":
                        tris = [idx[0:3], idx[4:7]]
                    else:
                        tris = [[idx[0], idx[1], idx[2]], [idx[0], idx[2], idx[3]]]
                    for t in tris:
                        pts = [slots[v] for v in t]
                        if all(p is not None for p in pts):
                            run["tris"].append(pts)
                    continue
                if cmd in NON_STATE:
                    continue
                # A state command: the draw ends here.
                end_run()
                if cmd == "gsDPSetRenderMode":
                    state["rm"] = " | ".join(args)
                elif cmd == "gsDPSetOtherMode" and len(args) == 2:
                    state["rm"] = args[1]
                elif cmd.startswith("gsDPLoadTextureBlock") or cmd.startswith("gsDPLoadMultiBlock") \
                        or cmd.startswith("gsDPLoadTextureTile") or cmd == "gsDPSetTextureImage":
                    texname = next((a for a in args if re.search(r"Tex", a)), args[0] if args else "")
                    state["tex"] = texname.lstrip("&")
                    state["fmt"] = next((a.replace("G_IM_FMT_", "") for a in args if a.startswith("G_IM_FMT_")), "")
                elif cmd in ("gsDPSetCombineLERP", "gsDPSetCombineMode"):
                    state["comb"] = ",".join(args)
                elif cmd == "gsDPSetPrimColor" and len(args) >= 6:
                    try:
                        state["prim_a"] = int(args[5], 0)
                    except ValueError:
                        pass
            return

        walk(dl)
        end_run()


def make_row(scene, room, kind, dl, state, tris, pngs):
    normals = []
    cx = cy = cz = 0.0
    for a, b, c in tris:
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
        normals.append(abs(ny / ln))
        cx += (a[0] + b[0] + c[0]) / 3
        cy += (a[1] + b[1] + c[1]) / 3
        cz += (a[2] + b[2] + c[2]) / 3
    count = len(tris)
    walls = sum(1 for n in normals if n < 0.5)
    floors = sum(1 for n in normals if n > 0.85)
    facing = "wall" if walls == count else "floor" if floors == count else "mixed"
    tex = state["tex"]
    png = next((p for p in pngs if p.startswith(tex + ".")), None) if tex else None
    return {
        "scene": scene, "room": room, "list": kind, "dl": dl[-40:], "tris": count, "rm": state["rm"],
        "tex": tex, "fmt": state["fmt"], "png": png or "", "comb": state["comb"],
        "prim_a": state["prim_a"], "facing": facing,
        "x": round(cx / count), "y": round(cy / count), "z": round(cz / count),
    }


for group in sorted(os.listdir(SCENES)):
    gdir = os.path.join(SCENES, group)
    for scene in sorted(os.listdir(gdir)):
        sdir = os.path.join(gdir, scene)
        if os.path.isdir(sdir):
            walk_scene(sdir, scene)

eval_flags({r["rm"] for r in rows_out})
png_kinds = {}
for r in rows_out:
    flags = _rm_cache.get(r["rm"])
    r["zmode"] = {0: "opa", 0x400: "inter", 0x800: "xlu", 0xC00: "dec"}.get((flags or 0) & ZMODE_MASK, "?") if flags is not None else "?"
    r["zcmp"] = int(bool(flags and flags & Z_CMP))
    r["zupd"] = int(bool(flags and flags & Z_UPD))
    r["cvg_x_alpha"] = int(bool(flags and flags & CVG_X_ALPHA))
    if r["png"]:
        key = (r["scene"], r["png"])
        if key not in png_kinds:
            group = next(g for g in os.listdir(SCENES) if os.path.isdir(os.path.join(SCENES, g, r["scene"])))
            png_kinds[key] = png_alpha_kind(os.path.join(SCENES, group, r["scene"], r["png"]))
        r["alpha"] = png_kinds[key]
    else:
        r["alpha"] = "none"
    r["excluded_now"] = int(r["zmode"] == "xlu" and (not r["zcmp"] or r["tris"] <= 2))

fields = ["scene", "room", "list", "dl", "tris", "zmode", "zcmp", "zupd", "cvg_x_alpha", "alpha",
          "fmt", "tex", "prim_a", "comb", "facing", "x", "y", "z", "excluded_now", "rm", "png"]
with open(os.path.join(OUT, "cutout_inventory.csv"), "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
    w.writeheader()
    w.writerows(rows_out)

xlu = [r for r in rows_out if r["zmode"] == "xlu"]
excl = [r for r in xlu if r["excluded_now"]]
summary = defaultdict(int)
for r in excl:
    summary[(r["alpha"], r["facing"], "zcmp" if r["zcmp"] else "nozcmp")] += 1
with open(os.path.join(OUT, "cutout_summary.txt"), "w") as fh:
    fh.write(f"draws walked: {len(rows_out)}; translucent z mode: {len(xlu)}; kept out of the traced scene now: {len(excl)}\n")
    fh.write("unresolved render modes: %d\n" % sum(1 for r in rows_out if r["zmode"] == "?"))
    fh.write("\nkept out, by texture alpha / facing / depth compare:\n")
    for k, v in sorted(summary.items(), key=lambda kv: -kv[1]):
        fh.write(f"  {v:5d}  {k[0]:8s} {k[1]:6s} {k[2]}\n")
    fh.write("\nkept out, cut-out texture, per scene (scene: count, facing):\n")
    per = defaultdict(lambda: defaultdict(int))
    for r in excl:
        if r["alpha"].startswith("cutout"):
            per[r["scene"]][r["facing"]] += 1
    for s in sorted(per):
        fh.write(f"  {s}: " + ", ".join(f"{f} {n}" for f, n in sorted(per[s].items())) + "\n")
print(open(os.path.join(OUT, "cutout_summary.txt")).read())
