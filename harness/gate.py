"""Run one engine configuration across every analytic scene and print one matrix.

Why this exists. P14 (the occlusion decision rule) and the period lock found on
A4 are both changes to the same shader, and both are the kind of change that
buys dB in one stratum by spending them in another. Scoring a candidate on the
scene it was designed for is how this project got a block matcher that lost to a
cross-fade: `kb/quality-harness.md` records that a prior tuned against ONE clip
produced contradictory results twice in a row.

So the unit of judgement is the MATRIX, never a scalar. One row per scene, one
column per mask, and a delta against a stored baseline so a regression is
visible without arithmetic. Nothing here pools scenes into a headline number --
`kb/quality-harness.md` rules out any pooled scalar at any stage, and the whole
point is that a design can win A4 by 11 dB and still be rejected for losing 0.5
on A3.

The scene set and what each one is FOR:

  A1  translate-linear    no occlusion to speak of; the control. A candidate that
                          moves A1 is touching something it did not mean to.
  A3  occlusion-passover  the P14 scene. Hard-edged foreground over a hard-edged
                          STATIC background, square wave along y while motion is
                          along x -- so it is structurally incapable of exposing
                          a period lock, which is why the lock survived until A4.
  A4  text-over-motion    static overlay over a MOVING background whose square
                          wave runs along the direction of the pan. The inverse
                          of A3 in two independent ways, and the scene that
                          exposed the period lock.
  A5  grating-aliasing    ADVERSARIAL and marked so in its clip.json: the grating
                          advances exactly one period per interval, |A-B| is zero
                          everywhere and the true motion is genuinely
                          unrecoverable. No arm can be scored as WRONG here, so
                          A5 is never a pass/fail gate. It is printed because a
                          design that suddenly does much BETTER on A5 is claiming
                          the impossible and is telling you it is overfitted.

A3 and A4 together are the pair that matters: A3's background is static and A4's
moves, so any rule that quietly assumes a static background wins one and loses
the other. That is the specific overfit this file exists to catch.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# scene id -> (corpus dir, is it a gate or is it informational only)
SCENES = [
    ("A1", "corpus/A1_p14", True),
    ("A3", "corpus/A3_occl", True),
    ("A4", "corpus/A4_p14", True),
    ("A5", "corpus/A5_p14", False),
]

MASKS = ("both", "occluded", "only_a", "only_b")

# The scorer prints a SUMMARY block per mask; this pulls the mean out of it
# rather than re-implementing the scoring, so gate.py can never disagree with
# run_analytic.py about what a number means.
_MASK_RE = re.compile(r"^--- MASK: (\w+) ---$")
_ROW_RE = re.compile(r"^(\S+)\s+(-|[-\d.]+)\s+(-|[-\d.]+)\s+(-|[-\d.]+)\s+(-|[-\d.]+)\s+(-|\d+)\s*$")


def parse_summary(text):
    """-> {mask: {arm: mean_db}} from run_analytic.py's SUMMARY section."""
    out = {}
    mask = None
    in_summary = False
    for line in text.splitlines():
        line = line.rstrip()
        if line.strip() == "SUMMARY":
            in_summary = True
            continue
        if not in_summary:
            continue
        m = _MASK_RE.match(line.strip())
        if m:
            mask = m.group(1)
            out.setdefault(mask, {})
            continue
        if mask is None or line.startswith("arm ") or not line.strip():
            continue
        m = _ROW_RE.match(line.strip())
        if m and m.group(2) != "-":
            out[mask][m.group(1)] = float(m.group(2))
    return out


def run_scene(scene, corpus, arms, run_root, extra):
    run_dir = os.path.join(run_root, scene)
    cmd = [sys.executable, os.path.join(HERE, "run_analytic.py"),
           "--corpus", corpus, "--arms", arms, "--run-dir", run_dir] + extra
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=HERE)
    if r.returncode != 0:
        # A scene that failed to score is not a scene that scored zero. Say so
        # loudly: a silently dropped row reads as "covered everything".
        sys.stderr.write(f"\n!! scene {scene} FAILED (exit {r.returncode})\n")
        sys.stderr.write(r.stdout[-3000:] + "\n" + r.stderr[-2000:] + "\n")
        return None
    return parse_summary(r.stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", required=True,
                    help="comma-separated mode:motion[:occ] specs, same syntax as run_analytic.py")
    ap.add_argument("--tag", required=True, help="name for this configuration, e.g. 'baseline' or 'select-v1'")
    ap.add_argument("--against", default=None, help="tag of a previous gate run to diff against")
    ap.add_argument("--runs", default="runs/gate")
    ap.add_argument("--scenes", default=None, help="comma-separated subset, e.g. A3,A4")
    args = ap.parse_args()

    want = set(args.scenes.split(",")) if args.scenes else None
    scenes = [s for s in SCENES if want is None or s[0] in want]

    run_root = os.path.join(HERE, args.runs, args.tag)
    os.makedirs(run_root, exist_ok=True)

    results = {}
    for scene, corpus, _gate in scenes:
        print(f"scoring {scene} ...", flush=True)
        got = run_scene(scene, corpus, args.arms, run_root, [])
        if got is not None:
            results[scene] = got

    base = None
    if args.against:
        bp = os.path.join(HERE, args.runs, args.against, "gate.json")
        if os.path.exists(bp):
            with open(bp, encoding="utf-8") as f:
                base = json.load(f)["results"]
        else:
            print(f"\n!! no baseline at {bp}; printing absolute numbers only")

    arms = [a.strip() for a in args.arms.split(",")]
    ref_arms = ["hold-A", "blend:ofa"]

    print()
    print(f"=== GATE MATRIX  tag={args.tag}" + (f"  vs {args.against}" if base else "") + " ===")
    for scene, _corpus, is_gate in scenes:
        if scene not in results:
            continue
        flag = "" if is_gate else "   (INFORMATIONAL ONLY - adversarial, no arm can be wrong here)"
        print(f"\n-- {scene}{flag}")
        cols = [m for m in MASKS if m in results[scene]]
        print("%-24s %s" % ("arm", "".join("%18s" % c for c in cols)))
        for arm in arms + [a for a in ref_arms if a not in arms]:
            cells = []
            for m in cols:
                v = results[scene].get(m, {}).get(arm)
                if v is None:
                    cells.append("%18s" % "-")
                    continue
                d = None
                if base and scene in base and m in base[scene] and arm in base[scene][m]:
                    d = v - base[scene][m][arm]
                cells.append("%18s" % (f"{v:7.2f} {d:+6.2f}" if d is not None else f"{v:7.2f}       "))
            print("%-24s %s" % (arm, "".join(cells)))

    out = os.path.join(run_root, "gate.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"tag": args.tag, "arms": arms, "results": results}, f, indent=2)
    print(f"\nwrote {out}")

    # A missing scene is a failed gate run, not a passed one.
    return 0 if len(results) == len(scenes) else 2


if __name__ == "__main__":
    sys.exit(main())
