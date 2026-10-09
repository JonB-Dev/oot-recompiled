"""How wide is the picture inside the frame, and is it pillarboxed?

The check for anything that forces the console's 4:3 into a wider window: the renderer keeps the
ratio and fills the rest with black, so a 4:3 picture in a 16:9 frame is a band of content with a
black bar either side. This prints where the content starts and ends, how wide it is, and what
ratio that works out to.

    python tools/pillarbox.py build-cmake/harness/prerender/pr2-prerendered.png

TWO TRAPS, BOTH MET WHILE WRITING THIS. A screen copy taken by BitBlt includes the window's own
one pixel border, which is bright, so a scan that starts at column zero finds content immediately
and reports no bars at all; the scan starts at column two for that reason. And the bars are not
quite zero, they are about one, so the threshold is a small number rather than an equality.
"""

import sys
from pathlib import Path

from PIL import Image

# A column this dark, averaged down the frame, is a bar rather than a dark part of the picture.
DARK = 6.0
# Skip the window's own border.
EDGE = 2


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    for arg in sys.argv[1:]:
        path = Path(arg)
        if not path.exists():
            print(f"{path.name}: missing")
            continue

        im = Image.open(path).convert("L")
        w, h = im.size
        px = im.load()

        def column_mean(x: int) -> float:
            return sum(px[x, y] for y in range(0, h, 4)) / (h / 4)

        left = EDGE
        while left < w // 2 and column_mean(left) < DARK:
            left += 1
        right = w - 1 - EDGE
        while right > w // 2 and column_mean(right) < DARK:
            right -= 1

        content = right - left + 1
        bars = (left - EDGE) + (w - 1 - EDGE - right)
        ratio = content / h if h else 0.0
        verdict = "pillarboxed" if bars > w * 0.02 else "fills the frame"
        print(f"{path.name}: {w}x{h}, content columns {left} to {right} "
              f"({content} wide, {content / w * 100:.0f}% of the frame), "
              f"bars {bars} px, content ratio {ratio:.2f} -> {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
