"""Render the TIP profile icon (a single glyph) into a multi-size .ico.

By default the icon is monochrome (1 bpp): Windows recolors monochrome IME icons to the
current theme's text color, so the glyph stays visible on light and dark taskbars.
Use --color for a 32-bpp anti-aliased icon instead.
"""

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64]


def render(glyph: str, size: int, font_path: str) -> Image.Image:
    """Glyph coverage as an 'L' image (255 = ink)."""
    scale = 4
    canvas = size * scale
    img = Image.new("L", (canvas, canvas), 0)
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(font_path, int(canvas * 0.92))
    left, top, right, bottom = draw.textbbox((0, 0), glyph, font=font)
    x = (canvas - (right - left)) / 2 - left
    y = (canvas - (bottom - top)) / 2 - top
    draw.text((x, y), glyph, font=font, fill=255)
    return img.resize((size, size), Image.LANCZOS)


def pack_rows(bits: list[list[int]]) -> bytes:
    """1-bpp rows, bottom-up, each padded to 32 bits."""
    out = bytearray()
    for row in reversed(bits):
        packed = bytearray((len(row) + 7) // 8)
        for i, bit in enumerate(row):
            if bit:
                packed[i // 8] |= 0x80 >> (i % 8)
        while len(packed) % 4:
            packed.append(0)
        out += packed
    return bytes(out)


def monochrome_entry(coverage: Image.Image) -> bytes:
    size = coverage.width
    ink = [[coverage.getpixel((x, y)) >= 110 for x in range(size)] for y in range(size)]
    xor_bits = [[0] * size for _ in range(size)]                       # palette 0 = black
    and_bits = [[0 if ink[y][x] else 1 for x in range(size)] for y in range(size)]
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 1, 0, 0, 0, 0, 2, 0)
    palette = struct.pack("<8B", 0, 0, 0, 0, 255, 255, 255, 0)
    return header + palette + pack_rows(xor_bits) + pack_rows(and_bits)


def write_monochrome_ico(path: Path, images: list[Image.Image]) -> None:
    entries = [monochrome_entry(img) for img in images]
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for img, data in zip(images, entries):
        dim = img.width if img.width < 256 else 0
        out += struct.pack("<BBBBHHII", dim, dim, 2, 0, 1, 1, len(data), offset)
        offset += len(data)
    for data in entries:
        out += data
    path.write_bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--glyph", default="注")
    parser.add_argument("--font", default=r"C:\Windows\Fonts\msjh.ttc")
    parser.add_argument("--color", action="store_true", help="32-bpp anti-aliased icon")
    args = parser.parse_args()

    images = [render(args.glyph, s, args.font) for s in SIZES]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.color:
        rgba = []
        for img in images:
            layer = Image.new("RGBA", img.size, (32, 32, 32, 0))
            layer.putalpha(img)
            rgba.append(layer)
        rgba[-1].save(args.output, format="ICO", sizes=[(s, s) for s in SIZES],
                      append_images=rgba[:-1])
    else:
        write_monochrome_ico(args.output, images)


if __name__ == "__main__":
    main()
