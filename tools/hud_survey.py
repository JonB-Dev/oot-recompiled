"""Where the interface's elements sit in a capture, as numbers.

usage: hud_survey.py <capture.png> [<capture.png> ...]

For each capture prints the bounding box of the elements that have a color of their own: the
hearts (red), the B button (the dark green disc), the A button (the blue disc), the C buttons
(the yellow discs) and the minimap (the teal of the overworld map, in the lower half). The
rupee counter is left to the eye: its green is the grass's. With the interface at the
console's ratio in a 1280 by 720 frame, the hearts start near column 240 and the buttons end
near column 1050; anchored to the edges they start near column 80 and end near column 1200,
the console's own margins from the frame's edges. A box that is missing is printed as such,
which is the check that every element still exists. The thresholds were set from sampled
pixels (a heart 255 70 50, the B button 0 100 0, the A button 60 60 170, a C button
170 107 0, the map 54 132 101, the sky 135 152 255, grass 144 156 75) with room for the
dither and the time of day.
"""
import sys

from PIL import Image


def box_of(pixels, width, height, region, test):
    x0, y0, x1, y1 = region
    left = top = None
    right = bottom = None
    count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if test(pixels[x, y]):
                count += 1
                left = x if left is None or x < left else left
                right = x if right is None or x > right else right
                top = y if top is None or y < top else top
                bottom = y if bottom is None or y > bottom else bottom
    if count < 300:
        return None
    return (left, right, top, bottom, count)


def red(p):
    r, g, b = p[:3]
    return r > 170 and g < 90 and b < 90


def green(p):
    r, g, b = p[:3]
    return g > 70 and r < 40 and b < 40


def blue(p):
    r, g, b = p[:3]
    return b > 140 and r < 90 and g < 90


def yellow(p):
    r, g, b = p[:3]
    return r > 150 and g > 90 and g < 150 and b < 40


def teal(p):
    r, g, b = p[:3]
    return g > 110 and b > 80 and r < 90 and g >= b


def main(paths):
    for path in paths:
        image = Image.open(path).convert("RGB")
        width, height = image.size
        pixels = image.load()
        # Each element is looked for in its own quarter of the frame, so a heart's dark edge is
        # not read as the A button and the sky is not read as anything: the hearts in the upper
        # left, the buttons in the upper right (from column 560, where the B button starts at
        # the console's ratio), the minimap in the lower right.
        upper_left = (0, 0, width // 2, height // 2)
        upper_right = (width * 7 // 16, 0, width, height // 2)
        lower_right = (width // 2, height // 2, width, height)
        elements = [
            ("hearts", red, upper_left),
            ("B button", green, upper_right),
            ("A button", blue, upper_right),
            ("C buttons", yellow, upper_right),
            ("minimap", teal, lower_right),
        ]
        print(f"{path} ({width} by {height})")
        for name, test, region in elements:
            box = box_of(pixels, width, height, region, test)
            if box is None:
                print(f"  {name:<12} missing")
            else:
                left, right, top, bottom, count = box
                print(f"  {name:<12} x {left:4d} to {right:4d}, y {top:3d} to {bottom:3d} ({count} pixels)")


if __name__ == "__main__":
    main(sys.argv[1:])
