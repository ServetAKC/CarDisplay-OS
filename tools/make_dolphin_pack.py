#!/usr/bin/env python3
"""Render the leaping dolphin as a .anm pack.

This exists so the animation installer has something real to install. Copying a
pack onto the SD card needs a card reader, and the whole point of the installer
environment is that there is not one - so the firmware ships with a pack baked
in, and this builds it.

The sprite is READ OUT OF THE FIRMWARE, not copied into this file. The dolphin
art lives once, in visualizerModes.cpp, and editing it there changes both the
procedural DOLPHIN mode and this pack. Two copies would drift the first time
somebody nudged a pixel.

    python tools/make_dolphin_pack.py -o firmware_assets/anim.anm

Then upload the cyd2usb_anim_installer environment once, and cyd2usb after it.
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SPRITE_SOURCE = os.path.join(HERE, "..", "SpotifyDiyThing", "visualizerModes.cpp")

WIDTH = 240
HEIGHT = 140
WATER_Y = 104
FRAMES = 32
FPS = 12

TRAVEL_LEFT = 8
TRAVEL_SPAN = 210
# 74 put the nose three pixels from the top edge. 62 leaves headroom without
# making the leap look timid.
ARC_HEIGHT = 62


def load_sprite(path):
    """Pull DOLPHIN_ART[] out of the firmware source."""
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()

    match = re.search(r"DOLPHIN_ART\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        sys.exit("could not find DOLPHIN_ART[] in %s" % path)

    rows = re.findall(r'"([.#]*)"', match.group(1))
    if not rows:
        sys.exit("DOLPHIN_ART[] in %s held no rows" % path)
    return rows


def draw_sprite(canvas, rows, nose_x, top_y, facing_right, sprite_width, scale):
    """Blit the sprite, mirrored about nose_x when travelling left.

    Mirrors the firmware's drawDolphin(): the art is drawn nose-right, so `col`
    counts back from the nose and facing left flips that about noseX.

    Scaled up by whole pixels. The art is 40x19, which is a small dolphin on a
    240 px canvas - the originals filled a good part of the panel - and a 1-bit
    sprite has nothing to lose to nearest-neighbour anyway.
    """
    for row_index, row in enumerate(rows):
        for col, cell in enumerate(row):
            if cell != "#":
                continue
            from_nose = sprite_width - 1 - col
            base_x = nose_x - from_nose * scale if facing_right else nose_x + from_nose * scale
            base_y = top_y + row_index * scale
            for dy in range(scale):
                y = base_y + dy
                if y < 0 or y >= HEIGHT:
                    continue
                for dx in range(scale):
                    x = base_x + dx
                    if 0 <= x < WIDTH:
                        canvas[y * WIDTH + x] = 1


def draw_sea(canvas, phase):
    """A solid horizon and two drifting dashed swells.

    The first version used continuous sine curves, which at this scale read as
    scattered noise rather than as water. Short dashes on a slow drift read as
    swell and, more to the point, leave the dolphin as the only detailed thing
    on screen.
    """
    for x in range(WIDTH):
        canvas[WATER_Y * WIDTH + x] = 1

    for period, speed, base, length in ((34, 2, 12, 11), (27, -3, 24, 7)):
        offset = (phase * speed) % period
        for start in range(-period, WIDTH + period, period):
            x0 = int(start + offset)
            y = WATER_Y + base
            if not (WATER_Y < y < HEIGHT):
                continue
            for x in range(x0, x0 + length):
                if 0 <= x < WIDTH:
                    canvas[y * WIDTH + x] = 1


def draw_splash(canvas, x, strength):
    """Two short arcs at the water line where the dolphin broke through."""
    for ring in range(strength):
        radius = 4 + ring * 5
        for side in (-1, 1):
            px = x + side * radius
            if 0 <= px < WIDTH:
                for py in range(WATER_Y - 3 - ring, WATER_Y):
                    if 0 <= py < HEIGHT:
                        canvas[py * WIDTH + px] = 1


def pack(frames, path, fps):
    row_bytes = (WIDTH + 7) // 8
    with open(path, "wb") as handle:
        handle.write(b"ANM1")
        handle.write(struct.pack("<HHHBBI", WIDTH, HEIGHT, len(frames), fps, 0, 0))
        for canvas in frames:
            out = bytearray(row_bytes * HEIGHT)
            for y in range(HEIGHT):
                for x in range(WIDTH):
                    if canvas[y * WIDTH + x]:
                        out[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)
            handle.write(out)
    return 16 + len(frames) * row_bytes * HEIGHT


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-o", "--output", default=os.path.join(HERE, "..", "firmware_assets", "anim.anm"))
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--fps", type=int, default=FPS)
    parser.add_argument("--scale", type=int, default=2, help="whole-pixel sprite scale")
    args = parser.parse_args()

    rows = load_sprite(SPRITE_SOURCE)
    sprite_width = max(len(r) for r in rows)
    sprite_height = len(rows)
    print("sprite: %dx%d, read from visualizerModes.cpp" % (sprite_width, sprite_height))

    frames = []
    half = args.frames // 2

    for index in range(args.frames):
        # First half travels right, second half travels back.
        facing_right = index < half
        step = index if facing_right else (args.frames - 1 - index)
        phase = step / float(max(1, half - 1))

        canvas = bytearray(WIDTH * HEIGHT)
        draw_sea(canvas, index)

        # Parabolic leap: at the water line at both ends, highest in the middle.
        height = int(ARC_HEIGHT * 4 * phase * (1.0 - phase))
        nose_x = TRAVEL_LEFT + int(phase * TRAVEL_SPAN)
        body_bottom = WATER_Y + 10 - height
        draw_sprite(canvas, rows, nose_x, body_bottom - sprite_height * args.scale,
                    facing_right, sprite_width, args.scale)

        # Splash for the first few frames after each entry into the water.
        if step < 3:
            draw_splash(canvas, TRAVEL_LEFT if facing_right else TRAVEL_LEFT + TRAVEL_SPAN,
                        3 - step)

        frames.append(canvas)

    output = os.path.abspath(args.output)
    size = pack(frames, output, args.fps)
    print("%s: %d frames, %dx%d, %d fps, %.1f KB" %
          (output, len(frames), WIDTH, HEIGHT, args.fps, size / 1024.0))


if __name__ == "__main__":
    main()
