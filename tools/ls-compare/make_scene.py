#!/usr/bin/env python3
"""Render the source frames of an analytic scene for player.html.

  python make_scene.py --scene A3 --size 960x540 --frames 48 --seed 0 --fps 24 --out frames/A3

Source frame k is the scene at t = k (source-frame units), rendered by
harness/gen/analytic.py; the same module renders the exact truth at any
fractional t when the recordings are scored, so nothing here is a reference —
only what the player shows.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "harness" / "gen"))
import analytic  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default="A3")
    ap.add_argument("--size", default="960x540")
    ap.add_argument("--frames", type=int, default=48)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--fps", type=int, default=24)
    ap.add_argument("--out", default="frames/A3")
    ap.add_argument("--step", type=float, default=1.0,
                    help="scene time between source frames; 3 = three times the motion per frame")
    args = ap.parse_args()

    if args.scene not in analytic.list_scenes():
        print(f"scene '{args.scene}' not in {analytic.list_scenes()}", file=sys.stderr)
        return 2
    w, h = (int(v) for v in args.size.lower().split("x"))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    scene = analytic.build_scene(args.scene, size=(w, h), seed=args.seed)
    files = []
    t0 = time.perf_counter()
    for k in range(args.frames):
        rgb = analytic.render(scene, k * args.step)["rgb"]
        name = f"frame_{k:04d}.png"
        analytic.save_png(str(out / name), rgb)
        files.append(name)
        if k % 8 == 7:
            print(f"  rendered {k + 1}/{args.frames}")
    clip = {"scene": args.scene, "size": [w, h], "seed": args.seed, "frames": args.frames,
            "fps": args.fps, "step": args.step, "files": files}
    (out / "clip.json").write_text(json.dumps(clip, indent=2), encoding="utf-8")
    print(f"{args.frames} frames in {time.perf_counter() - t0:.1f} s -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
