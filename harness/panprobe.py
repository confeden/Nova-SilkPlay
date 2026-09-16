"""Does the coarse magnitude prior cap the motion the matcher can still track?

G30's fix puts a prior against long vectors at the coarse pyramid level. The
obvious objection is that it must therefore suppress genuinely fast motion, and
the photographic corpus cannot settle it: Meridian is a STATIC camera (measured:
dominant global displacement 0.0 px at every stride), so its large vectors are a
1 % tail of moving subjects, not a whole-frame pan.

This builds the missing case honestly - a RIGID PAN of a real photograph at a
chosen velocity, where the ground truth at any phase is the same photograph
shifted by v*t and is therefore exact. No occlusion except at the borders, which
are excluded from the score. Real texture, known answer, arbitrary speed.
"""
import argparse, os, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, "..", "prototype", "silkplay.exe")

def shift(img, dx):
    # Integer shifts only: a fractional shift would need resampling, and then the
    # "ground truth" would carry the resampler's own error and flatter or punish
    # the engine for something that is not motion estimation.
    return np.roll(img, dx, axis=1)

def psnr(a, b, guard):
    a = a[:, guard:-guard].astype(np.float64); b = b[:, guard:-guard].astype(np.float64)
    e = ((a - b) ** 2).mean()
    return float("inf") if e <= 0 else 10 * np.log10(255.0 * 255.0 / e)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="corpus/R2_meridian/000100.png")
    ap.add_argument("--speeds", default="8,16,32,48,64")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--run-dir", default="runs/panprobe")
    ap.add_argument("--motion", default="blocks",
                    help="blocks | ofa | ofa+hints")
    args = ap.parse_args()

    src = np.asarray(Image.open(os.path.join(HERE, args.src)).convert("RGB"))
    out = []
    for v in [int(x) for x in args.speeds.split(",")]:
        d = os.path.join(HERE, args.run_dir, f"v{v}")
        ind = os.path.join(d, "in"); os.makedirs(ind, exist_ok=True)
        for i in range(args.frames):
            Image.fromarray(shift(src, i * v)).save(os.path.join(ind, f"{i:06d}.png"))
        od = os.path.join(d, "out")
        motion = "ofa" if args.motion == "ofa+hints" else args.motion
        cmd = [ENGINE, "--offline", ind, "--mode", "mc", "--motion", motion,
               "--offline-out", od, "--offline-t", "0.5"]
        if args.motion == "ofa+hints":
            cmd += ["--ofa-hints"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"v={v}: engine failed\n{r.stdout[-800:]}\n{r.stderr[-400:]}"); continue
        # v must be EVEN for t=0.5 to land on an integer shift; an odd v would put
        # the truth half a pixel away and charge the engine for our own rounding.
        guard = v + 8
        sc, bl = [], []
        for i in range(args.frames - 1):
            p = os.path.join(od, f"pair{i:04d}_t050.png")
            if not os.path.exists(p): continue
            e = np.asarray(Image.open(p).convert("RGB"))
            gt = shift(src, i * v + v // 2)
            a, b = shift(src, i * v), shift(src, (i + 1) * v)
            sc.append(psnr(e, gt, guard))
            bl.append(psnr(((a.astype(np.float64) + b) / 2).round().astype(np.uint8), gt, guard))
        if sc:
            out.append((v, np.mean(sc), np.mean(bl)))
            print(f"  v={v:3d} px/interval : mc {np.mean(sc):6.2f} dB   cross-fade {np.mean(bl):6.2f} dB   "
                  f"delta {np.mean(sc)-np.mean(bl):+6.2f}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
