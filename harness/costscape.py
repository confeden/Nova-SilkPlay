"""Dump the COARSE matching cost landscape, the way CSMatchCoarse actually sees it.

N23 closed off the magnitude prior and said what would have to replace it: a
period alias and a genuine fast vector differ in the SHAPE of the cost landscape,
not in the length of the vector. An alias is supposed to sit among several
near-equal minima spaced one period apart; a real vector in one decisive minimum.

That is a hypothesis about a curve nobody in this project has looked at. This
prints it, so the discriminator is designed from the measured shape instead of
from the story about it. It replicates MatchCell's coarse pass exactly rather
than approximating it -- same /8 level, same 7x7 window, same +-6 steps of 2
level pixels, same symmetric +-v/2 sampling, same 0.0006 length tie-break --
because a landscape from a slightly different cost function would be a landscape
for a different question.

Conventions taken from nsp_synth.cpp and held to deliberately:
  * the pyramid is level0 = /2, level1 = /4, level2 = /8, built by successive 2x
    box downsamples of the LINEAR-LIGHT luma (the shader's SRVs are _SRGB views,
    so everything it samples is linear);
  * MatchCell samples at (centreLvl + o -+ v/2 + 0.5) * invSize with a bilinear
    sampler, i.e. v is split symmetrically between the two frames;
  * gStepLvl = 2.0 level px at the coarse level, so the reachable vectors are
    multiples of 16 FULL-RES px out to +-96.
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

SEARCH = 6        # CSMatchCoarse: MatchCell(id.xy, 6, 3)
WINDOW = 3
STEP_LVL = 2.0    # runMatch(..., 8.0f, 2.0f, ...)
LEVEL_SCALE = 8.0
CELL_PX = 8       # block-matcher default


def srgb_to_linear(x):
    x = x.astype(np.float64) / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def luma_linear(path):
    rgb = srgb_to_linear(np.asarray(Image.open(path).convert("RGB")))
    return rgb @ np.array([0.2126, 0.7152, 0.0722])


def down2(a):
    # Even dimensions only; the pyramid allocator rounds the same way.
    h, w = a.shape
    a = a[: h - (h & 1), : w - (w & 1)]
    return 0.25 * (a[0::2, 0::2] + a[0::2, 1::2] + a[1::2, 0::2] + a[1::2, 1::2])


def bilinear(img, x, y):
    """Match SampleLevel(smpLin, uv): uv in texels, half-texel centres."""
    H, W = img.shape
    x = np.clip(x - 0.5, 0, W - 1)
    y = np.clip(y - 0.5, 0, H - 1)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    x1 = np.clip(x0 + 1, 0, W - 1); y1 = np.clip(y0 + 1, 0, H - 1)
    fx = x - x0; fy = y - y0
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x1] * fx * (1 - fy) +
            img[y1, x0] * (1 - fx) * fy + img[y1, x1] * fx * fy)


def landscape(la, lb, cell_xy):
    """Cost over the full 13x13 coarse candidate grid for one cell.

    Returns (costs[13,13], vx_full[13], vy_full[13]) with the vectors in
    FULL-RESOLUTION pixels, which is the unit every other number here uses.
    """
    cx, cy = cell_xy
    centre = np.array([(cx + 0.5) * CELL_PX, (cy + 0.5) * CELL_PX]) / LEVEL_SCALE
    n = 2 * SEARCH + 1
    costs = np.zeros((n, n))
    offs = np.arange(-WINDOW, WINDOW + 1, dtype=np.float64)
    ox, oy = np.meshgrid(offs, offs)
    for iy, dy in enumerate(range(-SEARCH, SEARCH + 1)):
        for ix, dx in enumerate(range(-SEARCH, SEARCH + 1)):
            v = np.array([dx, dy], dtype=np.float64) * STEP_LVL
            h = v * 0.5
            ax = centre[0] + ox - h[0] + 0.5
            ay = centre[1] + oy - h[1] + 0.5
            bx = centre[0] + ox + h[0] + 0.5
            by = centre[1] + oy + h[1] + 0.5
            sad = np.abs(bilinear(la, ax, ay) - bilinear(lb, bx, by)).sum()
            costs[iy, ix] = sad + 0.0006 * np.hypot(*(v * LEVEL_SCALE))
    axis = np.arange(-SEARCH, SEARCH + 1) * STEP_LVL * LEVEL_SCALE
    return costs, axis, axis


def report(name, la, lb, cells, true_v):
    print(f"\n{'='*74}\n{name}   (true motion {true_v} px/interval)\n{'='*74}")
    for cell in cells:
        costs, vx, _ = landscape(la, lb, cell)
        row = costs[SEARCH]                      # dy = 0, the pan axis
        order = np.argsort(row)
        best_i = order[0]
        print(f"\n cell {cell}:  cost along dy=0, by vx in full-res px")
        print("   vx :  " + " ".join(f"{int(v):>6d}" for v in vx))
        print("   sad:  " + " ".join(f"{c:6.3f}" for c in row))
        print(f"   best vx = {vx[best_i]:+.0f} (sad {row[best_i]:.4f})")
        # The shape question, stated as a number: how much better is the winner
        # than the best candidate that is NOT its immediate neighbour? A single
        # decisive minimum has a large margin; a periodic landscape does not.
        far = [i for i in range(len(row)) if abs(i - best_i) > 1]
        if far:
            j = min(far, key=lambda i: row[i])
            margin = row[j] / max(row[best_i], 1e-9)
            print(f"   best NON-ADJACENT rival vx = {vx[j]:+.0f} (sad {row[j]:.4f})"
                  f"   ratio rival/best = {margin:.2f}x")
        globalmin = row.min()
        near = [(int(vx[i]), round(float(row[i]), 4)) for i in range(len(row))
                if row[i] <= globalmin * 1.5 and abs(i - best_i) > 1]
        print(f"   rivals within 1.5x of the best (excluding neighbours): {near if near else 'none'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="all", choices=["all", "a4", "a3", "pan"])
    ap.add_argument("--pan-v", type=int, default=32)
    args = ap.parse_args()

    if args.case in ("all", "a4"):
        C = os.path.join(HERE, "corpus/A4_p14")
        clip = json.load(open(os.path.join(C, "clip.json"), encoding="utf-8"))
        src = {e["source_index"]: e["path"] for e in clip["files"]
               if e["role"] == "source" and e["kind"] == "rgb"}
        a = luma_linear(os.path.join(C, src[0])); b = luma_linear(os.path.join(C, src[1]))
        for _ in range(3):
            a, b = down2(a), down2(b)
        # Cells in the panning background, away from the static overlay.
        report("A4 (period 43.2 px ALONG a -4.75 px/interval pan)", a, b,
               [(40, 20), (70, 12), (100, 30)], -4.75)

    if args.case in ("all", "a3"):
        C = os.path.join(HERE, "corpus/A3_occl")
        clip = json.load(open(os.path.join(C, "clip.json"), encoding="utf-8"))
        src = {e["source_index"]: e["path"] for e in clip["files"]
               if e["role"] == "source" and e["kind"] == "rgb"}
        a = luma_linear(os.path.join(C, src[0])); b = luma_linear(os.path.join(C, src[1]))
        for _ in range(3):
            a, b = down2(a), down2(b)
        report("A3 (static background, object at +11.25 px/interval)", a, b,
               [(40, 34), (60, 34), (20, 10)], 0.0)

    if args.case in ("all", "pan"):
        p = os.path.join(HERE, "corpus/R2_meridian/000100.png")
        full = np.asarray(Image.open(p).convert("RGB"))
        v = args.pan_v
        a = luma_linear(p)
        b = srgb_to_linear(np.roll(full, v, axis=1)) @ np.array([0.2126, 0.7152, 0.0722])
        for _ in range(3):
            a, b = down2(a), down2(b)
        report(f"REAL PHOTOGRAPH, rigid pan at {v} px/interval (the case N23 killed)",
               a, b, [(60, 40), (120, 60), (180, 30)], float(v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
