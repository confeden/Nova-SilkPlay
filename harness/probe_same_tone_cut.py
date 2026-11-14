#!/usr/bin/env python3
"""Synthetic P23 probe: identical global tone histograms, different shot layout."""
import argparse
import pathlib

import numpy as np
from PIL import Image

try:
    from .cut_features import features
except ImportError:  # Executed as a standalone script from harness/.
    from cut_features import features


def scene(width=960, height=540):
    rng = np.random.default_rng(2301)
    dark = rng.choice(np.array([24, 40, 56, 72], dtype=np.uint8), (height, width // 2))
    bright = rng.choice(np.array([168, 184, 200, 216], dtype=np.uint8), (height, width // 2))
    return np.concatenate((dark, bright), axis=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-dir", help="also write frames for an offline engine run")
    args = parser.parse_args()
    base = scene()
    frames = [np.roll(base, shift=2 * i, axis=1) for i in range(5)]
    cut = np.roll(base, shift=base.shape[1] // 2, axis=1)
    rows = [features(a.astype(np.float64), b.astype(np.float64)) for a, b in zip(frames, frames[1:] + [cut])]
    print("same-tone cut probe (four small pans, then swap equal tone populations between halves)")
    for i, row in enumerate(rows):
        label = "CUT" if i == len(rows) - 1 else "motion"
        print(f"pair {i}: {label:6s} hist={row['hist']:.4f} spatial={row['spatial']:.4f} "
              f"spatial8={row['spatial8']:.4f} grid={row['grid']:.4f}")
    cut_row = rows[-1]
    if cut_row["hist"] > 1e-12 or cut_row["spatial8"] < 0.3:
        raise SystemExit("same-tone probe did not isolate a spatial cut")
    if args.write_dir:
        out = pathlib.Path(args.write_dir)
        out.mkdir(parents=True, exist_ok=True)
        for i, frame in enumerate(frames + [cut]):
            rgb = np.repeat(frame[:, :, None], 3, axis=2)
            Image.fromarray(rgb).save(out / f"{i:06d}.png")
        print(f"wrote {len(frames) + 1} frames to {out}")


if __name__ == "__main__":
    main()
