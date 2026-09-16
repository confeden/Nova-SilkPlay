#!/usr/bin/env python3
"""warp_lab.py - run PSWarpLab modes over a real-footage corpus and score them (P21).

The corpus is decimated by STRIDE so the engine never sees the withheld frames; at
t = 0.5 with an even stride the withheld middle frame is exact ground truth. Every mode
runs over the same pairs. PSNR alone cannot judge G50: its fragments are a fraction of
the pixels, and blur scores better than a sharp picture with a few torn edges. So the
report also counts pixels with a large error (any channel off by more than --big) and
SPURIOUS EDGES - edge strength the output has and the truth does not, which is exactly
what a fragment adds and what blur or ghosting never adds.

  python warp_lab.py --corpus corpus/R3_bbb60_720crop --stride 2 --count 32 --modes 0,99,17@0.05,19
  python warp_lab.py --corpus corpus/R3_bbb60_720crop --stride 2 --modes 0 --maps 1,5 --pair 12

A mode is a Synth::SetWarpLab value, optionally with its parameter: 17@0.05. 0 is the
shipping warp; 99 is the lab copy's default path. See PSWarpLab in nsp_synth.cpp.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.normpath(os.path.join(HERE, "..", "prototype", "silkplay.exe"))


def parse_mode(spec):
    """'17@0.02' -> (17, 0.02); '0' -> (0, None)."""
    if "@" in spec:
        m, p = spec.split("@", 1)
        return int(m), float(p)
    return int(spec), None


def run_mode(frames, out, mode, motion, param=None):
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    src = out + "_src"
    if os.path.isdir(src):
        shutil.rmtree(src)
    os.makedirs(src)
    for k, f in enumerate(frames):
        shutil.copyfile(f, os.path.join(src, f"{k:06d}.png"))
    cmd = [ENGINE, "--offline", src, "--offline-out", out, "--offline-t", "0.5", "--mode", "mc",
           "--motion", motion]
    if mode:
        cmd += ["--warp-lab", str(mode)]
    if param is not None:
        cmd += ["--warp-lab-p", str(param)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    shutil.rmtree(src, ignore_errors=True)
    if r.returncode != 0:
        sys.exit(f"engine failed for mode {mode}: {r.stdout[-800:]}{r.stderr[-800:]}")
    return sorted(glob.glob(os.path.join(out, "pair*_t050.png")))


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)


def luma(x):
    x = x.astype(np.float64)
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def grad(y):
    gx = np.abs(np.diff(y, axis=1))[:-1, :]
    gy = np.abs(np.diff(y, axis=0))[:, :-1]
    return np.maximum(gx, gy)


def spurious_edges(o, g, floor=8.0):
    """Mean excess edge strength (luma levels) and the share of pixels whose excess > 24."""
    ex = np.maximum(grad(luma(o)) - grad(luma(g)) - floor, 0.0)
    return float(ex.mean()), float((ex > 24.0).mean())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--stride", type=int, default=4)
    ap.add_argument("--first", type=int, default=0, help="index of the first corpus frame used")
    ap.add_argument("--count", type=int, default=13, help="how many decimated frames (pairs = count-1)")
    ap.add_argument("--modes", default="0,99")
    ap.add_argument("--maps", default="")
    ap.add_argument("--motion", default="ofa")
    ap.add_argument("--big", type=int, default=40, help="a pixel error above this many code levels is 'big'")
    ap.add_argument("--out", default=os.path.join(HERE, "runs", "warp_lab"))
    ap.add_argument("--pair", type=int, default=-1, help="write crops of this pair for every mode/map")
    ap.add_argument("--crop", default="", help="x,y,w,h for --pair crops (default: worst block of mode 0)")
    a = ap.parse_args()
    if a.stride % 2:
        sys.exit("--stride must be even so t=0.5 has an exact ground-truth frame")

    all_frames = sorted(glob.glob(os.path.join(a.corpus, "*.png")))
    idx = [a.first + k * a.stride for k in range(a.count)]
    idx = [i for i in idx if i < len(all_frames)]
    frames = [all_frames[i] for i in idx]
    truths = [all_frames[i + a.stride // 2] for i in idx[:-1]]
    os.makedirs(a.out, exist_ok=True)
    print(f"{len(frames)} frames from {a.corpus}, stride {a.stride}: {len(truths)} pairs scored at t=0.5")

    modes = [m for m in a.modes.split(",") if m != ""]
    maps = [m for m in a.maps.split(",") if m != ""]
    outs = {}
    for spec in modes + maps:
        m, prm = parse_mode(spec)
        outs[spec] = run_mode(frames, os.path.join(a.out, "mode" + spec.replace("@", "_p")), m, a.motion, prm)

    base = None
    if "0" in outs:
        base = [np.abs(load(p) - load(t)).max(axis=2) for p, t in zip(outs["0"], truths)]
    print(f"\n{'mode':>9} {'PSNR':>7} {'big px %':>9} {'spur edge':>10} {'spur>24 %':>10} {'big vs mode 0':>22} {'vs mode 0':>10}")
    for m in modes:
        mse, big, fixed, added, same, npx = [], 0, 0, 0, True, 0
        sp_mean, sp_frac = [], []
        for k, (p, t) in enumerate(zip(outs[m], truths)):
            o, g = load(p), load(t)
            sm, sf = spurious_edges(o, g)
            sp_mean.append(sm)
            sp_frac.append(sf)
            e = np.abs(o - g)
            mse.append(float((e.astype(np.float64) ** 2).mean()))
            em = e.max(axis=2)
            big += int((em > a.big).sum())
            npx += em.size
            if base is not None:
                fixed += int(((base[k] > a.big) & (em <= a.big)).sum())
                added += int(((base[k] <= a.big) & (em > a.big)).sum())
                if hashlib.sha256(open(p, "rb").read()).digest() != \
                        hashlib.sha256(open(outs["0"][k], "rb").read()).digest():
                    same = False
        psnr = 10 * np.log10(255.0 ** 2 / max(np.mean(mse), 1e-9))
        vs = f"-{fixed} / +{added}" if base is not None else ""
        ident = ("identical" if same else "differs") if base is not None else ""
        print(f"{m:>9} {psnr:7.2f} {100.0 * big / npx:9.4f} {np.mean(sp_mean):10.3f} "
              f"{100.0 * np.mean(sp_frac):10.3f} {vs:>22} {ident:>10}")

    if a.pair >= 0 and base is not None:
        k = a.pair
        if a.crop:
            x, y, w, h = (int(v) for v in a.crop.split(","))
        else:
            bm = base[k] > a.big
            h, w = 270, 480
            best, x, y = -1, 0, 0
            for yy in range(0, bm.shape[0] - h + 1, 30):
                for xx in range(0, bm.shape[1] - w + 1, 30):
                    s = int(bm[yy:yy + h, xx:xx + w].sum())
                    if s > best:
                        best, x, y = s, xx, yy
        tiles = [("truth", truths[k])] + [(f"mode {m}", outs[m][k]) for m in modes + maps]
        cols = 3
        sheet = Image.new("RGB", (w * 2 * cols, (h * 2 + 20) * ((len(tiles) + cols - 1) // cols)), (20, 20, 20))
        dr = ImageDraw.Draw(sheet)
        for n, (label, path) in enumerate(tiles):
            im = Image.open(path).convert("RGB").crop((x, y, x + w, y + h)).resize((w * 2, h * 2), Image.NEAREST)
            cx, cy = (n % cols) * w * 2, (n // cols) * (h * 2 + 20)
            sheet.paste(im, (cx, cy + 20))
            dr.text((cx + 4, cy + 4), f"{label}  pair {k}  crop {x},{y},{w},{h}", fill=(255, 255, 0))
        path = os.path.join(a.out, f"sheet_pair{k}.png")
        sheet.save(path)
        print(f"\nsheet -> {path}")


if __name__ == "__main__":
    main()
