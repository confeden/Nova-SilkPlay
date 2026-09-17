#!/usr/bin/env python3
"""cut_features.py - how well cheap global features separate scene cuts from motion (P23).

For every consecutive pair of a corpus (optionally decimated by --stride) it computes
features a GPU can reduce to one number without a readback, and prints their distribution
over the pairs labelled as cuts and over everything else:

  hist   half the L1 distance between 16-band luma histograms (0 same .. 1 disjoint)
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


def features(ya, yb):
    ha = np.bincount(np.clip((ya / 16).astype(int), 0, 15).ravel(), minlength=16) / ya.size
    hb = np.bincount(np.clip((yb / 16).astype(int), 0, 15).ravel(), minlength=16) / yb.size
    h, w = ya.shape
    ga = np.array([[ya[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4].mean() for j in range(4)] for i in range(4)])
    gb = np.array([[yb[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4].mean() for j in range(4)] for i in range(4)])
    return {
        "hist": 0.5 * np.abs(ha - hb).sum(),
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
    for name in ("hist", "mean", "std", "grid"):
        non = sorted(f[name] for i, f in rows if i not in cuts)
        cut = sorted(f[name] for i, f in rows if i in cuts)
        line = f"  {name:5s} non-cut max {non[-1]:.3f} p99 {non[int(0.99 * (len(non) - 1))]:.3f} median {non[len(non) // 2]:.3f}"
        if cut:
            line += f" | cut min {cut[0]:.3f} max {cut[-1]:.3f}"
        print(line)


if __name__ == "__main__":
    main()
