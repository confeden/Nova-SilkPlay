#!/usr/bin/env python3
"""PPM (P6) -> PNG, with optional crop and box downscale.

The prototype's --dump writes PPM because that needs no library on the C++ side;
this turns the result into something a viewer (or a model) can look at. Pure
stdlib: zlib does the compression, struct does the chunks.

  python ppm2png.py shot_mc50.ppm out.png                 # whole frame
  python ppm2png.py shot_mc50.ppm out.png --scale 2       # half size
  python ppm2png.py shot_mc50.ppm out.png --crop 600,150,400,300
  python ppm2png.py a.ppm b.ppm c.ppm stack.png --stack --crop 600,150,400,120
"""
import argparse
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a P6 PPM")
    # Header is three whitespace-separated tokens after the magic, then one
    # whitespace byte, then the raw rows.
    pos = 2
    vals = []
    while len(vals) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        vals.append(int(data[start:pos]))
    pos += 1
    w, h, maxv = vals
    if maxv != 255:
        raise SystemExit(f"{path}: only 8-bit PPM supported")
    return w, h, data[pos : pos + w * h * 3]


def crop(w, h, px, box):
    x, y, cw, ch = box
    x = max(0, min(x, w - 1))
    y = max(0, min(y, h - 1))
    cw = max(1, min(cw, w - x))
    ch = max(1, min(ch, h - y))
    out = bytearray()
    for row in range(y, y + ch):
        off = (row * w + x) * 3
        out += px[off : off + cw * 3]
    return cw, ch, bytes(out)


def downscale(w, h, px, n):
    if n <= 1:
        return w, h, px
    ow, oh = w // n, h // n
    out = bytearray(ow * oh * 3)
    for oy in range(oh):
        for ox in range(ow):
            r = g = b = 0
            for dy in range(n):
                base = ((oy * n + dy) * w + ox * n) * 3
                for dx in range(n):
                    r += px[base + dx * 3]
                    g += px[base + dx * 3 + 1]
                    b += px[base + dx * 3 + 2]
            k = n * n
            o = (oy * ow + ox) * 3
            out[o] = r // k
            out[o + 1] = g // k
            out[o + 2] = b // k
    return ow, oh, bytes(out)


def upscale(w, h, px, n):
    """Nearest-neighbour zoom, for inspecting something 15 px wide."""
    if n <= 1:
        return w, h, px
    ow, oh = w * n, h * n
    out = bytearray(ow * oh * 3)
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += px[(y * w + x) * 3 : (y * w + x) * 3 + 3] * n
        for k in range(n):
            o = ((y * n + k) * ow) * 3
            out[o : o + len(row)] = row
    return ow, oh, bytes(out)


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += rgb[y * w * 3 : (y + 1) * w * 3]
    comp = zlib.compress(bytes(raw), 6)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", comp)
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="one or more .ppm, then the output .png")
    ap.add_argument("--scale", type=int, default=1, help="integer box downscale")
    ap.add_argument("--zoom", type=int, default=1, help="integer nearest-neighbour upscale")
    ap.add_argument("--crop", help="X,Y,W,H applied before scaling")
    ap.add_argument("--stack", action="store_true", help="stack the inputs vertically")
    args = ap.parse_args()

    if len(args.inputs) < 2:
        raise SystemExit("need at least one input .ppm and an output .png")
    out_path = args.inputs[-1]
    ins = args.inputs[:-1]
    box = tuple(int(v) for v in args.crop.split(",")) if args.crop else None

    tiles = []
    for path in ins:
        w, h, px = read_ppm(path)
        if box:
            w, h, px = crop(w, h, px, box)
        w, h, px = downscale(w, h, px, args.scale)
        w, h, px = upscale(w, h, px, args.zoom)
        tiles.append((w, h, px))

    if len(tiles) == 1:
        w, h, px = tiles[0]
        write_png(out_path, w, h, px)
    else:
        width = max(t[0] for t in tiles)
        gap = 4
        height = sum(t[1] for t in tiles) + gap * (len(tiles) - 1)
        canvas = bytearray(width * height * 3)
        y0 = 0
        for w, h, px in tiles:
            for y in range(h):
                dst = ((y0 + y) * width) * 3
                canvas[dst : dst + w * 3] = px[y * w * 3 : (y + 1) * w * 3]
            y0 += h + gap
        write_png(out_path, width, height, bytes(canvas))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    sys.exit(main())
