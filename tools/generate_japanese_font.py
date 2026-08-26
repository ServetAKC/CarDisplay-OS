"""Generate HondaThing's low-RAM 16x16 embedded Japanese font.

Usage on Windows:
  python tools/generate_japanese_font.py

The script uses the Japanese font already installed with Windows. PlatformIO
embeds the generated file directly in ESP32 flash.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


RANGES = (
    (0x0020, 0x007E),
    (0x00A0, 0x00FF),
    (0x2000, 0x206F),
    (0x2100, 0x214F),
    (0x3000, 0x30FF),
    (0x31F0, 0x33FF),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
    (0xFF00, 0xFFEF),
)
WIDTH = 16
HEIGHT = 16
BYTES_PER_GLYPH = WIDTH * HEIGHT // 8
GLYPH_COUNT = sum(end - start + 1 for start, end in RANGES)


def glyph_bytes(font: ImageFont.FreeTypeFont, codepoint: int) -> bytes:
    image = Image.new("1", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    try:
        draw.text((0, 0), chr(codepoint), font=font, fill=1, anchor="lt")
    except (UnicodeEncodeError, ValueError):
        draw.text((0, 0), "?", font=font, fill=1, anchor="lt")

    packed = bytearray()
    for y in range(HEIGHT):
        for block in range(2):
            value = 0
            for bit in range(8):
                if image.getpixel((block * 8 + bit, y)):
                    value |= 0x80 >> bit
            packed.append(value)
    return bytes(packed)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--font",
        type=Path,
        default=Path(r"C:\Windows\Fonts\msgothic.ttc"),
        help="Japanese TrueType/OpenType font installed on this computer",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("firmware_assets/jp16.huf"),
    )
    args = parser.parse_args()

    if not args.font.exists():
        raise SystemExit(f"Font not found: {args.font}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    font = ImageFont.truetype(str(args.font), HEIGHT, index=0)

    with args.output.open("wb") as output:
        output.write(struct.pack("<4sHHII", b"HJF1", WIDTH, HEIGHT,
                                 GLYPH_COUNT, BYTES_PER_GLYPH))
        for start, end in RANGES:
            for codepoint in range(start, end + 1):
                output.write(glyph_bytes(font, codepoint))

    expected_size = 16 + GLYPH_COUNT * BYTES_PER_GLYPH
    actual_size = args.output.stat().st_size
    if actual_size != expected_size:
        raise SystemExit(f"Unexpected output size: {actual_size} != {expected_size}")
    print(f"Created {args.output} ({actual_size:,} bytes, {GLYPH_COUNT:,} glyphs)")


if __name__ == "__main__":
    main()
