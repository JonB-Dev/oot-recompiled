"""How EVEN is the motion in a capture burst, step by step?

`burst_count.py` answers whether the picture changed between two grabs. That is enough to tell a
sixty hertz picture from a twenty hertz one, and it is not enough to tell a smooth sixty from a
juddering sixty: a picture that lurches forward twice and then stands still still "moves" at every
step. This prints the SIZE of each step instead, as the fraction of the cropped frame whose pixels
changed by more than the threshold, so the shape of the motion is visible as a sequence.

    python tools/burst_evenness.py build-cmake/harness/stutter/st5-roll

Read it as a rhythm, not as absolute numbers. Grabs are taken as fast as the screen can be copied
(about fourteen milliseconds each), and the picture changes every sixteen and a half, so with an
even picture the fractions drift slowly and neighboring steps are within about half of each
other. A repeating big, big, nothing is a picture presenting in bursts; a single zero among
healthy steps is one frame that was shown twice.
"""

import sys
from pathlib import Path

from PIL import Image, ImageChops

CROP = (120, 200, 840, 600)
THRESHOLD = 40


def cropped(path: Path) -> Image.Image:
    with Image.open(path) as im:
        return im.convert("RGB").crop(CROP)


def moved_fraction(a: Image.Image, b: Image.Image) -> float:
    diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > THRESHOLD else 0)
    return diff.histogram()[255] / (a.size[0] * a.size[1])


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1])
    d = root / "bitblt"
    files = sorted(d.glob("burst-*.png"))
    if not files:
        print(f"no grabs under {d}")
        return 1

    images = [cropped(f) for f in files]
    steps = [moved_fraction(a, b) for a, b in zip(images, images[1:])]

    print(f"{len(files)} grabs, {len(steps)} steps")
    print("  step sizes: " + " ".join(f"{s * 100:5.2f}" for s in steps))

    moving = [s for s in steps if s > 0.005]
    if not moving:
        print("  nothing moved: this burst says nothing about evenness")
        return 0

    mean = sum(moving) / len(moving)
    lo = min(moving)
    hi = max(moving)
    stalls = sum(1 for s in steps if s <= 0.005)
    print(f"  of {len(steps)} steps, {len(steps) - stalls} moved and {stalls} did not")
    print(f"  moving steps: smallest {lo * 100:.2f}%, mean {mean * 100:.2f}%, largest {hi * 100:.2f}%, "
          f"largest over smallest {hi / lo:.1f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
