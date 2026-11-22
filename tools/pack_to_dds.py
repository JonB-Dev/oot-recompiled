"""Convert a PNG texture pack to BC7 DDS with full mip chains, beside the original.

    python tools/pack_to_dds.py <pack folder holding rt64.json> <output folder> [--texconv path] [--jobs N]

The user, 2026-10-07, "yes Absolutely!" to converting his pack. RT64's own guide (lib/rt64/
TEXTURE-PACKS.md) says not to give players PNG packs: every PNG is decoded on the CPU as it streams
in and sits in video memory uncompressed and without mipmaps. BC7 is a quarter of that memory at
full detail, carries its own mip chain (less shimmer at a distance), and is uploaded as it is.

THE FORMAT IS BC7_UNORM, NOT BC7_UNORM_SRGB, and the input's gamma metadata is ignored. RT64
uploads a PNG replacement as R8G8B8A8_UNORM and its shaders use the values as they are, so a DDS
must carry the same values the same way: an sRGB format would be decoded to linear on sampling and
read darker, and a PNG tagged as sRGB would otherwise be converted on the way in.

Microsoft's texconv does the work (DirectXTex, GPU BC7 through DirectCompute). It is a tool on this
machine, never shipped and never committed. One texconv runs per folder of the pack, several at
once, so the folder structure is kept and the GPU stays busy.

Nothing in the source pack is touched. The output is a pack of its own: the textures as .dds under
the same paths, mod.json with its display name marked so the Texture pack row tells the two apart,
thumb.dds and anything else copied, and rt64.json copied LAST, so a conversion that stops part way
never looks like a pack to the program's scan (a pack is a folder holding rt64.json).

The person's own material stays theirs: this reads their files and writes a converted copy beside
them in their own folder, the same way the program reads them, and nothing goes into the repository.

Build-time tooling, never shipped.
"""

import argparse
import concurrent.futures as futures
import json
import os
import shutil
import subprocess
import sys
import time


def leaf_dirs(root):
    """Every folder under root holding at least one PNG, relative to root."""
    found = []
    for dirpath, _dirnames, filenames in os.walk(root):
        if any(name.lower().endswith(".png") for name in filenames):
            found.append(os.path.relpath(dirpath, root))
    return sorted(found)


def convert_dir(texconv, src_root, dst_root, rel):
    src = os.path.join(src_root, rel)
    dst = os.path.join(dst_root, rel)
    os.makedirs(dst, exist_ok=True)
    pngs = [n for n in os.listdir(src) if n.lower().endswith(".png")]
    missing = [n for n in pngs if not os.path.exists(os.path.join(dst, os.path.splitext(n)[0] + ".dds"))]
    if not missing:
        return rel, len(pngs), 0, ""
    # Not recursive: one folder per run keeps each output beside its own path.
    args = [texconv, "-nologo", "-y", "-f", "BC7_UNORM", "-m", "0", "--ignore-srgb", "-o", dst,
            os.path.join(src, "*.png")]
    run = subprocess.run(args, capture_output=True, text=True, errors="replace")
    failed = [line for line in run.stdout.splitlines() if "FAILED" in line or "ERROR" in line]
    still = [n for n in pngs if not os.path.exists(os.path.join(dst, os.path.splitext(n)[0] + ".dds"))]
    note = "; ".join(failed[:3]) if failed else ""
    return rel, len(pngs), len(still), note


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--texconv", default=os.path.join(os.path.dirname(__file__), "..", "build-cmake", "tools-cache", "texconv.exe"))
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--label", default="BC7")
    a = parser.parse_args()

    src_root = os.path.abspath(a.source)
    dst_root = os.path.abspath(a.output)
    if not os.path.isfile(os.path.join(src_root, "rt64.json")):
        sys.exit(f"no rt64.json in {src_root}")
    if not os.path.isfile(a.texconv):
        sys.exit(f"no texconv at {a.texconv}")
    os.makedirs(dst_root, exist_ok=True)

    dirs = leaf_dirs(src_root)
    total = sum(1 for d in dirs for n in os.listdir(os.path.join(src_root, d)) if n.lower().endswith(".png"))
    print(f"{len(dirs)} folders, {total} PNGs, {a.jobs} at once", flush=True)

    start = time.time()
    done = 0
    problems = []
    with futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        jobs = [pool.submit(convert_dir, a.texconv, src_root, dst_root, d) for d in dirs]
        for i, job in enumerate(futures.as_completed(jobs), 1):
            rel, count, still, note = job.result()
            done += count
            if still or note:
                problems.append((rel, still, note))
            if i % 100 == 0 or i == len(jobs):
                print(f"  {i}/{len(jobs)} folders, {done}/{total} textures, {time.time() - start:.0f} s", flush=True)

    # Everything that is not a PNG, copied as it is, except the two files handled below.
    for dirpath, _dirnames, filenames in os.walk(src_root):
        for name in filenames:
            if name.lower().endswith(".png") or (dirpath == src_root and name in ("rt64.json", "mod.json")):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), src_root)
            target = os.path.join(dst_root, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, name), target)

    mod_path = os.path.join(src_root, "mod.json")
    if os.path.isfile(mod_path):
        with open(mod_path, "r", encoding="utf-8") as fh:
            mod = json.load(fh)
        if isinstance(mod.get("display_name"), str):
            mod["display_name"] = f"{mod['display_name']} ({a.label})"
        with open(os.path.join(dst_root, "mod.json"), "w", encoding="utf-8") as fh:
            json.dump(mod, fh, indent=4)

    if problems:
        print(f"PACK_TO_DDS_INCOMPLETE {len(problems)} folders with textures not converted; rt64.json NOT written")
        for rel, still, note in problems[:20]:
            print(f"  {rel}: {still} missing {note}")
        sys.exit(1)

    # Last, so a stopped conversion never reads as a pack.
    shutil.copy2(os.path.join(src_root, "rt64.json"), os.path.join(dst_root, "rt64.json"))
    print(f"PACK_TO_DDS_OK {total} textures in {time.time() - start:.0f} s -> {dst_root}")


if __name__ == "__main__":
    main()
