"""Turn the capture pairs from tools/harness/motion.ps1 into numbers.

For each pair (two screen copies a few milliseconds apart) report the fraction of pixels that
changed, then across all pairs the mean and the fraction of PAIRS that changed at all. The second
number is the one that matters: at the game's own rate a pair changes only when it straddles a
game frame, so well under half do; with transforms interpolated at the display rate nearly every
pair changes.

An optional region (x,y,w,h in capture pixels) restricts the comparison, so a phase that tags the
player can ask about the middle of the picture rather than the whole of it.

    python tools/motion_check.py build-cmake/harness/motion/camera
    python tools/motion_check.py <dir> --region 400,200,480,320 --min-pairs-changed 0.9

Exit status is 1 when --min-pairs-changed is given and not met, so a phase can gate on it.
Nothing here writes a picture anywhere; the captures stay where the harness put them.
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops

# A pixel counts as changed when any channel moved by more than this.
#
# FORTY, NOT EIGHT, and the reason is measured. At the display's rate the renderer presents
# distinct frames even when the game has drawn nothing new, and it re-dithers each one: on the
# Phase 34 build with nothing tagged, nearly every capture pair differed, in up to two thirds of
# their pixels, by one to thirty two levels, HUD included. At the game's own rate the two
# captures of a pair within one game frame were byte identical. Geometry that actually moves
# changes pixels by far more than thirty two levels at the edges it crosses, so the threshold
# sits above the dither and below motion. The first version used eight and reported a smooth
# picture that was only noise.
THRESHOLD = 40


def changed_fraction(a: Path, b: Path, region):
    with Image.open(a) as ia, Image.open(b) as ib:
        ia = ia.convert("RGB")
        ib = ib.convert("RGB")
        if ia.size != ib.size:
            return None
        if region:
            x, y, w, h = region
            ia = ia.crop((x, y, x + w, y + h))
            ib = ib.crop((x, y, x + w, y + h))
        # Per channel absolute difference, collapsed to one channel by taking the maximum, then
        # thresholded to 0 or 255 so the histogram gives the count directly. All of it runs in
        # the library, which is what keeps thirty pairs of 1280 by 720 under a few seconds.
        diff = ImageChops.difference(ia, ib)
        r, g, bl = diff.split()
        peak = ImageChops.lighter(ImageChops.lighter(r, g), bl)
        mask = peak.point(lambda v: 255 if v > THRESHOLD else 0)
        hist = mask.histogram()
        total = ia.size[0] * ia.size[1]
        return hist[255] / total if total else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("directory", help="the motion.ps1 output directory holding pair-NNN-a.png and pair-NNN-b.png")
    ap.add_argument("--region", help="x,y,w,h in capture pixels; default is the whole capture")
    ap.add_argument("--min-pairs-changed", type=float, default=None,
                    help="fail unless at least this fraction of pairs changed at all (0 to 1)")
    ap.add_argument("--pair-threshold", type=float, default=0.005,
                    help="a pair counts as changed when more than this fraction of its pixels did "
                         "(half a percent: the noise sources the game itself draws, sparkles and "
                         "glows, cover about a tenth of that)")
    args = ap.parse_args()

    region = None
    if args.region:
        parts = [int(p) for p in args.region.split(",")]
        if len(parts) != 4:
            print("--region wants x,y,w,h")
            return 2
        region = tuple(parts)

    directory = Path(args.directory)
    firsts = sorted(directory.glob("pair-*-a.png"))
    if not firsts:
        print(f"no pairs in {directory}")
        return 2

    fractions = []
    for a in firsts:
        b = a.with_name(a.name.replace("-a.png", "-b.png"))
        if not b.exists():
            continue
        f = changed_fraction(a, b, region)
        if f is None:
            print(f"{a.name}: sizes differ, skipped")
            continue
        fractions.append(f)
        print(f"{a.stem[:-2]}: {f:.4f}")

    if not fractions:
        print("no complete pairs")
        return 2

    mean = sum(fractions) / len(fractions)
    pairs_changed = sum(1 for f in fractions if f > args.pair_threshold) / len(fractions)
    print(f"pairs: {len(fractions)}  mean changed fraction: {mean:.4f}  pairs changed: {pairs_changed:.2f}")

    if args.min_pairs_changed is not None and pairs_changed < args.min_pairs_changed:
        print(f"FAIL: {pairs_changed:.2f} of pairs changed, wanted at least {args.min_pairs_changed:.2f}")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
