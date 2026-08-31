#!/usr/bin/env python3
"""Build a .anm animation pack for the PIONEER visualizer mode.

The CYD plays pre-drawn frame sequences off the SD card, because the Pioneer
head-unit displays this imitates were frame sequences and no amount of
trigonometry produces that look. This turns a PNG sequence or an animated GIF
into the pack the firmware reads.

Deliberately standard library only. PlatformIO ships a Python but no Pillow, and
requiring a pip install into that virtualenv to convert an image would be a poor
trade - so PNG goes through zlib (stdlib) and GIF through an LZW decoder written
out below.

Usage
-----
    python tools/make_animation.py frames/ -o dolphin.anm
    python tools/make_animation.py clip.gif -o dolphin.anm --fps 12
    python tools/make_animation.py clip.gif -o dolphin.anm --invert --threshold 100

Then copy the .anm onto the card as /anim/dolphin.anm and pick PIONEER in the
visualizer. Several packs in /anim play one after another.

Getting frames out of a video, if that is what you have:
    ffmpeg -i clip.mp4 -vf fps=12,scale=320:-1 frames/%04d.png

Format
------
    magic       uint32   "ANM1"
    width       uint16   <= 320
    height      uint16   <= 200
    frameCount  uint16
    frameRate   uint8    frames per second
    flags       uint8    reserved, 0
    reserved    uint32
    then frameCount frames of ceil(width/8)*height bytes,
    1 bit per pixel, MSB first, row major, 1 = lit.
"""

import argparse
import os
import struct
import sys
import zlib

MAGIC = b"ANM1"
MAX_WIDTH = 320
MAX_HEIGHT = 200


# ---------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """Return (width, height, greyscale bytearray). Stdlib only, via zlib."""
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)

    width = height = depth = colour = None
    palette = b""
    idat = bytearray()
    pos = 8

    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # 4 length + 4 type + body + 4 crc

        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if body[12] != 0:
                raise ValueError("%s is interlaced; re-export without it" % path)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if depth != 8:
        raise ValueError("%s is %d-bit; only 8 bits per channel is supported" % (path, depth))

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour)
    if channels is None:
        raise ValueError("%s has an unsupported colour type %d" % (path, colour))

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height)
    previous = bytearray(stride)
    pos = 0

    for y in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride

        # Undo the per-scanline filter. This is the whole of PNG decoding once
        # zlib has done the hard part.
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upper_left = previous[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, previous[i], upper_left)) & 0xFF
        elif filter_type != 0:
            raise ValueError("%s uses unknown filter %d" % (path, filter_type))

        for x in range(width):
            base = x * channels
            if colour == 3:
                index = line[base] * 3
                r, g, b = palette[index], palette[index + 1], palette[index + 2]
                value = (r * 299 + g * 587 + b * 114) // 1000
            elif colour in (0, 4):
                value = line[base]
            else:
                r, g, b = line[base], line[base + 1], line[base + 2]
                value = (r * 299 + g * 587 + b * 114) // 1000
            out[y * width + x] = value

        previous = line

    return width, height, out


# ---------------------------------------------------------------------------
# GIF
# ---------------------------------------------------------------------------

def _lzw_decode(minimum_size, data, expected):
    """Decode one GIF image's LZW stream into palette indices."""
    clear_code = 1 << minimum_size
    end_code = clear_code + 1
    code_size = minimum_size + 1
    table = [bytes([i]) for i in range(clear_code)] + [b"", b""]

    out = bytearray()
    previous = None
    bit_buffer = 0
    bit_count = 0
    pos = 0

    while len(out) < expected:
        while bit_count < code_size:
            if pos >= len(data):
                return out
            bit_buffer |= data[pos] << bit_count
            bit_count += 8
            pos += 1

        code = bit_buffer & ((1 << code_size) - 1)
        bit_buffer >>= code_size
        bit_count -= code_size

        if code == clear_code:
            table = [bytes([i]) for i in range(clear_code)] + [b"", b""]
            code_size = minimum_size + 1
            previous = None
            continue
        if code == end_code:
            break

        if code < len(table):
            entry = table[code]
        elif previous is not None:
            entry = previous + previous[:1]
        else:
            break

        out += entry
        if previous is not None:
            table.append(previous + entry[:1])
            if len(table) == (1 << code_size) and code_size < 12:
                code_size += 1
        previous = entry

    return out


def read_gif(path):
    """Return (width, height, [greyscale bytearray, ...]) for an animated GIF."""
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError("%s is not a GIF" % path)

    width, height, packed = struct.unpack("<HHB", data[6:11])
    pos = 13
    global_palette = b""
    if packed & 0x80:
        size = 2 << (packed & 7)
        global_palette = data[pos:pos + size * 3]
        pos += size * 3

    # One canvas reused across frames, because GIF frames are patches onto the
    # previous one rather than whole images.
    canvas = bytearray(width * height)
    frames = []

    while pos < len(data):
        block = data[pos]
        pos += 1

        if block == 0x3B:  # trailer
            break

        if block == 0x21:  # extension
            pos += 1  # label
            while pos < len(data) and data[pos]:
                pos += data[pos] + 1
            pos += 1
            continue

        if block != 0x2C:  # not an image descriptor; give up cleanly
            break

        left, top, frame_w, frame_h, flags = struct.unpack("<HHHHB", data[pos:pos + 9])
        pos += 9

        palette = global_palette
        if flags & 0x80:
            size = 2 << (flags & 7)
            palette = data[pos:pos + size * 3]
            pos += size * 3
        interlaced = bool(flags & 0x40)

        minimum_size = data[pos]
        pos += 1

        chunks = bytearray()
        while pos < len(data) and data[pos]:
            length = data[pos]
            chunks += data[pos + 1:pos + 1 + length]
            pos += length + 1
        pos += 1  # block terminator

        indices = _lzw_decode(minimum_size, bytes(chunks), frame_w * frame_h)

        rows = list(range(frame_h))
        if interlaced:
            rows = (list(range(0, frame_h, 8)) + list(range(4, frame_h, 8)) +
                    list(range(2, frame_h, 4)) + list(range(1, frame_h, 2)))

        for source_y, target_y in enumerate(rows):
            for x in range(frame_w):
                offset = source_y * frame_w + x
                if offset >= len(indices):
                    break
                index = indices[offset] * 3
                if index + 2 >= len(palette):
                    continue
                r, g, b = palette[index], palette[index + 1], palette[index + 2]
                canvas_x, canvas_y = left + x, top + target_y
                if 0 <= canvas_x < width and 0 <= canvas_y < height:
                    canvas[canvas_y * width + canvas_x] = (r * 299 + g * 587 + b * 114) // 1000

        frames.append(bytearray(canvas))

    if not frames:
        raise ValueError("%s produced no frames" % path)
    return width, height, frames


# ---------------------------------------------------------------------------
# Packing
# ---------------------------------------------------------------------------

def resize(pixels, width, height, target_w, target_h):
    """Nearest neighbour. The output is one bit per pixel, so anything cleverer
    is thrown away by the threshold two lines later."""
    if (width, height) == (target_w, target_h):
        return pixels
    out = bytearray(target_w * target_h)
    for y in range(target_h):
        source_y = y * height // target_h
        for x in range(target_w):
            out[y * target_w + x] = pixels[source_y * width + (x * width // target_w)]
    return out


def pack_frame(pixels, width, height, threshold, invert):
    row_bytes = (width + 7) // 8
    out = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            lit = pixels[y * width + x] >= threshold
            if invert:
                lit = not lit
            if lit:
                out[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)
    return out


def fit(width, height, requested_w, requested_h):
    """Scale down to fit the overlay while keeping the aspect ratio."""
    if requested_w or requested_h:
        target_w = requested_w or width
        target_h = requested_h or height
    else:
        target_w, target_h = width, height

    scale = min(MAX_WIDTH / target_w, MAX_HEIGHT / target_h, 1.0)
    return max(1, int(target_w * scale)), max(1, int(target_h * scale))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", help="animated GIF, or a directory of PNG frames")
    parser.add_argument("-o", "--output", required=True, help="output .anm path")
    parser.add_argument("--fps", type=int, default=12, help="playback rate (default 12)")
    parser.add_argument("--threshold", type=int, default=128,
                        help="brightness above which a pixel is lit, 0-255 (default 128)")
    parser.add_argument("--invert", action="store_true",
                        help="light the dark pixels instead; use for artwork on white")
    parser.add_argument("--width", type=int, default=0, help="target width before the fit")
    parser.add_argument("--height", type=int, default=0, help="target height before the fit")
    args = parser.parse_args()

    if os.path.isdir(args.source):
        names = sorted(n for n in os.listdir(args.source) if n.lower().endswith(".png"))
        if not names:
            sys.exit("no PNG files in %s" % args.source)
        frames = []
        width = height = None
        for name in names:
            w, h, pixels = read_png(os.path.join(args.source, name))
            if width is None:
                width, height = w, h
            elif (w, h) != (width, height):
                sys.exit("%s is %dx%d but the first frame is %dx%d" % (name, w, h, width, height))
            frames.append(pixels)
    elif args.source.lower().endswith(".gif"):
        width, height, frames = read_gif(args.source)
    else:
        sys.exit("source must be a .gif or a directory of .png frames")

    if len(frames) > 65535:
        sys.exit("too many frames (%d); the format holds 65535" % len(frames))

    target_w, target_h = fit(width, height, args.width, args.height)
    row_bytes = (target_w + 7) // 8

    with open(args.output, "wb") as handle:
        handle.write(MAGIC)
        handle.write(struct.pack("<HHHBBI", target_w, target_h, len(frames),
                                 max(1, min(60, args.fps)), 0, 0))
        for pixels in frames:
            scaled = resize(pixels, width, height, target_w, target_h)
            handle.write(pack_frame(scaled, target_w, target_h, args.threshold, args.invert))

    size = 16 + len(frames) * row_bytes * target_h
    print("%s: %d frames, %dx%d, %d fps, %.1f KB" %
          (args.output, len(frames), target_w, target_h, args.fps, size / 1024.0))
    print("Copy it to the SD card as /anim/%s" % os.path.basename(args.output))


if __name__ == "__main__":
    main()
