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
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.environ.get("NSP_ENGINE") or os.path.normpath(os.path.join(HERE, "..", "prototype", "silkplay.exe"))


def parse_one(part):
    if "@" in part:
        m, p = part.split("@", 1)
        return int(m), float(p)
    return int(part), None


def parse_mode(spec):
    """'22@0.1/f3@0.02/sh' -> (22, 0.1, 3, 0.02, ['--ofa-seed-hints']).

    Tokens after the warp mode: fN[@p] = field lab mode, sh = NVOFA hint buffer seeded
    from our coarse field (G54; default is a zero buffer), nofc = no field coherence pass."""
    parts = spec.split("/")
    wm, wp = parse_one(parts[0])
    fm, fp, extra = 0, None, []
    for tok in parts[1:]:
        if tok.startswith("f"):
            fm, fp = parse_one(tok[1:])
        elif tok == "sh":
            extra.append("--ofa-seed-hints")
        elif tok == "nofc":
            extra.append("--no-field-cohere")
    return wm, wp, fm, fp, extra


def run_mode(frames, out, mode, motion, param=None, field=0, field_param=None, extra=()):
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
    if field:
        cmd += ["--field-lab", str(field)]
    if field_param is not None:
        cmd += ["--field-lab-p", str(field_param)]
    cmd += list(extra)
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


def _run_gate_arm(frames, out, spec, motion_default="ofa"):
    """Run one run_analytic.py-style arm on the decimated real-content frames."""
    parts = spec.split(":")
    if len(parts) < 2 or len(parts) > 5:
        raise ValueError(f"bad arm {spec!r}: expected mode:motion[:occ][:labN[@p]][:fldN[@p]]")
    mode, motion = parts[0], parts[1] or motion_default
    occ = lab = fld = None
    for extra in parts[2:]:
        if extra.startswith("lab"):
            lab = extra[3:]
        elif extra.startswith("fld"):
            fld = extra[3:]
        else:
            occ = extra
    if mode == "oracle":
        raise ValueError(f"real-content gate arm {spec!r} cannot use oracle fields")

    hints = motion == "ofa+hints"
    seed_hints = motion == "ofa+seedhints"
    engine_motion = "ofa" if hints or seed_hints else motion
    src = out + "_src"
    shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(out)
    os.makedirs(src)
    try:
        for k, frame in enumerate(frames):
            shutil.copyfile(frame, os.path.join(src, f"{k:06d}.png"))
        cmd = [ENGINE, "--offline", src, "--mode", mode, "--motion", engine_motion,
               "--offline-out", out, "--offline-t", "0.5"]
        if hints:
            cmd += ["--ofa-hints"]
        elif seed_hints:
            cmd += ["--ofa-hints", "--ofa-seed-hints"]
        elif engine_motion == "ofa":
            cmd += ["--no-ofa-hints"]
        if occ:
            cmd += ["--occ", occ]
        if lab:
            lab_mode, _, lab_p = lab.partition("@")
            cmd += ["--warp-lab", lab_mode]
            if lab_p:
                cmd += ["--warp-lab-p", lab_p]
        if fld:
            fld_mode, _, fld_p = fld.partition("@")
            cmd += ["--field-lab", fld_mode]
            if fld_p:
                cmd += ["--field-lab-p", fld_p]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"engine failed for {spec}: {result.stdout[-1200:]}{result.stderr[-1200:]}")
        outputs = sorted(glob.glob(os.path.join(out, "pair*_t050.png")))
        if len(outputs) != len(frames) - 1:
            raise RuntimeError(f"{spec}: expected {len(frames) - 1} outputs, got {len(outputs)}")
        return outputs
    finally:
        shutil.rmtree(src, ignore_errors=True)


def score_gate_corpus(corpus, arms, out, *, stride=2, count=32, big=40):
    """Score a fixed real-content clip for gate.py; return per-clip and per-pair metrics."""
    if count < 2:
        raise ValueError("real-content count must be at least 2 input frames")
    all_frames = sorted(glob.glob(os.path.join(corpus, "*.png")))
    if stride < 2 or stride % 2:
        raise ValueError("real-content stride must be even and at least 2")
    if len(all_frames) < stride + 1:
        raise ValueError(f"not enough PNG frames in {corpus}")
    input_indices = list(range(0, min(len(all_frames), (count - 1) * stride + 1), stride))
    frames = [all_frames[i] for i in input_indices]
    truths = [all_frames[i + stride // 2] for i in input_indices[:-1]]
    if len(truths) != len(frames) - 1 or not truths:
        raise ValueError(f"could not form exact midpoint pairs from {corpus}")

    scored = {}
    for spec in arms:
        name = re.sub(r"[^A-Za-z0-9_.-]", "_", spec)
        outputs = _run_gate_arm(frames, os.path.join(out, name), spec)
        mse, big_px, npx, edge_means, edge_high = [], 0, 0, [], []
        per_pair = []
        for pair, (output_path, truth_path) in enumerate(zip(outputs, truths)):
            image, truth = load(output_path), load(truth_path)
            error = np.abs(image - truth)
            pixel_error = error.max(axis=2)
            pair_mse = float((error.astype(np.float64) ** 2).mean())
            edge_mean, edge_high_share = spurious_edges(image, truth)
            mse.append(pair_mse)
            big_px += int((pixel_error > big).sum())
            npx += pixel_error.size
            edge_means.append(edge_mean)
            edge_high.append(edge_high_share)
            per_pair.append({
                "pair": pair,
                "psnr_db": float(10 * np.log10(255.0 ** 2 / max(pair_mse, 1e-9))),
                "big_error_px_pct": float(100 * (pixel_error > big).mean()),
                "spurious_edge_mean": edge_mean,
                "spurious_edge_gt24_pct": float(100 * edge_high_share),
            })
        scored[spec] = {
            "psnr_db": float(10 * np.log10(255.0 ** 2 / max(float(np.mean(mse)), 1e-9))),
            "big_error_px_pct": float(100 * big_px / npx),
            "spurious_edge_mean": float(np.mean(edge_means)),
            "spurious_edge_gt24_pct": float(100 * np.mean(edge_high)),
            "per_pair": per_pair,
        }
    return {
        "corpus": os.path.basename(os.path.normpath(corpus)),
        "stride": stride,
        "pairs": len(truths),
        "big_error_threshold": big,
        "spurious_edge_floor_luma": 8.0,
        "spurious_edge_hot_threshold": 24.0,
        "arms": scored,
    }


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
        m, prm, fm, fp, extra = parse_mode(spec)
        name = "mode" + spec.replace("@", "_p").replace("/", "_")
        outs[spec] = run_mode(frames, os.path.join(a.out, name), m, a.motion, prm, fm, fp, extra)

    base = None
    if "0" in outs:
        base = [np.abs(load(p) - load(t)).max(axis=2) for p, t in zip(outs["0"], truths)]
    print(f"\n{'mode':>16} {'PSNR':>7} {'big px %':>9} {'spur edge':>10} {'spur>24 %':>10} {'big vs mode 0':>22} {'vs mode 0':>10}")
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
        print(f"{m:>16} {psnr:7.2f} {100.0 * big / npx:9.4f} {np.mean(sp_mean):10.3f} "
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
