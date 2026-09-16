#!/usr/bin/env python3
"""Run the offline engine over a decimated corpus and score every withheld frame.

Decimation: the engine is given every STRIDE-th frame and never sees the ones in
between, so the withheld frames are ground truth at t = k/STRIDE. Nothing about
the timing of the live path is involved — the pair is an array index and the
phase is a number we chose, which is the whole point of the offline instrument.

  python run.py --corpus corpus/R2_meridian --stride 2
  python run.py --corpus corpus/R2_meridian --stride 2 --arms mc:ofa,blend:blocks

An arm is mode:motion[:occ]. The optional third field is the occlusion evidence
(`bidir` or `self`) and it exists so the two can be run against the SAME frames
in one report: a quality claim compared across two invocations is a claim about
two runs, not about the change.

`--strata` exists because a whole-frame average cannot judge a change that only
touches boundaries: measured here, the bidirectional occlusion evidence altered
0.21 % of pixels by up to 106 code levels and moved the frame PSNR by 0.02 dB.
The strata come from the ground truth alone (score/regions.py), never from the
engine's own field — a broken field would otherwise define away its own failure.

The report prints the per-triple table FIRST and the summary last, on purpose: a
report that leads with an average is a report that hides the one clip a change
broke.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from score.regions import masks_from_triple
from score.metrics import psnr_region

# NSP_ENGINE lets an experimental build be scored without touching prototype/.
ENGINE = os.environ.get("NSP_ENGINE") or os.path.join(HERE, "..", "prototype", "silkplay.exe")


def load(path):
    return np.asarray(Image.open(path).convert("RGB"))


def psnr(x, gt, mask=None):
    d = (x.astype(np.float64) - gt.astype(np.float64)) ** 2
    if mask is not None:
        if mask.sum() < 64:
            return None
        d = d[mask]
    mse = float(d.mean())
    return float("inf") if mse == 0.0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--stride", type=int, default=2)
    ap.add_argument("--arms", default="mc:blocks,mc:ofa,blend:blocks")
    ap.add_argument("--run-dir", default=None)
    ap.add_argument("--limit", type=int, default=0, help="only the first N triples")
    ap.add_argument("--keep", action="store_true", help="keep the staged input frames")
    ap.add_argument("--strata", action="store_true", help="enable stratified reporting")
    args = ap.parse_args()

    with open(os.path.join(args.corpus, "clip.json"), encoding="utf-8") as f:
        clip = json.load(f)
    frames = sorted(clip["frames"], key=lambda e: e["index"])
    idx = [e["index"] for e in frames]
    if idx != list(range(idx[0], idx[0] + len(idx))):
        print("corpus frames are not contiguous; decimation would pair the wrong frames")
        return 2

    S = args.stride
    # Sources are the frames at 0, S, 2S, ...; the ground truth frames for the
    # pair (kS, (k+1)S) are the frames in between, which the engine never sees.
    src_pos = list(range(0, len(frames) - S, S))
    t_values = [j / S for j in range(1, S)]
    t_str = ",".join(f"{round(t, 4):g}" for t in t_values)
    if args.limit:
        src_pos = src_pos[: args.limit + 1]
    if len(src_pos) < 2:
        print("not enough frames for one triple")
        return 2

    run_dir = args.run_dir or os.path.join(HERE, "runs", os.path.basename(args.corpus.rstrip("/\\")))
    in_dir = os.path.join(run_dir, "in")
    os.makedirs(in_dir, exist_ok=True)
    for k, p in enumerate(src_pos + [src_pos[-1] + S]):
        shutil.copyfile(os.path.join(args.corpus, frames[p]["file"]),
                        os.path.join(in_dir, f"{k:06d}.png"))

    arms = []
    for spec in args.arms.split(","):
        parts = spec.split(":")
        if len(parts) not in (2, 3):
            print(f"bad arm '{spec}': expected mode:motion[:occ]")
            return 2
        mode, motion = parts[0], parts[1]
        # `ofa+hints` is the NVOFA arm with its search seeded from our own coarse
        # field (G30). Spelled as a motion name so an arm spec still names one
        # configuration completely.
        hints = (motion == "ofa+hints")
        if hints:
            motion = "ofa"
        occ = parts[2] if len(parts) == 3 else None
        out_dir = os.path.join(run_dir, "out_" + "_".join(parts))
        # Hint seeding is ON by default in the engine, so the plain `ofa` arm has
        # to opt OUT explicitly or the two arms would measure the same thing.
        hintFlag = ["--ofa-hints"] if hints else (["--no-ofa-hints"] if motion == "ofa" else [])
        cmd = [ENGINE] + hintFlag + ["--offline", in_dir, "--mode", mode, "--motion", motion,
               "--offline-out", out_dir, "--offline-t", t_str]
        if occ:
            cmd += ["--occ", occ]
        print("running:", " ".join(cmd[1:]), flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])
            print(f"engine failed for {spec}")
            return 2
        arms.append((spec, out_dir))

    # ---- score -------------------------------------------------------------
    rows = []
    for k in range(len(src_pos)):
        a_i, b_i = src_pos[k], src_pos[k] + S
        A = load(os.path.join(args.corpus, frames[a_i]["file"]))
        B = load(os.path.join(args.corpus, frames[b_i]["file"]))
        # How much motion this triple actually carries: without it, an easy
        # triple and a hard one look the same in the table.
        motion = float(np.abs(A.astype(np.int32) - B.astype(np.int32)).mean())
        for j, t in enumerate(t_values, start=1):
            gt_i = src_pos[k] + j
            GT = load(os.path.join(args.corpus, frames[gt_i]["file"]))
            row = {"triple": k, "src": frames[a_i]["index"], "gt": frames[gt_i]["index"], "t": t}
            row["hold-A"] = psnr(A, GT)
            row["hold-B"] = psnr(B, GT)
            TTT = f"{round(t * 100):03d}"

            arm_imgs = {}
            for name, out_dir in arms:
                p = os.path.join(out_dir, f"pair{k:04d}_t{TTT}.png")
                img = load(p) if os.path.exists(p) else None
                arm_imgs[name] = img
                row[name] = psnr(img, GT) if img is not None else None
            row["motion"] = motion

            # Masks depend on (A, GT, B) and so on the phase, but not on the
            # arm: computing them once per row is what keeps every arm scored
            # on byte-identical pixel sets.
            if args.strata:
                masks = masks_from_triple(A, GT, B)
                row["_scene_cut"] = masks["meta"]["scene_cut"]
                row["_counts"] = masks["counts"]
                if not row["_scene_cut"]:
                    for stratum in ("static", "moving_interior", "occlusion", "halo_ring"):
                        m = masks[stratum]
                        row[f"hold-A|{stratum}"] = psnr_region(A, GT, m)
                        row[f"hold-B|{stratum}"] = psnr_region(B, GT, m)
                        for name, _ in arms:
                            img = arm_imgs[name]
                            row[f"{name}|{stratum}"] = psnr_region(img, GT, m) if img is not None else None

            rows.append(row)

    cols = ["hold-A", "hold-B"] + [n for n, _ in arms]
    print()
    print(f"CLIP {clip['clip']}  stride {S}  {len(src_pos)} triples  "
          f"{clip['native_rate']['num']}/{clip['native_rate']['den']} fps native")
    print("PSNR dB against the withheld frame, per triple")
    print("%-6s %-6s %-6s %8s  %s" % ("triple", "gt", "t", "|A-B|", "  ".join("%12s" % c for c in cols)))
    for r in rows:
        best = max((r[c] for c in cols if r[c] is not None), default=None)
        cells = []
        for c in cols:
            v = r[c]
            cells.append("%12s" % ("-" if v is None else
                                   ("%8.2f  *" % v if v == best else "%8.2f   " % v)))
        print("%-6d %-6d %-6s %8.2f  %s" % (r["triple"], r["gt"], f"{round(r['t'], 4):g}", r["motion"], "  ".join(cells)))

    print()
    print("SUMMARY (the table above is the result; this is only its shape)")

    subsets = [(None, rows)]
    if len(t_values) > 1:
        for t in t_values:
            subsets.append((f"t={round(t, 4):g}", [r for r in rows if r["t"] == t]))

    for label, subset in subsets:
        if label:
            print()
            print(f"SUMMARY {label}")
        print("%-14s %8s %8s %8s %8s %8s" % ("arm", "mean", "median", "worst", "best", "wins"))
        for c in cols:
            vals = np.array([r[c] for r in subset if r[c] is not None], dtype=np.float64)
            wins = sum(1 for r in subset
                       if r[c] is not None and r[c] >= max(v for v in
                                                           (r[x] for x in cols) if v is not None))
            print("%-14s %8.2f %8.2f %8.2f %8.2f %8d" %
                  (c, vals.mean(), np.median(vals), vals.min(), vals.max(), wins))

    if args.strata:
        print()
        print("SUMMARY STRATA")
        strata_names = ("static", "moving_interior", "occlusion", "halo_ring")
        cut_skipped = sum(1 for r in rows if r.get("_scene_cut"))
        if cut_skipped:
            print(f"Skipped {cut_skipped} rows due to scene cuts.")

        valid_rows = [r for r in rows if not r.get("_scene_cut")]
        if valid_rows:
            for stratum in strata_names:
                mean_frac = np.mean([r["_counts"][stratum] / r["_counts"]["total"] for r in valid_rows])
                print(f"Stratum {stratum}: {mean_frac*100:.2f}% of pixels")

        for stratum in strata_names:
            stratum_rows = []
            inf_removed = 0
            none_removed = 0
            for r in valid_rows:
                has_none = False
                has_inf = False
                for c in cols:
                    val = r.get(f"{c}|{stratum}")
                    if val is None:
                        has_none = True
                    elif val == math.inf:
                        has_inf = True

                if has_inf:
                    inf_removed += 1
                elif has_none:
                    none_removed += 1
                else:
                    stratum_rows.append(r)

            if not stratum_rows:
                mean_px = 0.0
            else:
                mean_px = float(np.mean([r["_counts"][stratum] for r in stratum_rows]))

            print()
            print(f"--- STRATUM: {stratum} ({len(stratum_rows)} rows, mean {mean_px:.1f} px) ---")
            if inf_removed > 0:
                print(f"(Excluded {inf_removed} rows where an arm scored inf)")
            if none_removed > 0:
                print(f"(Excluded {none_removed} rows where the stratum was refused)")

            print("%-14s %8s %8s %8s %8s %8s" % ("arm", "mean", "median", "worst", "best", "wins"))
            for c in cols:
                vals = np.array([r[f"{c}|{stratum}"] for r in stratum_rows], dtype=np.float64)
                if len(vals) == 0:
                    print("%-14s %8s %8s %8s %8s %8s" % (c, "-", "-", "-", "-", "-"))
                else:
                    wins = sum(1 for r in stratum_rows
                               if r[f"{c}|{stratum}"] >= max(r[f"{x}|{stratum}"] for x in cols))
                    print("%-14s %8.2f %8.2f %8.2f %8.2f %8d" %
                          (c, vals.mean(), np.median(vals), vals.min(), vals.max(), wins))

    # The mask bookkeeping is per-run state, not a measurement; leaving it in
    # rows.json would put a different-shaped record in the file depending on a
    # flag.
    for r in rows:
        r.pop("_scene_cut", None)
        r.pop("_counts", None)

    out_json = os.path.join(run_dir, "rows.json")
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump({"clip": clip["clip"], "stride": S, "rows": rows}, f, indent=2)
    print(f"\nper-triple rows -> {out_json}")

    if not args.keep:
        shutil.rmtree(in_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
