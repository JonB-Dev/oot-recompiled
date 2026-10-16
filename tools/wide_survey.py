"""Read a directory of wide captures for what the 2D layer does at the sides of the frame.

usage: wide_survey.py <dir> [--margin 160] [--band 96] [--every 1]

For each capture (pair-NNN-a.png, or any png when there are no pairs) prints the mean luminance of
the left margin, the center and the right margin, over the whole height and over the top and
bottom bands. A fade to black that covers the frame darkens all three columns together; one that
stops at the 4:3 edge darkens the center and leaves the margins where they were. The letterbox
bars of a cutscene show as a top and bottom band that is black across all three columns. The
margin defaults to 160 pixels, the pillar each side of a 4:3 picture in a 1280 by 720 frame.
Numbers, not pictures, so the record can carry them.
"""
import argparse
from pathlib import Path

from PIL import Image


def luminance(image: Image.Image, box) -> float:
    region = image.crop(box).convert("L")
    histogram = region.histogram()
    total = sum(histogram)
    if total == 0:
        return 0.0
    return sum(level * count for level, count in enumerate(histogram)) / total


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    parser.add_argument("--margin", type=int, default=160)
    parser.add_argument("--band", type=int, default=96)
    parser.add_argument("--every", type=int, default=1)
    args = parser.parse_args()

    directory = Path(args.directory)
    files = sorted(directory.glob("pair-*-a.png")) or sorted(directory.glob("*.png"))
    if not files:
        print(f"no captures in {directory}")
        return

    print(f"{'capture':<18} {'left':>6} {'center':>7} {'right':>6} | {'top L':>6} {'top C':>6} {'top R':>6} | {'bot L':>6} {'bot C':>6} {'bot R':>6}")
    for index, path in enumerate(files):
        if index % args.every:
            continue
        image = Image.open(path)
        width, height = image.size
        margin = args.margin
        band = args.band
        columns = [(0, margin), (margin, width - margin), (width - margin, width)]
        whole = [luminance(image, (x0, 0, x1, height)) for x0, x1 in columns]
        top = [luminance(image, (x0, 0, x1, band)) for x0, x1 in columns]
        bottom = [luminance(image, (x0, height - band, x1, height)) for x0, x1 in columns]
        print(f"{path.name:<18} {whole[0]:6.1f} {whole[1]:7.1f} {whole[2]:6.1f} | "
              f"{top[0]:6.1f} {top[1]:6.1f} {top[2]:6.1f} | {bottom[0]:6.1f} {bottom[1]:6.1f} {bottom[2]:6.1f}")
    print(f"{len(files)} captures, {width} by {height}")


if __name__ == "__main__":
    main()
