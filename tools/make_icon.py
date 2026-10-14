"""Render the TIP icon (a single glyph) into a multi-size .ico."""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def render(glyph: str, size: int, font_path: str) -> Image.Image:
    # Draw at 4x and downsample for clean anti-aliasing at small sizes.
    scale = 4
    canvas = size * scale
    img = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(font_path, int(canvas * 0.86))
    left, top, right, bottom = draw.textbbox((0, 0), glyph, font=font)
    x = (canvas - (right - left)) / 2 - left
    y = (canvas - (bottom - top)) / 2 - top
    draw.text((x, y), glyph, font=font, fill=(32, 32, 32, 255))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--glyph", default="注")
    parser.add_argument("--font", default=r"C:\Windows\Fonts\msjhbd.ttc")
    args = parser.parse_args()

    images = [render(args.glyph, s, args.font) for s in SIZES]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    images[-1].save(args.output, format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=images[:-1])


if __name__ == "__main__":
    main()
