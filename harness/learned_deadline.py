"""Measure exact-phase RIFE synthesis against a refresh interval, without a presenter.

Resident source pair, optionally resized for a timing-only full-resolution load.
This is a necessary GPU budget check, never a claim of achieved display cadence.
"""
import argparse
import json
from pathlib import Path
import time

import numpy as np
import torch
import torch.nn.functional as F

from learned_probe import Candidate, tensor


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--size', required=True, help='WIDTHxHEIGHT, timing-only resize')
    ap.add_argument('--refresh', type=float, default=164.999)
    ap.add_argument('--source-fps', type=float, default=30)
    ap.add_argument('--frames', type=int, default=330)
    args = ap.parse_args()
    w, h = (int(n) for n in args.size.split('x'))
    if min(w, h, args.frames, args.refresh, args.source_fps) <= 0:
        ap.error('sizes, frames and rates must be positive')
    torch.set_num_threads(2)
    torch.backends.cudnn.benchmark = True
    args.model, args.precision, args.backend = 'rife-fields', 'bf16', 'trt-rtx'
    args.max_side, args.phase = 768, 0.5
    frames = sorted(Path(args.corpus).glob('*.png'))
    if len(frames) < 3:
        ap.error('need at least three source PNGs')
    a, b = [F.interpolate(tensor(frames[i], torch.float32), size=(h, w), mode='bilinear',
                          align_corners=False).contiguous() for i in (0, 2)]
    candidate = Candidate(args, h, w)
    budget = 1000 / args.refresh
    gpu, wall, prediction, render, phases = [], [], [], [], []
    with torch.inference_mode(), torch.autocast('cuda', dtype=torch.bfloat16):
        for _ in range(15):
            candidate(a, b)
        torch.cuda.synchronize()
        for i in range(args.frames):
            phase = ((i + 0.5) * args.source_fps / args.refresh) % 1
            args.phase = phase
            first, last = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
            begin = time.perf_counter()
            first.record()
            output = candidate(a, b)
            last.record()
            last.synchronize()
            wall.append((time.perf_counter() - begin) * 1000)
            gpu.append(first.elapsed_time(last))
            s, m, e = candidate.stage_events
            prediction.append(s.elapsed_time(m))
            render.append(m.elapsed_time(e))
            phases.append(phase)
        if not torch.isfinite(output).all():
            raise RuntimeError('non-finite synthesis output')
    def summary(values):
        return {str(p): float(np.percentile(values, p)) for p in (50, 95, 99, 100)}
    report = {'gpu': torch.cuda.get_device_name(), 'source_size': [w, h], 'model_size': [candidate.w, candidate.h],
              'refresh_hz': args.refresh, 'budget_ms': budget, 'frames': args.frames,
              'scope': 'resident-pair synthesis; timing-only resized source; no capture, interop, or display',
              'gpu_ms_percentiles': summary(gpu), 'wall_ms_percentiles': summary(wall),
              'predict_ms_percentiles': summary(prediction), 'render_ms_percentiles': summary(render),
              'gpu_over_budget': sum(t > budget for t in gpu), 'wall_over_budget': sum(t > budget for t in wall),
              'gpu_ms': gpu, 'wall_ms': wall, 'phase': phases}
    destination = Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k not in ('gpu_ms', 'wall_ms', 'phase')}, indent=2))


if __name__ == '__main__':
    main()
