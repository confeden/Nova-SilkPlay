"""Serial cross-clip and arbitrary-phase checks for learned VFI probes.

The CUDA models are never run concurrently. A failed case remains a failed row;
it is not replaced with a fallback or omitted from the manifest.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="harness/runs/learned/matrix-v1")
    ap.add_argument("--rife-fields", action="store_true", help="focused BF16 exact-phase field validation")
    args = ap.parse_args()
    root = Path(args.out).resolve()
    root.mkdir(parents=True, exist_ok=True)
    cases = []
    for clip in ("R2_meridian", "R4_spring245"):
        for model in ("rife", "rife-pair", "vfg"):
            options = ["--model", model, "--corpus", str(HERE / "corpus" / clip), "--count", "32"]
            if model == "vfg":
                options += ["--vfg-no-auto-cut", "--max-side", "0"]
            else:
                options += ["--backend", "trt-rtx", "--precision", "fp32", "--max-side", "768"]
            cases.append((f"{model}-{clip}", options))
    for phase in ("0.25", "0.75"):
        for model in ("rife", "rife-pair"):
            cases.append((f"{model}-R3-stride4-t{phase}", ["--model", model,
                "--backend", "trt-rtx", "--precision", "fp32", "--max-side", "768",
                "--corpus", str(HERE / "corpus/R3_bbb60_720crop"),
                "--stride", "4", "--phase", phase, "--count", "16"]))
    for mode in ("low", "high"):
        cases.append((f"vfg-{mode}-noauto-R3", ["--model", "vfg", "--vfg-mode", mode,
            "--vfg-no-auto-cut", "--max-side", "0", "--count", "32",
            "--corpus", str(HERE / "corpus/R3_bbb60_720crop")]))
    cases.append(("rife-pair-1440-timing-only", ["--model", "rife-pair", "--backend", "trt-rtx",
        "--precision", "fp32", "--max-side", "768", "--count", "2", "--bench", "31",
        "--corpus", str(HERE / "runs/startup-final-checks/sources1440")]))
    if args.rife_fields:
        common = ["--model", "rife-fields", "--backend", "trt-rtx", "--precision", "bf16", "--max-side", "768"]
        cases = [(f"fields-bf16-{clip}", [*common, "--corpus", str(HERE / "corpus" / clip), "--count", "32"])
                 for clip in ("R2_meridian", "R4_spring245")]
        cases += [(f"fields-bf16-R3-stride4-t{phase}", [*common, "--corpus", str(HERE / "corpus/R3_bbb60_720crop"),
                    "--stride", "4", "--phase", phase, "--count", "16"]) for phase in ("0.25", "0.75")]
    records = []
    failed = False
    for index, (name, options) in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {name}", flush=True)
        destination = root / name
        destination.mkdir(exist_ok=True)
        command = [sys.executable, str(HERE / "learned_probe.py"), *options, "--out", str(destination)]
        with (destination / "run.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        record = {"case": name, "command": command, "exit_code": result.returncode}
        if result.returncode:
            failed = True
            print(f"FAILED: {name}\n" + "\n".join((destination / "run.log").read_text(encoding="utf-8").splitlines()[-12:]), flush=True)
        else:
            metrics = json.loads((destination / "metrics.json").read_text(encoding="utf-8"))
            record["metrics"] = metrics
            print(f"  {metrics['psnr_db']:.2f} dB; big-error {metrics['big_error_px_pct']:.4f}%; "
                  f"GPU {metrics['gpu_ms_median']:.2f} ms", flush=True)
        records.append(record)
        (root / "matrix.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
