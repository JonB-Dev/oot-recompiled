"""texture_compare.py: are two folders of captures the same picture?

    python tools\\texture_compare.py <folder-a> <folder-b> [--threshold 0.002]

Pairs the PNG files by name (a capture per scene, written by tools\\harness\\abshots.ps1), and for
each pair reports one of:

    IDENTICAL  <name>                      the bytes are equal
    SAME       <name>  <fraction>          the pixels differ in fewer than the threshold's share
    DIFFERENT  <name>  <fraction>          more than that

then a summary line the harness reads: `IDENTICAL <n> of <m>` when every pair is byte identical,
otherwise `DIFFERENT <k> of <m>`. Exit 0 either way; the caller decides what it wanted (a phase
proving Off is identical wants the first; a phase proving a pass draws something wants the second).

"Fraction" is the share of pixels whose color differs by more than a small amount in any channel,
so a capture with a slightly different dither pattern reads SAME rather than DIFFERENT. Missing
pairs are listed and counted as different.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops


def differing_fraction(a: Path, b: Path, tolerance: int = 8) -> tuple[float, int]:
    """The share of pixels that differ by more than the tolerance, and the largest channel
    difference anywhere. The second number tells dither noise (a few levels) from a change."""
    with Image.open(a) as ia, Image.open(b) as ib:
        ia = ia.convert("RGB")
        ib = ib.convert("RGB")
        if ia.size != ib.size:
            return 1.0, 255
        diff = ImageChops.difference(ia, ib)
        largest = max(hi for _, hi in diff.getextrema())
        gray = diff.convert("L")
        # Any channel over the tolerance counts the pixel; the L conversion keeps the maximum
        # channel difference well enough for a threshold this coarse.
        mask = gray.point(lambda v: 255 if v > tolerance else 0)
        hist = mask.histogram()
        changed = hist[255]
        total = ia.size[0] * ia.size[1]
        return (changed / total if total else 1.0), largest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("--threshold", type=float, default=0.002, help="share of pixels above which a pair is DIFFERENT")
    args = parser.parse_args()

    names = sorted({p.name for p in args.a.glob("*.png")} | {p.name for p in args.b.glob("*.png")})
    if not names:
        print("DIFFERENT 0 of 0 (no captures found)")
        return 0

    identical = 0
    different = 0
    for name in names:
        pa, pb = args.a / name, args.b / name
        if not pa.exists() or not pb.exists():
            print(f"MISSING    {name}  (only in {'a' if pa.exists() else 'b'})")
            different += 1
            continue
        if pa.read_bytes() == pb.read_bytes():
            print(f"IDENTICAL  {name}")
            identical += 1
            continue
        fraction, largest = differing_fraction(pa, pb)
        if fraction <= args.threshold:
            print(f"SAME       {name}  {fraction:.5f}  (largest channel difference {largest})")
        else:
            print(f"DIFFERENT  {name}  {fraction:.5f}  (largest channel difference {largest})")
            different += 1

    if identical == len(names):
        print(f"IDENTICAL {identical} of {len(names)}")
    else:
        print(f"DIFFERENT {different} of {len(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
