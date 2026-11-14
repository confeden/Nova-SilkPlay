#!/usr/bin/env python3
"""cut_features.py - how well cheap global features separate scene cuts from motion (P23).

For every consecutive pair of a corpus (optionally decimated by --stride) it computes
features a GPU can reduce to one number without a readback, and prints their distribution
over the pairs labelled as cuts and over everything else:

  hist   half the L1 distance between 16-band luma histograms (0 same .. 1 disjoint)
  spatial mean half-L1 distance of those histograms averaged over a 4x4 image grid
  mean   |mean luma difference| / 255
  std    |luma standard deviation difference| / 255
  grid   mean |difference| of a 4x4 grid of block-mean lumas / 255 (coarse layout)

  python cut_features.py --corpus corpus/R4_spring245 --cuts 20,61,93,146
  python cut_features.py --corpus corpus/R3_bbb60_720crop --stride 4
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image


def luma(path):
    f = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return 0.2126 * f[..., 0] + 0.7152 * f[..., 1] + 0.0722 * f[..., 2]


def tone_hist(y):
    return np.bincount(np.clip((y / 16).astype(int), 0, 15).ravel(), minlength=16) / y.size


def features(ya, yb):
    ha, hb = tone_hist(ya), tone_hist(yb)
    h, w = ya.shape
    ga = np.array([[ya[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4].mean() for j in range(4)] for i in range(4)])
    gb = np.array([[yb[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4].mean() for j in range(4)] for i in range(4)])
    spatial = 0.0
    for i in range(4):
        for j in range(4):
            ya_tile = ya[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4]
            yb_tile = yb[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4]
            ha_tile = np.bincount(np.clip((ya_tile / 16).astype(int), 0, 15).ravel(), minlength=16) / ya_tile.size
            hb_tile = np.bincount(np.clip((yb_tile / 16).astype(int), 0, 15).ravel(), minlength=16) / yb_tile.size
            spatial += 0.5 * np.abs(ha_tile - hb_tile).sum() / 16.0
    # Match CSCutHist's sample locations and 8x8-cell quadrants. These values
    # are still a CPU proxy: the shader samples linear-light sRGB resources.
    yi = np.minimum(4 + 8 * np.arange((ya.shape[0] + 7) // 8), ya.shape[0] - 1)
    xi = np.minimum(4 + 8 * np.arange((ya.shape[1] + 7) // 8), ya.shape[1] - 1)
    ya8, yb8 = ya[np.ix_(yi, xi)], yb[np.ix_(yi, xi)]
    h8, w8 = ya8.shape
    spatial8 = 0.0
    for top in (0, h8 // 2):
        for left in (0, w8 // 2):
            ya_tile = ya8[top:top + h8 // 2, left:left + w8 // 2]
            yb_tile = yb8[top:top + h8 // 2, left:left + w8 // 2]
            spatial8 += 0.5 * np.abs(tone_hist(ya_tile) - tone_hist(yb_tile)).sum() / 4.0
    return {
        "hist": 0.5 * np.abs(ha - hb).sum(),
        "spatial": spatial,
        "spatial8": spatial8,
        "mean": abs(ya.mean() - yb.mean()) / 255.0,
        "std": abs(ya.std() - yb.std()) / 255.0,
        "grid": np.abs(ga - gb).mean() / 255.0,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--stride", type=int, default=1)
    ap.add_argument("--cuts", default="", help="pair indices (in decimated numbering) that are cuts")
    ap.add_argument("--glob", default="*.png")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.corpus, a.glob)))[::a.stride]
    cuts = {int(c) for c in a.cuts.split(",") if c}
    rows = []
    prev = luma(files[0])
    for i in range(1, len(files)):
        cur = luma(files[i])
        rows.append((i - 1, features(prev, cur)))
        prev = cur
    print(f"{a.corpus} stride {a.stride}: {len(rows)} pairs, {len(cuts)} labelled cuts")
    for name in ("hist", "spatial", "spatial8", "mean", "std", "grid"):
        non = sorted(f[name] for i, f in rows if i not in cuts)
        cut = sorted(f[name] for i, f in rows if i in cuts)
        line = f"  {name:5s} non-cut max {non[-1]:.3f} p99 {non[int(0.99 * (len(non) - 1))]:.3f} median {non[len(non) // 2]:.3f}"
        if cut:
            line += f" | cut min {cut[0]:.3f} max {cut[-1]:.3f}"
        print(line)


if __name__ == "__main__":
    main()
