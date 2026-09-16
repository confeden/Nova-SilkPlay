"""Score the offline engine against an analytic corpus that carries a TRUE
per-pixel occlusion map.

Why this exists next to run.py. The decimation harness derives its strata from
pixels alone, and its "occlusion" class is defined as "both |A-GT| and |B-GT|
are large" -- which also swallows any strongly textured moving region. On the
photographic corpus that class is 12.5 % of the frame, far more than the real
occlusion bands, so a change that only affects genuine disocclusion is diluted
below the noise. Here the generator emits the occlusion map it drew the scene
from (0 = visible in both source frames, 1 = only in A, 2 = only in B), so the
contested pixels are known exactly rather than inferred.

Scene A3 is the case that matters: a hard-edged foreground passing over a
hard-edged background, so the background revealed behind it exists in exactly
one of the two source frames and no vector can make the pair agree there.
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

from score.metrics import psnr_region

# NSP_ENGINE lets an experimental build be scored without touching prototype/.
ENGINE = os.environ.get("NSP_ENGINE") or os.path.join(HERE, "..", "prototype", "silkplay.exe")

def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"))

def load_occl(path):
    return np.load(path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default="corpus/A3_occl")
    ap.add_argument("--arms", default="mc:ofa:self,mc:ofa:bidir")
    ap.add_argument("--run-dir", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--oracle-dir", default=None,
                    help="override the directory an 'oracle:*' arm injects from; used by the "
                         "sensitivity probe, which feeds a deliberately WRONG field and requires "
                         "the score to fall")
    args = ap.parse_args()

    with open(os.path.join(args.corpus, "clip.json"), encoding="utf-8") as f:
        clip = json.load(f)

    sources = {}   # source_index -> path
    gts = {}       # (interval, t_float) -> {"rgb": path, "occl": path}

    for entry in clip["files"]:
        if entry["role"] == "source" and entry["kind"] == "rgb":
            sources[entry["source_index"]] = entry["path"]
        elif entry["role"] == "ground_truth":
            if entry["kind"] in ("rgb", "occl"):
                key = (entry["interval"], entry["t_float"])
                if key not in gts:
                    gts[key] = {}
                gts[key][entry["kind"]] = entry["path"]

    # A gap in the source indices would pair frames that are not neighbours,
    # which is the one error that cannot be seen in the output.
    source_indices = sorted(sources.keys())
    if source_indices != list(range(len(source_indices))):
        print("source frames are not contiguous from 0")
        return 2

    # Every ground-truth frame must bring its own occlusion map: scoring one
    # without it would silently fall back to a whole-frame number under a
    # stratified heading.
    incomplete = sorted(k for k, v in gts.items() if "rgb" not in v or "occl" not in v)
    if incomplete:
        print(f"{len(incomplete)} ground-truth frame(s) lack an rgb or occl file, "
              f"first {incomplete[0]}")
        return 2
    if not gts:
        print("the manifest lists no ground-truth frames")
        return 2

    t_values = sorted(list(set(k[1] for k in gts.keys())))
    t_str = ",".join(f"{round(t, 4):g}" for t in t_values)

    run_dir = args.run_dir or os.path.join(HERE, "runs", os.path.basename(args.corpus.rstrip("/\\")))
    in_dir = os.path.join(run_dir, "in")
    os.makedirs(in_dir, exist_ok=True)

    for i in source_indices:
        shutil.copyfile(os.path.join(args.corpus, sources[i]), os.path.join(in_dir, f"{i:06d}.png"))

    arms = []
    for spec in args.arms.split(","):
        parts = spec.split(":")
        if len(parts) < 2 or len(parts) > 4:
            print(f"bad arm '{spec}': expected mode:motion[:occ][:labN[@p]]")
            return 2
        mode, motion = parts[0], parts[1]
        # Optional fields after mode:motion - the occlusion evidence, and a warp-lab
        # mode (P21) spelled labN or labN@p, e.g. mc:ofa+hints:bidir+cand:lab16@0.05.
        occ, lab = None, None
        for extra in parts[2:]:
            if extra.startswith("lab"):
                lab = extra[3:]
            else:
                occ = extra
        out_dir = os.path.join(run_dir, "out_" + "_".join(p.replace("@", "_p") for p in parts))

        # The ORACLE arm. `oracle:<motion>` runs the same `mc` synthesis but
        # replaces the estimated field with the corpus's TRUE one, written by
        # make_oracle_fields.py. It is the reference that splits one score into
        # two: engine-vs-oracle is the estimator's error, oracle-vs-truth is the
        # architecture's own resampling ceiling, and no amount of tuning moves
        # the second. `motion` is still passed through because it decides the
        # CELL SIZE (ofa uses 4 px, blocks 8), and an oracle scored on a
        # different grid from the arm it is the ceiling for would be a ceiling
        # for nothing.
        inject = None
        if mode == "oracle":
            mode = "mc"
            sub = "oracle4" if motion == "ofa" else "oracle"
            inject = args.oracle_dir or os.path.join(args.corpus, sub)
            if not os.path.isdir(inject):
                print(f"arm '{spec}': no oracle fields at {inject}; "
                      f"run make_oracle_fields.py --corpus {args.corpus} "
                      f"--out {inject} --cell {4 if motion == 'ofa' else 8}")
                return 2

        # `ofa+hints` is the NVOFA arm with its search seeded from our own coarse
        # field. It is spelled as a motion name rather than a flag so an arm
        # spec still names one configuration completely.
        hints = (motion == "ofa+hints")
        if hints:
            motion = "ofa"

        cmd = [ENGINE, "--offline", in_dir, "--mode", mode, "--motion", motion,
               "--offline-out", out_dir, "--offline-t", t_str]
        # Hint seeding is ON by default in the engine, so the plain `ofa` arm has
        # to opt OUT explicitly or the two arms would measure the same thing.
        if hints:
            cmd += ["--ofa-hints"]
        elif motion == "ofa":
            cmd += ["--no-ofa-hints"]
        if occ:
            cmd += ["--occ", occ]
        if lab:
            lab_mode, _, lab_p = lab.partition("@")
            cmd += ["--warp-lab", lab_mode]
            if lab_p:
                cmd += ["--warp-lab-p", lab_p]
        if inject:
            cmd += ["--offline-inject", os.path.abspath(inject)]
        print("running:", " ".join(cmd[1:]), flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])
            print(f"engine failed for {spec}")
            return 2
        arms.append((spec, out_dir))

    rows = []
    arm_names = ["hold-A", "hold-B"] + [spec for spec, _ in arms]

    gt_keys = sorted(gts.keys())
    for interval, t in gt_keys:
        gt_paths = gts[(interval, t)]
        GT = load_rgb(os.path.join(args.corpus, gt_paths["rgb"]))
        occl = load_occl(os.path.join(args.corpus, gt_paths["occl"]))

        A = load_rgb(os.path.join(args.corpus, sources[interval]))
        B = load_rgb(os.path.join(args.corpus, sources[interval+1]))

        TTT = f"{round(t * 100):03d}"

        arm_imgs = {}
        for spec, out_dir in arms:
            p = os.path.join(out_dir, f"pair{interval:04d}_t{TTT}.png")
            arm_imgs[spec] = load_rgb(p) if os.path.exists(p) else None

        masks = {
            "both": occl == 0,
            "only_a": occl == 1,
            "only_b": occl == 2,
            "occluded": occl != 0,
        }

        row = {
            "interval": interval,
            "t": t,
            "mask_px": {m: int(masks[m].sum()) for m in masks},
        }

        for m_name, m_data in masks.items():
            row[f"hold-A|{m_name}"] = psnr_region(A, GT, m_data)
            row[f"hold-B|{m_name}"] = psnr_region(B, GT, m_data)
            for spec, _ in arms:
                img = arm_imgs[spec]
                row[f"{spec}|{m_name}"] = psnr_region(img, GT, m_data) if img is not None else None

        rows.append(row)

    # The per-row table comes first: a report that leads with an average hides
    # the one case a change broke.
    print()
    print("PSNR dB on the 'occluded' mask, per triple")
    cols = arm_names
    print("%-8s %-6s %-9s  %s" % ("interval", "t", "occl_px", "  ".join("%12s" % c for c in cols)))
    for r in rows:
        m_name = "occluded"
        valid_scores = [r[f"{c}|{m_name}"] for c in cols if r[f"{c}|{m_name}"] is not None and r[f"{c}|{m_name}"] != math.inf]
        best = max(valid_scores) if valid_scores else None

        cells = []
        for c in cols:
            v = r[f"{c}|{m_name}"]
            if v is None or v == math.inf:
                cells.append("%12s" % "-")
            else:
                cells.append("%12s" % ("%8.2f  *" % v if v == best else "%8.2f   " % v))

        print("%-8d %-6s %-9d  %s" % (r["interval"], f"{round(r['t'], 4):g}", r["mask_px"]["occluded"], "  ".join(cells)))

    print()
    print("SUMMARY")
    for m_name in ("both", "occluded", "only_a", "only_b"):
        usable_rows = []
        inf_or_none_removed = 0
        for r in rows:
            usable = True
            for c in cols:
                v = r[f"{c}|{m_name}"]
                if v is None or v == math.inf:
                    usable = False
                    break
            if usable:
                usable_rows.append(r)
            else:
                inf_or_none_removed += 1

        print()
        print(f"--- MASK: {m_name} ---")
        if inf_or_none_removed > 0:
            print(f"(Excluded {inf_or_none_removed} rows where an arm scored inf or was refused)")

        print("%-14s %8s %8s %8s %8s %8s" % ("arm", "mean", "median", "worst", "best", "wins"))
        for c in cols:
            vals = np.array([r[f"{c}|{m_name}"] for r in usable_rows], dtype=np.float64)
            if len(vals) == 0:
                print("%-14s %8s %8s %8s %8s %8s" % (c, "-", "-", "-", "-", "-"))
            else:
                wins = sum(1 for r in usable_rows
                           if r[f"{c}|{m_name}"] >= max(r[f"{x}|{m_name}"] for x in cols))
                print("%-14s %8.2f %8.2f %8.2f %8.2f %8d" %
                      (c, vals.mean(), np.median(vals), vals.min(), vals.max(), wins))

    print()
    print("MASK SHARES")
    for m_name in ("both", "occluded", "only_a", "only_b"):
        if not rows:
            break
        fractions = [r["mask_px"][m_name] / (r["mask_px"]["both"] + r["mask_px"]["occluded"]) for r in rows]
        print(f"Mask {m_name}: {np.mean(fractions)*100:.2f}% of pixels")

    out_json = os.path.join(run_dir, "rows.json")
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump({"rows": rows}, f, indent=2)
    print(f"\nrows -> {out_json}")

    if not args.keep:
        shutil.rmtree(in_dir, ignore_errors=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
