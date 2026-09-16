#!/usr/bin/env python3
"""dda_view.py - look at a ddacap recording without reading every frame by eye.

Prints the per-frame change series (mean absolute difference to the previous recorded
frame, on a subsampled grid), flags probable scene cuts, and writes contact sheets of
consecutive frames: the most-moving stretch and every cut's neighbourhood, where a
frame generated across a cut would show up as a blend of two shots.

  python dda_view.py PREFIX W H [--sheet 8] [--out DIR]
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
    return frames, rows


def sheet(frames, idx, rows, path, cols=4):
    w, h = frames.shape[2], frames.shape[1]
    rows_n = (len(idx) + cols - 1) // cols
    img = Image.new("RGB", (cols * w, rows_n * (h + 18)), (32, 32, 32))
    d = ImageDraw.Draw(img)
    t0 = float(rows[idx[0]][1])
    for k, i in enumerate(idx):
        x, y = (k % cols) * w, (k // cols) * (h + 18)
        img.paste(Image.fromarray(frames[i]), (x, y + 18))
        d.text((x + 4, y + 3), f"#{i}  +{float(rows[i][1]) - t0:.1f} ms  acc {rows[i][3]}", fill=(255, 255, 0))
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("w", type=int)
    ap.add_argument("h", type=int)
    ap.add_argument("--sheet", type=int, default=8)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or os.path.dirname(a.prefix)
    base = os.path.basename(a.prefix)
    frames, rows = load(a.prefix, a.w, a.h)
    g = frames[:, ::4, ::4].astype(np.int16)
    diff = np.abs(g[1:] - g[:-1]).mean(axis=(1, 2, 3))
    diff = np.concatenate([[0.0], diff])
    med = float(np.median(diff[diff > 0])) if (diff > 0).any() else 0.0
    print(f"{base}: {len(frames)} frames, change per frame median {med:.2f}, max {diff.max():.2f}")
    cuts = [i for i in range(1, len(diff)) if diff[i] > max(25.0, 8 * med)]
    print(f"  probable cuts at frames {cuts[:20]}")
    n = a.sheet
    csum = np.convolve(diff, np.ones(n), mode="valid")
    start = int(np.argmax(csum))
    sheet(frames, list(range(start, start + n)), rows, os.path.join(out, f"{base}_moving.png"))
    print(f"  most-moving stretch {start}..{start + n - 1} -> {base}_moving.png")
    for c in cuts[:3]:
        lo = max(0, c - n // 2)
        sheet(frames, list(range(lo, min(len(frames), lo + n))), rows, os.path.join(out, f"{base}_cut{c}.png"))
        print(f"  cut neighbourhood {lo}..{lo + n - 1} -> {base}_cut{c}.png")


if __name__ == "__main__":
    main()
