"""Cut the application's icon from the mark.

The mark is our own drawing: three stacked triangles in the decision's brass (design decision
9693d652-0b2a-43e6-ae79-09ea2382cad3, the panel header's mark, drawn in a 24 by 21 box with a
gap between the three). This writes it as a Windows icon at the sizes the shell asks for, each
rasterized from the geometry at its own size rather than scaled from one bitmap, so the small
ones stay crisp. The icon is the executable's own (src/main/app.rc) and the window's.

    python tools/make_icon.py assets/icon/mark.ico

Pure Python, no imaging library: the small sizes are uncompressed 32-bit bitmaps, which every
Windows version reads, and the two large ones are PNG entries, which Windows expects at those
sizes and which keep the file a tenth of the size. Sixteen samples per pixel soften the edges.
"""

import struct
import sys
import zlib

# The mark, as the decision draws it: three triangles in a 24 wide, 20 tall box (y from 1 to 21).
TRIANGLES = [
    ((12.0, 1.0), (17.6, 10.6), (6.4, 10.6)),     # top
    ((5.9, 11.6), (11.5, 21.0), (0.3, 21.0)),     # bottom left
    ((18.1, 11.6), (23.7, 21.0), (12.5, 21.0)),   # bottom right
]
BOX_W, BOX_TOP, BOX_H = 24.0, 1.0, 20.0

# The brass, from the decision's mark gradient (tokens.rcss carries the same four).
BRASS = (0xB8, 0x91, 0x2F)
BRASS_LIGHT = (0xE8, 0xCD, 0x7E)
BRASS_DEEP = (0xA8, 0x80, 0x1F)

SIZES = [16, 24, 32, 48, 64, 128, 256]
PNG_FROM = 128       # this size and up are stored as PNG
SAMPLES = 4          # per axis, so sixteen per pixel
FILL = 0.86          # the mark's width as a share of the icon's


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def inside(p, tri):
    (x, y) = p
    (ax, ay), (bx, by), (cx, cy) = tri
    d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
    d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
    d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
    negative = d1 < 0 or d2 < 0 or d3 < 0
    positive = d1 > 0 or d2 > 0 or d3 > 0
    return not (negative and positive)


def shade(tri, p):
    """A facet's color at a point: lighter toward its apex, deeper toward the right, so the
    three read as metal rather than as three flat prints."""
    (ax, ay), (bx, by), (cx, cy) = tri
    base_y = max(by, cy)
    t = (p[1] - ay) / (base_y - ay) if base_y > ay else 0.0
    t = min(1.0, max(0.0, t))
    color = lerp(BRASS_LIGHT, BRASS, 0.35 + 0.65 * t)
    tilt = min(1.0, max(0.0, p[0] / BOX_W))
    return lerp(color, BRASS_DEEP, 0.3 * tilt)


def raster(size):
    """Rows of (r, g, b, a) from the top down."""
    scale = size * FILL / BOX_W
    offset_x = (size - BOX_W * scale) / 2.0
    offset_y = (size - BOX_H * scale) / 2.0 - BOX_TOP * scale
    step = 1.0 / SAMPLES
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            hits = 0
            color_sum = [0, 0, 0]
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    x = (px + (sx + 0.5) * step - offset_x) / scale
                    y = (py + (sy + 0.5) * step - offset_y) / scale
                    for tri in TRIANGLES:
                        if inside((x, y), tri):
                            c = shade(tri, (x, y))
                            color_sum[0] += c[0]
                            color_sum[1] += c[1]
                            color_sum[2] += c[2]
                            hits += 1
                            break
            if hits == 0:
                row.append((0, 0, 0, 0))
            else:
                alpha = int(round(255 * hits / (SAMPLES * SAMPLES)))
                row.append((color_sum[0] // hits, color_sum[1] // hits, color_sum[2] // hits, alpha))
        rows.append(row)
    return rows


def bitmap_entry(size):
    """One image of the icon: a BITMAPINFOHEADER, the color rows bottom up in BGRA, then the
    one bit mask every entry must carry even when the alpha channel does the work."""
    rows = raster(size)
    pixels = bytearray()
    for row in reversed(rows):
        for (r, g, b, a) in row:
            pixels += bytes((b, g, r, a))
    mask_row = ((size + 31) // 32) * 4
    mask = bytes(mask_row * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
    return header + bytes(pixels) + mask


def png_entry(size):
    """One image of the icon as a PNG: RGBA rows, each behind a filter byte of zero."""
    rows = raster(size)
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for (r, g, b, a) in row:
            raw += bytes((r, g, b, a))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def write_icon(path):
    entries = [(size, png_entry(size) if size >= PNG_FROM else bitmap_entry(size)) for size in SIZES]
    directory = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    table = b""
    images = b""
    for size, image in entries:
        table += struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
        images += image
    with open(path, "wb") as f:
        f.write(directory + table + images)
    return sum(len(image) for _, image in entries)


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "assets/icon/mark.ico"
    written = write_icon(target)
    print("ICON_OK %s, %d sizes, %d bytes of image" % (target, len(SIZES), written))
