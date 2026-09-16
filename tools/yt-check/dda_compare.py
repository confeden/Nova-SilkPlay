#!/usr/bin/env python3
"""dda_compare.py - two ddacap recordings of the same moment, frame against frame.

Finds the most-moving stretch of the first recording, matches every frame to the most
similar frame of the second one near the same relative time (the arms show the film
with different lags and generate different phases, so time alone does not align them),
and writes a sheet: row A above row B, at full size and zoomed into the region that
moves most. Visual only - real content has no ground truth.

  python dda_compare.py A_PREFIX B_PREFIX W H --labels ours,ls --out DIR [--n 7] [--zoom 3]
"""
import argparse
import csv
import os

import numpy as np
from PIL import Image, ImageDraw


def load(prefix, w, h):
    raw = np.fromfile(prefix + ".raw", dtype=np.uint8)
    n = raw.size // (w * h * 4)
    frames = raw[: n * w * h * 4].reshape(n, h, w, 4)[..., [2, 1, 0]]
    with open(prefix + ".csv", newline="") as f:
        rows = list(csv.reader(line for line in f if not line.startswith("#")))[1:]
    t = np.array([float(r[1]) for r in rows[:n]])
    return frames, t - t[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("w", type=int)
    ap.add_argument("h", type=int)
    ap.add_argument("--labels", default="A,B")
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=7)
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--start", type=int, default=-1, help="first frame of A to show (default: most moving)")
    ap.add_argument("--box", default="", help="zoom box x,y,w,h in crop pixels (default: most moving block)")
    a = ap.parse_args()
    la, lb = a.labels.split(",")
    A, ta = load(a.a, a.w, a.h)
    B, tb = load(a.b, a.w, a.h)
    ga = A[:, ::4, ::4].astype(np.int16)
    gb = B[:, ::4, ::4].astype(np.int16)
    motion = np.abs(ga[1:] - ga[:-1]).mean(axis=(1, 2, 3))
    n = a.n
    start = a.start if a.start >= 0 else int(np.argmax(np.convolve(motion, np.ones(n), mode="valid")))
    idx_a = list(range(start, min(len(A), start + n)))
    # Offset between the two recordings: the best match for the middle frame of the stretch,
    # searched over the whole of B, then every frame matched within +-4 of that offset.
    mid = idx_a[len(idx_a) // 2]
    d_all = np.abs(gb - ga[mid]).mean(axis=(1, 2, 3))
    off = int(np.argmin(d_all)) - mid
    idx_b = []
    for i in idx_a:
        lo, hi = max(0, i + off - 4), min(len(B), i + off + 5)
        d = np.abs(gb[lo:hi] - ga[i]).mean(axis=(1, 2, 3))
        idx_b.append(lo + int(np.argmin(d)))
    if a.box:
        bx, by, bw, bh = (int(v) for v in a.box.split(","))
    else:
        act = np.abs(A[idx_a[-1]].astype(np.int16) - A[idx_a[0]].astype(np.int16)).mean(axis=2)
        bw, bh = a.w // 4, a.h // 4
        best, bx, by = -1.0, 0, 0
        for y in range(0, a.h - bh + 1, 8):
            for x in range(0, a.w - bw + 1, 8):
                s = act[y:y + bh, x:x + bw].mean()
                if s > best:
                    best, bx, by = s, x, y
    zw, zh = bw * a.zoom, bh * a.zoom
    sw, sh = a.w // 2, a.h // 2
    tile_w = max(zw, sw)
    img = Image.new("RGB", (len(idx_a) * tile_w, 2 * (sh + zh + 20)), (24, 24, 24))
    dr = ImageDraw.Draw(img)
    for row, (frames, idx, t, lab) in enumerate(((A, idx_a, ta, la), (B, idx_b, tb, lb))):
        y0 = row * (sh + zh + 20)
        for k, i in enumerate(idx):
            x0 = k * tile_w
            small = Image.fromarray(frames[i]).resize((sw, sh), Image.BILINEAR)
            ImageDraw.Draw(small).rectangle([bx // 2, by // 2, (bx + bw) // 2, (by + bh) // 2], outline=(255, 60, 60))
            img.paste(small, (x0, y0 + 20))
            zoom = Image.fromarray(frames[i, by:by + bh, bx:bx + bw]).resize((zw, zh), Image.NEAREST)
            img.paste(zoom, (x0, y0 + 20 + sh))
            dr.text((x0 + 4, y0 + 4), f"{lab} #{i} {t[i]:.0f} ms", fill=(255, 255, 0))
    name = f"cmp_{os.path.basename(a.a)}_vs_{os.path.basename(a.b)}_{start}.png"
    img.save(os.path.join(a.out, name))
    print(f"stretch {idx_a[0]}..{idx_a[-1]} of {la}, offset {off}, {lb} frames {idx_b}, box {bx},{by},{bw},{bh} -> {name}")


if __name__ == "__main__":
    main()
