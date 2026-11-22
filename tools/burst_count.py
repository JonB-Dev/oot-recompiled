"""Count the distinct pictures in a capture burst from tools/harness/burst.ps1.

Two grabs count as the same picture when they are byte identical over the middle of the frame.
Prints, per method, how many of the grabs were new pictures and the run lengths, which is the
capture's own frame rate made visible: at sixty hertz on screen a burst of twelve grabs of about
fourteen milliseconds each holds about ten distinct pictures; at twenty hertz about four.

It also prints how many steps between consecutive grabs MOVED, by the motion check's rule (a
channel changed by more than forty levels in over half a percent of the crop). Distinct is the
capture's rate and moved is the picture's: at the display's rate the renderer re-dithers every
presented frame, so every grab is distinct whether or not anything moved, and only the moved
count says something was drawn between game frames. A page turn of the pause menu captured with
`pausecycle.ps1 -TurnBurst 24` moves in every step when the pages are tagged and interpolated,
and in about every other step at the menu's own thirty frames a second (phase 45).

    python tools/burst_count.py build-cmake/harness/burst/<tag>
    python tools/burst_count.py build-cmake/harness/pausecycle/<tag>/cycle-1-turn-1-burst
"""

import hashlib
import sys
from pathlib import Path

from PIL import Image, ImageChops

CROP = (120, 200, 840, 600)
# The motion check's threshold and the reason for it: the renderer's re-dithering moves pixels
# by up to thirty two levels between presented frames, and geometry that moves changes them by
# far more at the edges it crosses.
THRESHOLD = 40
MOVED_FRACTION = 0.005


def cropped(path: Path) -> Image.Image:
    with Image.open(path) as im:
        return im.convert("RGB").crop(CROP)


def moved_fraction(a: Image.Image, b: Image.Image) -> float:
    diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > THRESHOLD else 0)
    return diff.histogram()[255] / (a.size[0] * a.size[1])


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1])
    for method in ("bitblt", "printwindow"):
        d = root / method
        files = sorted(d.glob("burst-*.png"))
        if not files:
            print(f"{method}: no grabs")
            continue
        images = [cropped(f) for f in files]
        digests = [hashlib.sha1(im.tobytes()).hexdigest() for im in images]
        distinct = 1
        runs = [1]
        for a, b in zip(digests, digests[1:]):
            if a == b:
                runs[-1] += 1
            else:
                distinct += 1
                runs.append(1)
        moved = sum(1 for a, b in zip(images, images[1:]) if moved_fraction(a, b) > MOVED_FRACTION)
        print(f"{method}: {len(files)} grabs, {distinct} distinct pictures, run lengths {runs}, "
              f"{moved} of {len(files) - 1} steps moved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
