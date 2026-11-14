#!/usr/bin/env python3
"""Offline P23 regression probe: cuts hold the prior frame, motion does not."""
import argparse
import glob
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

from PIL import Image
import numpy as np


HERE = pathlib.Path(__file__).resolve().parent
ENGINE = pathlib.Path(os.environ.get("NSP_ENGINE") or HERE.parent / "prototype" / "silkplay.exe")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default="corpus/R4_spring245")
    parser.add_argument("--stride", type=int, default=2)
    parser.add_argument("--cuts", default="20,61,93,146", help="source pair indices, before decimation")
    parser.add_argument("--motion", default="blocks", choices=("blocks", "ofa"))
    parser.add_argument("--log", help="preserve the native cut diagnostics")
    args = parser.parse_args()
    corpus = pathlib.Path(args.corpus)
    if not corpus.is_absolute():
        corpus = HERE / corpus
    if args.stride < 1:
        raise SystemExit("stride must be positive")
    frames = sorted(glob.glob(str(corpus / "*.png")))[::args.stride]
    expected = {int(c) // args.stride for c in args.cuts.split(",") if c.strip()}
    if len(frames) < 2:
        raise SystemExit("need at least two frames")
    with tempfile.TemporaryDirectory(prefix="nsp-p23-") as tmp:
        src, out = pathlib.Path(tmp) / "in", pathlib.Path(tmp) / "out"
        src.mkdir()
        for index, path in enumerate(frames):
            shutil.copyfile(path, src / f"{index:06d}.png")
        env = os.environ.copy()
        env["NSP_CUT_STATS"] = "1"
        result = subprocess.run(
            [str(ENGINE), "--offline", str(src), "--mode", "mc", "--motion", args.motion,
             "--offline-out", str(out), "--offline-t", "0.5"],
            cwd=HERE.parent, capture_output=True, text=True, env=env,
        )
        if args.log:
            pathlib.Path(args.log).write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise SystemExit(result.stdout[-2000:] + result.stderr[-2000:])
        def values(pattern):
            return [float(match.group(1)) for line in result.stdout.splitlines()
                    if (match := re.search(pattern, line))]

        # Warm-up executes the final source pair once before the numbered pass.
        # Drop that diagnostic sample so metric index == pairNNNN.
        spatial = values(r"cutspatial\s+([\d.]+)")[1:]
        hist = values(r"cuthist\s+([\d.]+)")[1:]
        share = [tuple(map(float, match.groups())) for line in result.stdout.splitlines()
                 if (match := re.search(r"cutshare\s+([\d.]+)\s+([\d.]+)", line))][1:]
        held = set()
        for pair in range(len(frames) - 1):
            image = np.asarray(Image.open(out / f"pair{pair:04d}_t050.png").convert("RGB"))
            source_a = np.asarray(Image.open(frames[pair]).convert("RGB"))
            if np.array_equal(image, source_a):
                held.add(pair)
        print(f"expected hard-cut holds: {sorted(expected)}")
        print(f"observed exact prior-frame outputs: {sorted(held)}")
        if spatial:
            samples = sorted({p + d for p in expected | held for d in (-1, 0, 1)} | {106})
            print("GPU cut metrics at candidate pairs:")
            for pair in samples:
                if pair < min(len(spatial), len(hist)):
                    print(f"  pair {pair}: spatial={spatial[pair]:.4f} hist={hist[pair]:.4f} "
                          f"share={share[pair][0]:.4f}/{share[pair][1]:.4f}")
            noncuts = [value for index, value in enumerate(spatial)
                       if index not in expected]
            if noncuts:
                print(f"GPU spatial non-cut max={max(noncuts):.4f}; "
                      f"p99={np.percentile(noncuts, 99):.4f}")
        if held != expected:
            raise SystemExit(f"P23 mismatch: missing={sorted(expected - held)}, false={sorted(held - expected)}")
        print(f"PASS: {len(expected)} hard cuts held exactly; no false holds among {len(frames) - 1} pairs")


if __name__ == "__main__":
    main()
