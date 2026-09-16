"""Build the ORACLE FLOW field for an analytic corpus: the true motion, on the
engine's own coarse grid, in the engine's own units.

Why this exists. Every arm scored by run_analytic.py measures one number that is
really two faults added together: the motion estimator guessed the wrong vector,
AND the warp/blend architecture cannot reconstruct the frame even when handed the
right one. A loss on a grating or an occlusion band tells you which arm lost,
never which of those two halves lost it. Feeding the engine the true field turns
the first fault off, so whatever error survives is the architecture's own
resampling ceiling -- the floor no motion estimator, however good, can get under.
That is the reference this file produces.

The conversion. The corpus stores, per ground-truth frame at (interval, t), the
displacement from the intermediate frame back to source A and forward to source
B. The engine instead wants the A->B displacement over one FULL source interval,
anchored on the intermediate frame. For linear motion v those are
flow_to_a = -v*t and flow_to_b = v*(1-t), so

    v_true = flow_to_b - flow_to_a

exactly, with t cancelling out. That subtraction is the entire conversion.

Sampling. The engine keeps the field on cells of cellPx pixels square, so each
cell gets ONE vector: v_true read at the cell centre by nearest-pixel lookup,
never averaged over the cell. An average would blend the foreground's and the
background's motion into a compromise vector wherever a cell straddles an object
boundary -- which is precisely the defect the oracle is meant to measure rather
than to hide. The oracle must be the best an honest cell-based field can be, not
a smoothed one.

Output is raw little-endian float32, no header: gridH*gridW*2 values, row-major,
x then y per cell, named to match the engine's pair%04d_t%03d.png outputs.
"""
import argparse
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# Either of these means the convention is wrong, not that the scene is unusual: a
# static field would mean the two half-flows were subtracted the wrong way (or
# from each other), and a huge one would mean the field is in some unit other
# than pixels per source interval.
WARN_MAX_PX = 200.0


def here_path(p):
    """Relative paths resolve against the script, not the shell's cwd, so the
    same command works from anywhere."""
    return p if os.path.isabs(p) else os.path.join(HERE, p)


def load_flow(path, W, H, what):
    a = np.load(path)
    if a.dtype != np.float32:
        raise SystemExit(f"{what}: {path}\n  dtype is {a.dtype}, expected float32")
    if a.shape != (H, W, 2):
        raise SystemExit(f"{what}: {path}\n  shape is {a.shape}, expected {(H, W, 2)} "
                         f"from clip.json size [W,H] = [{W},{H}]")
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default="corpus/A3_occl")
    ap.add_argument("--out", default=None, help="default: <corpus>/oracle")
    ap.add_argument("--cell", type=int, default=8, help="engine cell size in pixels")
    args = ap.parse_args()

    corpus = here_path(args.corpus)
    out_dir = here_path(args.out) if args.out else os.path.join(corpus, "oracle")
    cell = args.cell
    if cell < 1:
        print(f"--cell must be at least 1, got {cell}")
        return 2

    with open(os.path.join(corpus, "clip.json"), encoding="utf-8") as f:
        clip = json.load(f)

    W, H = clip["size"]          # clip.json stores [W, H]; numpy arrays are (H, W, 2)
    gridW = math.ceil(W / cell)
    gridH = math.ceil(H / cell)

    gts = {}   # (interval, t_float) -> {"flow_to_a": path, "flow_to_b": path}
    for entry in clip["files"]:
        if entry["role"] == "ground_truth" and entry["kind"] in ("flow_to_a", "flow_to_b"):
            key = (entry["interval"], entry["t_float"])
            if key not in gts:
                gts[key] = {}
            gts[key][entry["kind"]] = entry["path"]

    if not gts:
        print("the manifest lists no ground-truth flow_to_a / flow_to_b files")
        return 2

    # Half a pair cannot be converted, and silently skipping the frame would leave
    # a gap the engine would fill with whatever was in that slot before.
    incomplete = sorted(k for k, v in gts.items()
                        if "flow_to_a" not in v or "flow_to_b" not in v)
    if incomplete:
        print(f"{len(incomplete)} ground-truth frame(s) lack flow_to_a or flow_to_b, "
              f"first {incomplete[0]}")
        return 2

    # Cell centres as integer pixel indices. Clamped because the last cell of a row
    # or column is a partial one whenever W or H is not a multiple of cell.
    cx_px = np.minimum(np.arange(gridW) * cell + cell // 2, W - 1)
    cy_px = np.minimum(np.arange(gridH) * cell + cell // 2, H - 1)

    os.makedirs(out_dir, exist_ok=True)

    records = []
    written_names = {}
    for interval, t in sorted(gts.keys()):
        paths = gts[(interval, t)]
        to_a = load_flow(os.path.join(corpus, paths["flow_to_a"]), W, H, "flow_to_a")
        to_b = load_flow(os.path.join(corpus, paths["flow_to_b"]), W, H, "flow_to_b")
        if to_a.shape != to_b.shape:
            raise SystemExit(f"interval {interval} t {t}: flow_to_a {to_a.shape} and "
                             f"flow_to_b {to_b.shape} disagree")

        v_true = to_b - to_a                     # the whole conversion
        grid = v_true[np.ix_(cy_px, cx_px)]      # nearest-pixel, never an average
        grid = np.ascontiguousarray(grid, dtype="<f4")
        if grid.shape != (gridH, gridW, 2):
            raise SystemExit(f"internal: grid is {grid.shape}, expected {(gridH, gridW, 2)}")

        name = f"pair{interval:04d}_t{round(t * 100):03d}.f32"
        if name in written_names:
            raise SystemExit(f"{name} would be written twice: t={written_names[name]} and "
                             f"t={t} round to the same hundredth, so the engine could not "
                             f"tell the two frames apart")
        written_names[name] = t

        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(grid.tobytes())

        mag = np.hypot(grid[..., 0], grid[..., 1])
        records.append({
            "file": name,
            "interval": interval,
            "t": t,
            "mag_min": float(mag.min()),
            "mag_max": float(mag.max()),
            "mag_mean": float(mag.mean()),
        })

    manifest = {
        "corpus": os.path.abspath(corpus),
        "cell": cell,
        "gridW": gridW,
        "gridH": gridH,
        "W": W,
        "H": H,
        "n_files": len(records),
        "format": "raw little-endian float32, no header, row-major, (x,y) per cell",
        "convention": "v_true = flow_to_b - flow_to_a; A->B displacement over one full "
                      "source interval, anchored on the intermediate frame; sampled at "
                      "the cell centre by nearest-pixel lookup",
        "files": records,
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    overall_max = max(r["mag_max"] for r in records)
    overall_mean = sum(r["mag_mean"] for r in records) / len(records)

    print(f"corpus {corpus}")
    print(f"  {W}x{H}, cell {cell} -> grid {gridW}x{gridH} "
          f"({gridH * gridW * 2} float32 per file, {gridH * gridW * 2 * 4} bytes)")
    print(f"  wrote {len(records)} field(s) -> {out_dir}")
    print()
    print("%-8s %-6s %10s %10s %10s" % ("interval", "t", "min|v|", "max|v|", "mean|v|"))
    for r in records:
        print("%-8d %-6s %10.4f %10.4f %10.4f" %
              (r["interval"], f"{round(r['t'], 4):g}",
               r["mag_min"], r["mag_max"], r["mag_mean"]))
    print()
    print("ALL FRAMES  max|v_true| = %.4f px   mean|v_true| = %.4f px"
          % (overall_max, overall_mean))

    # The statistics are the only thing that catches a sign or a unit error, since
    # a wrong field still writes a file of exactly the right size.
    bad = [r for r in records if r["mag_max"] == 0.0 or r["mag_max"] > WARN_MAX_PX]
    for r in bad:
        why = "is ZERO (nothing moves)" if r["mag_max"] == 0.0 else f"exceeds {WARN_MAX_PX:g} px"
        print(f"WARNING: {r['file']} (interval {r['interval']}, t={round(r['t'], 4):g}) "
              f"max|v_true| = {r['mag_max']:.4f} px {why} -- that means the convention "
              f"is wrong, not the scene")
    if bad:
        print(f"WARNING: {len(bad)} of {len(records)} field(s) failed the sanity check; "
              f"do NOT score against these")

    return 0


if __name__ == "__main__":
    sys.exit(main())
