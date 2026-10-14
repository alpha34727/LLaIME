"""Render the TIP profile icon into a multi-size .ico.

The shell does not recolor profile icons for the taskbar theme, so the default style is a
white rounded tile with a dark outline and glyph (like other third-party IMEs), which reads
on both light and dark taskbars. --style mono writes a 1-bpp glyph-only icon instead.
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


def framed(glyph: str, size: int, font_path: str) -> Image.Image:
    """White rounded tile, dark border, dark glyph; drawn at 8x then downsampled."""
    scale = 8
    canvas = size * scale
    img = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    border = max(1, round(size / 16)) * scale
    inset = scale // 2
    radius = canvas * 0.18
    draw.rounded_rectangle((inset, inset, canvas - 1 - inset, canvas - 1 - inset), radius,
                           fill=(255, 255, 255, 255), outline=(64, 64, 64, 255), width=border)
    font = ImageFont.truetype(font_path, int(canvas * 0.72))
    left, top, right, bottom = draw.textbbox((0, 0), glyph, font=font)
    x = (canvas - (right - left)) / 2 - left
    y = (canvas - (bottom - top)) / 2 - top
    draw.text((x, y), glyph, font=font, fill=(32, 32, 32, 255))
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
    parser.add_argument("--font", default=r"C:\Windows\Fonts\msjhbd.ttc")
    parser.add_argument("--style", choices=["framed", "mono"], default="framed")
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.style == "mono":
        write_monochrome_ico(args.output, [render(args.glyph, s, args.font) for s in SIZES])
    else:
        images = [framed(args.glyph, s, args.font) for s in SIZES + [256]]
        images[-1].save(args.output, format="ICO", sizes=[(s, s) for s in SIZES + [256]],
                        append_images=images[:-1])


if __name__ == "__main__":
    main()
