#!/usr/bin/env python3
"""Turn a Pioneer .lkd clip into a CarDisplay-OS .anm pack. Stdlib only.

.lkd  = 20-byte header, then gzip -> tar -> 24-bit bottom-up BMP holding
        frame_count frames of 256x64 stacked vertically.
.anm  = "ANM1", w, h, frames, fps, flags, reserved, then 1 bit per pixel.
"""
import gzip, io, struct, sys, tarfile, argparse

def read_lkd(path):
    d = open(path, 'rb').read()
    if d[:4] != b'zLKD':
        raise SystemExit('%s: bad magic %r' % (path, d[:4]))
    version, f1, f2, n = struct.unpack('<4I', d[4:20])
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(d[20:]))) as tf:
        m = tf.getmembers()[0]
        bmp = tf.extractfile(m).read()
    return version, n, bmp, m.name

def parse_bmp(b):
    if b[:2] != b'BM':
        raise SystemExit('not a BMP')
    off = struct.unpack('<I', b[10:14])[0]
    hdr = struct.unpack('<IiiHHI', b[14:34])
    _, w, h, planes, bpp, comp = hdr
    if bpp != 24 or comp != 0:
        raise SystemExit('unexpected BMP: bpp=%d comp=%d' % (bpp, comp))
    bottom_up = h > 0
    h = abs(h)
    stride = (w*3 + 3) & ~3
    # greyscale, top-down row order
    grey = bytearray(w*h)
    for y in range(h):
        src_row = (h-1-y) if bottom_up else y
        p = off + src_row*stride
        q = y*w
        for x in range(w):
            bl, gr, rd = b[p], b[p+1], b[p+2]
            grey[q+x] = (rd*299 + gr*587 + bl*114)//1000
            p += 3
    return w, h, grey

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lkd'); ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--fps', type=int, default=17)
    ap.add_argument('--threshold', type=int, default=128)
    ap.add_argument('--invert', action='store_true')
    ap.add_argument('--dither', action='store_true',
                    help='Bayer 4x4 ordered dither instead of a hard threshold')
    ap.add_argument('--stretch', action='store_true',
                    help='stretch the source levels to full 0..255 before dithering')
    ap.add_argument('--max-frames', type=int, default=0)
    a = ap.parse_args()

    version, n, bmp, member = read_lkd(a.lkd)
    w, h, grey = parse_bmp(bmp)
    fh = h // n
    print('lkd v%d  member=%s  strip=%dx%d  -> %d frames of %dx%d' % (version, member, w, h, n, w, fh))

    hist = [0]*8
    for v in grey: hist[v >> 5] += 1
    print('parlaklik dagilimi (8 kova):', hist)

    frames = list(range(n))
    if a.max_frames and n > a.max_frames:
        # evenly spaced subset, keeps the whole motion
        frames = [round(i*(n-1)/(a.max_frames-1)) for i in range(a.max_frames)]
        print('kare sayisi %d -> %d indirildi' % (n, len(frames)))

    # Bayer 4x4, the same trick the panel itself used to fake grey
    BAYER = [[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]]
    lo, hi = min(grey), max(grey)
    def level(v):
        if a.stretch and hi > lo:
            return (v - lo)*255//(hi - lo)
        return v

    row_bytes = (w + 7)//8
    out = bytearray()
    out += struct.pack('<4sHHHBBI', b'ANM1', w, fh, len(frames), a.fps, 0, 0)
    lit = 0
    for fi in frames:
        base = fi*fh*w
        for y in range(fh):
            row = bytearray(row_bytes)
            r = base + y*w
            for x in range(w):
                v = level(grey[r+x])
                if a.dither:
                    on = v > (BAYER[y & 3][x & 3]*16 + 8)
                else:
                    on = v >= a.threshold
                if a.invert: on = not on
                if on:
                    row[x >> 3] |= 0x80 >> (x & 7); lit += 1
            out += row
    open(a.out, 'wb').write(out)
    total = len(frames)*w*fh
    print('%s: %d kare, %dx%d, %d fps, %.1f KB, yanan piksel %%%.1f'
          % (a.out, len(frames), w, fh, a.fps, len(out)/1024.0, 100.0*lit/total))

if __name__ == "__main__":
    main()
