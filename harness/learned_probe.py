"""Evaluate local learned VFI candidates; never changes the shipping engine.

Run with .venv-torch/Scripts/python.exe. Checkpoints stay in ignored directories.
CUDA-event timings exclude disk IO, PNG encoding and CPU scoring; wall timings
include model submission and GPU completion. Each clip remains a separate row.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import statistics
import sys
import time

import numpy as np
from PIL import Image
import torch
import torch.nn.functional as F

from warp_lab import spurious_edges

ROOT = Path(__file__).resolve().parent.parent
RIFE_MODELS = ("rife", "rife-pair", "rife-fields")


class FP32Deconvolution(torch.nn.Module):
    def __init__(self, layer):
        super().__init__()
        self.layer = layer.float()

    def forward(self, value):
        with torch.autocast("cuda", enabled=False):
            return self.layer(value.float()).to(value.dtype)


def fp32_deconvolutions(module):
    # TensorRT-RTX 1.7 rejected the BF16 ConvTranspose graph. Keep explicit
    # FP32 islands around those layers; do not hide a failed build with a fallback.
    for name, child in list(module.named_children()):
        if isinstance(child, torch.nn.ConvTranspose2d):
            setattr(module, name, FP32Deconvolution(child))
        else:
            fp32_deconvolutions(child)


def tensor(path, dtype):
    pixels = np.asarray(Image.open(path).convert("RGB")).copy()
    return torch.from_numpy(pixels).permute(2, 0, 1)[None].to("cuda", dtype=dtype) / 255.0


def sample(image, flow):
    n, _, h, w = flow.shape
    yy, xx = torch.meshgrid(torch.arange(h, device="cuda"), torch.arange(w, device="cuda"), indexing="ij")
    grid = torch.stack((xx, yy), 0)[None] + flow.float()
    grid = torch.stack((2 * grid[:, 0] / max(w - 1, 1) - 1,
                        2 * grid[:, 1] / max(h - 1, 1) - 1), -1)
    return F.grid_sample(image.float(), grid, padding_mode="border", align_corners=True)


def splat(image, flow, phase, importance):
    """Bilinear forward projection; exact coverage is retained, not silently filled."""
    _, _, h, w = image.shape
    yy, xx = torch.meshgrid(torch.arange(h, device="cuda"), torch.arange(w, device="cuda"), indexing="ij")
    x, y = xx + phase * flow[0, 0], yy + phase * flow[0, 1]
    x0, y0 = x.floor(), y.floor()
    accum = torch.zeros((4, h * w), device="cuda")
    payload = torch.cat((image[0].float(), torch.ones((1, h, w), device="cuda")), 0)
    for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        xi, yi = x0 + dx, y0 + dy
        valid = (xi >= 0) & (xi < w) & (yi >= 0) & (yi < h)
        weight = (1 - (x - xi).abs()) * (1 - (y - yi).abs()) * importance[0, 0] * valid
        index = (yi.clamp(0, h - 1).long() * w + xi.clamp(0, w - 1).long()).reshape(1, -1)
        accum.scatter_add_(1, index.expand(4, -1), (payload * weight).reshape(4, -1))
    return accum.reshape(1, 4, h, w)


class Candidate:
    def __init__(self, args, h, w):
        self.args = args
        self.dtype = {"fp16": torch.float16, "fp32": torch.float32, "bf16": torch.bfloat16}[args.precision]
        self.original_size = (h, w)
        ratio = min(1.0, args.max_side / max(h, w)) if args.max_side else 1.0
        self.h, self.w = math.ceil(h * ratio / 32) * 32, math.ceil(w * ratio / 32) * 32
        self.last_holes = 0.0
        if args.model == "vfg":
            from nvvfx import VideoFrameGeneration
            if not args.max_side:
                self.h, self.w = h, w
            self.dtype = torch.float32
            self.checkpoint = None
            self.model = VideoFrameGeneration(getattr(VideoFrameGeneration.Mode, args.vfg_mode.upper()),
                automatic_shot_change_detection_enabled=not args.vfg_no_auto_cut)
            self.model.input_width, self.model.input_height = self.w, self.h
            self.model.load()
            return
        if args.model in RIFE_MODELS:
            folder = ROOT / "third_party/flow/Practical-RIFE"
            sys.path.insert(0, str(folder))
            from train_log.IFNet_HDv3 import IFNet
            self.model = IFNet()
            self.checkpoint = folder / "train_log/flownet.pkl"
            state = torch.load(self.checkpoint, map_location="cpu", weights_only=True)
            state = {k.removeprefix("module."): v for k, v in state.items()}
            # The author's checkpoint also contains the two commented-out training heads.
            training_keys = [k for k in state if k.startswith(("teacher.", "caltime."))]
            print(f"RIFE: excluding {len(training_keys)} training-only tensors", flush=True)
            state = {k: v for k, v in state.items() if k not in training_keys}
            self.model.load_state_dict(state, strict=True)
        elif args.model == "neuflow":
            sys.path.insert(0, str(ROOT / "third_party/flow/NeuFlow_v2"))
            from NeuFlow.neuflow import NeuFlow
            from NeuFlow.backbone_v7 import ConvBlock
            from safetensors.torch import load_file
            self.model = NeuFlow()
            self.checkpoint = ROOT / "third_party/flow-weights/neuflow-v2/model.safetensors"
            self.model.load_state_dict(load_file(str(self.checkpoint)), strict=True)
            self.model.eval()
            for module in self.model.modules():
                if isinstance(module, ConvBlock):
                    module.conv1 = torch.nn.utils.fuse_conv_bn_eval(module.conv1, module.norm1)
                    module.conv2 = torch.nn.utils.fuse_conv_bn_eval(module.conv2, module.norm2)
                    module.forward = module.forward_fuse
        else:
            folder = Path(args.framefusion_dir).resolve()
            spec = importlib.util.spec_from_file_location("nova_framefusion_reference", folder / "source.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            self.checkpoint = folder / "Comfy/ComfyUI_FrameFusion/models/anime_predictor.safetensors"
            self.model = module.Handler(str(self.checkpoint))
            self.h, self.w = self.model._resolve_model_size(h, w)
            self.renderer = module.FrameRenderer(self.model.kernel_size).cuda().eval()
        self.model = self.model.cuda().eval().to(dtype=self.dtype)
        if self.dtype == torch.bfloat16:
            fp32_deconvolutions(self.model)
        self.trt_runtime = None
        if args.backend == "trt-rtx":
            if args.model not in RIFE_MODELS:
                raise ValueError("TensorRT-RTX export is currently implemented for RIFE only")
            from trt_rife import RifeRuntime
            self.trt_runtime = RifeRuntime(self.model, self.checkpoint, self.h, self.w, self.dtype,
                                           ROOT / "harness/runs/learned/engines", fields=args.model != "rife")
        if args.model == "neuflow":
            self.model.init_bhwd(2, self.h, self.w, "cuda", amp=self.dtype == torch.float16)

    def __call__(self, a, b):
        if self.args.model == "vfg":
            aa = F.interpolate(a, size=(self.h, self.w), mode="bilinear", align_corners=False)[0].contiguous()
            bb = F.interpolate(b, size=(self.h, self.w), mode="bilinear", align_corners=False)[0].contiguous()
            # The SDK owns/reuses its output; copy before the next inference.
            result = self.model.run_at_timestep(aa, bb, self.args.phase,
                stream_ptr=torch.cuda.current_stream().cuda_stream)
            output = torch.from_dlpack(result.image).clone()[None]
            return F.interpolate(output, size=self.original_size, mode="bilinear", align_corners=False)
        if self.args.model == "framefusion":
            prediction = self.model(a, b)
            # Its API counts intermediate frames, so index 0 of ONE frame is t=0.5.
            # Frame2Cpu returns a CUDA BHWC uint8 tensor despite its name.
            image = self.renderer(a, b, prediction, 0, 1)
            return image.permute(0, 3, 1, 2).float() / 255.0
        aa = F.interpolate(a, size=(self.h, self.w), mode="bilinear", align_corners=False).to(self.dtype).contiguous()
        bb = F.interpolate(b, size=(self.h, self.w), mode="bilinear", align_corners=False).to(self.dtype).contiguous()
        if self.args.model in RIFE_MODELS:
            if self.args.model != "rife":
                start, middle, end = (torch.cuda.Event(enable_timing=True) for _ in range(3))
                start.record()
            if self.trt_runtime:
                image = self.trt_runtime(aa, bb, 0.5 if self.args.model == "rife-pair" else self.args.phase)
            else:
                flow, mask, merged = self.model(torch.cat((aa, bb), 1),
                    timestep=0.5 if self.args.model == "rife-pair" else self.args.phase,
                    scale_list=[16, 8, 4, 2, 1])
                image = torch.cat((flow[-1], mask), 1).float() if self.args.model != "rife" else merged[-1]
            if self.args.model != "rife":
                h, w = self.original_size
                fields = F.interpolate(image, size=(h, w), mode="bilinear", align_corners=False)
                scale = torch.tensor([w / self.w, h / self.h, w / self.w, h / self.h, 1], device="cuda")
                fields = fields * scale[None, :, None, None]
                middle.record()
                output = self.render_pair(a, b, fields, self.args.phase) if self.args.model == "rife-pair" else self.render_fields(a, b, fields)
                end.record()
                self.stage_events = (start, middle, end)
                return output
            # This arm resizes synthesized RGB; that quality cost must stay in the score.
            return F.interpolate(image, size=self.original_size, mode="bilinear", align_corners=False)
        # NeuFlow's published implementation expects BGR 0..255 and normalizes IN PLACE.
        ab = torch.cat((aa, bb), 0).flip(1) * 255
        ba = torch.cat((bb, aa), 0).flip(1) * 255
        flow = self.model(ab, ba, iters_s8=self.args.iterations)[-1].float()
        h, w = self.original_size
        flow = F.interpolate(flow, size=(h, w), mode="bilinear", align_corners=False)
        flow = flow * torch.tensor([w / self.w, h / self.h], device="cuda")[None, :, None, None]
        fa, fb = flow[:1], flow[1:]
        ca = (fa + sample(fb, fa)).square().sum(1, keepdim=True).sqrt()
        cb = (fb + sample(fa, fb)).square().sum(1, keepdim=True).sqrt()
        # Cycle consistency is visibility evidence, not an oracle for foreground depth.
        wa = torch.exp(-ca.clamp(max=12))
        wb = torch.exp(-cb.clamp(max=12))
        sa, sb = splat(a, fa, 0.5, wa), splat(b, fb, 0.5, wb)
        combined = sa + sb
        covered = combined[:, 3:] > 1e-4
        self.holes = ~covered
        return torch.where(covered, combined[:, :3] / combined[:, 3:].clamp_min(1e-4),
                           (a.float() + b.float()) * 0.5)

    @staticmethod
    def render_fields(a, b, fields):
        """Fields predicted for THIS phase; source RGB is never resized for synthesis."""
        mask = fields[:, 4:].sigmoid()
        return sample(a, fields[:, :2]) * mask + sample(b, fields[:, 2:4]) * (1 - mask)

    @staticmethod
    def render_pair(a, b, fields, phase):
        if phase <= 0:
            return a.float()
        if phase >= 1:
            return b.float()
        dt = phase - 0.5
        if dt:
            velocity = fields[:, 2:4] - fields[:, :2]
            offset = -dt * velocity
            for _ in range(3):
                offset = -dt * sample(velocity, offset)
            fields = sample(fields, offset)
        velocity = fields[:, 2:4] - fields[:, :2]
        image_a = sample(a, fields[:, :2] - dt * velocity)
        image_b = sample(b, fields[:, 2:4] - dt * velocity)
        mask = fields[:, 4:].sigmoid()
        wa, wb = (1 - phase) * mask, phase * (1 - mask)
        return (image_a * wa + image_b * wb) / (wa + wb).clamp_min(1e-6)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", choices=(*RIFE_MODELS, "neuflow", "framefusion", "vfg"), required=True)
    ap.add_argument("--phase", type=float, default=0.5, help="withheld-frame phase (arbitrary phase supported by RIFE and VFG)")
    ap.add_argument("--vfg-mode", choices=("low", "medium", "high"), default="medium")
    ap.add_argument("--vfg-no-auto-cut", action="store_true")
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-side", type=int, default=768, help="0 keeps source resolution (rounded up to 32)")
    ap.add_argument("--precision", choices=("fp16", "fp32", "bf16"), default="fp16")
    ap.add_argument("--backend", choices=("pytorch", "trt-rtx"), default="pytorch")
    ap.add_argument("--iterations", type=int, default=8)
    ap.add_argument("--count", type=int, default=16)
    ap.add_argument("--stride", type=int, default=2)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--bench", type=int, default=15)
    ap.add_argument("--repeat-check", type=int, default=3, help="repeat the first warmed pair and compare output pixels")
    ap.add_argument("--framefusion-dir")
    args = ap.parse_args()
    if args.stride < 2 or args.stride % 2 or args.count < 2:
        ap.error("need an even stride >=2 and count >=2")
    if args.bench < 1 or args.warmup < 1 or args.max_side < 0 or args.iterations < 1:
        ap.error("need positive bench/warmup/iterations and max-side >=0")
    if args.repeat_check < 1:
        ap.error("repeat-check must be positive")
    if args.model == "framefusion" and not args.framefusion_dir:
        ap.error("--framefusion-dir is required for FrameFusion")
    if args.backend == "trt-rtx" and args.model not in RIFE_MODELS:
        ap.error("--backend trt-rtx is implemented for RIFE only")
    if args.precision == "bf16" and args.model not in RIFE_MODELS:
        ap.error("BF16 is currently evaluated for RIFE only")
    if not 0 < args.phase < 1 or args.phase * args.stride != int(args.phase * args.stride):
        ap.error("phase must identify a withheld corpus frame inside the source interval")
    if args.phase != 0.5 and args.model not in (*RIFE_MODELS, "vfg"):
        ap.error("non-midpoint probe is implemented for RIFE and VFG only")
    if not torch.cuda.is_available():
        raise SystemExit("CUDA required; refusing a silent CPU fallback")
    torch.set_num_threads(2)
    torch.backends.cudnn.benchmark = True
    corpus = Path(args.corpus).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    frames = sorted(corpus.glob("*.png"))
    stop = args.first + (args.count - 1) * args.stride
    if stop >= len(frames):
        raise SystemExit(f"need source frame {stop}, corpus only has {len(frames)} PNGs")
    w, h = Image.open(frames[args.first]).size
    candidate = Candidate(args, h, w)
    # In field arms reduced precision belongs to the estimator, not to source RGB.
    color_dtype = torch.float32 if args.model in ("rife-pair", "rife-fields") else candidate.dtype
    print(f"{args.model}: {torch.cuda.get_device_name()}, source {w}x{h}, model {candidate.w}x{candidate.h}", flush=True)
    a, b = (tensor(frames[args.first + i * args.stride], color_dtype) for i in (0, 1))
    gpu_ms, wall_ms, prepare_ms, render_ms = [], [], [], []
    # NeuFlow's published half path expects half LayerNorm outputs, not AMP's fp32 ones.
    # The current VFX wrapper initializes on stream 0 during load(). Keep that
    # stream until the wrapper exposes setting it before load; other arms use a worker stream.
    stream = torch.cuda.default_stream() if args.model == "vfg" else torch.cuda.Stream()
    stream.wait_stream(torch.cuda.current_stream())
    with torch.cuda.stream(stream), torch.inference_mode(), torch.autocast("cuda",
            enabled=candidate.dtype != torch.float32 and args.model in RIFE_MODELS,
            dtype=candidate.dtype if candidate.dtype != torch.float32 else torch.float16):
        for _ in range(args.warmup):
            candidate(a, b)
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()
        for _ in range(args.bench):
            start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
            t = time.perf_counter()
            start.record()
            prediction = candidate(a, b)
            end.record()
            end.synchronize()
            gpu_ms.append(start.elapsed_time(end))
            wall_ms.append((time.perf_counter() - t) * 1000)
            if args.model in ("rife-pair", "rife-fields"):
                first, mid, last = candidate.stage_events
                prepare_ms.append(first.elapsed_time(mid))
                render_ms.append(mid.elapsed_time(last))
        if not torch.isfinite(prediction).all():
            raise RuntimeError("non-finite model output")
        reference_pixels = prediction.float().clamp(0, 1).mul(255).round().clone()
        repeat_error = 0.0
        for _ in range(args.repeat_check):
            repeated = candidate(a, b)
            if not torch.isfinite(repeated).all():
                raise RuntimeError("non-finite output during repeat check")
            repeated_pixels = repeated.float().clamp(0, 1).mul(255).round()
            repeat_error = max(repeat_error, float((repeated_pixels - reference_pixels).abs().max().item()))
        rows, mse = [], []
        for pair in range(args.count - 1):
            index = args.first + pair * args.stride
            a, b = tensor(frames[index], color_dtype), tensor(frames[index + args.stride], color_dtype)
            prediction = candidate(a, b)
            if not torch.isfinite(prediction).all():
                raise RuntimeError(f"non-finite output at pair {pair}")
            pixels = prediction[0].float().clamp(0, 1).mul(255).round().byte().permute(1, 2, 0).cpu().numpy()
            truth = np.asarray(Image.open(frames[index + int(args.stride * args.phase)]).convert("RGB"))
            error = pixels.astype(np.float64) - truth
            pair_mse = float(np.square(error).mean())
            mse.append(pair_mse)
            edge, hot = spurious_edges(pixels.astype(np.int16), truth.astype(np.int16))
            row = {"pair": pair, "source_index": index,
                   "psnr_db": 10 * math.log10(255**2 / max(pair_mse, 1e-9)),
                   "big_error_px_pct": float((np.abs(error).max(2) > 40).mean() * 100),
                   "spurious_edge_mean": edge, "spurious_edge_gt24_pct": float(hot * 100)}
            source_a = np.asarray(Image.open(frames[index]).convert("RGB"))
            source_b = np.asarray(Image.open(frames[index + args.stride]).convert("RGB"))
            row["exact_hold_a"] = bool(np.array_equal(pixels, source_a))
            row["exact_hold_b"] = bool(np.array_equal(pixels, source_b))
            row["identical_inputs"] = bool(np.array_equal(source_a, source_b))
            if args.model == "neuflow":
                row["splat_holes_pct"] = float(candidate.holes.float().mean().item() * 100)
            rows.append(row)
            Image.fromarray(pixels).save(out / f"pair{pair:04d}_t{round(args.phase * 100):03d}.png")
    summary = {"model": args.model, "backend": args.backend, "gpu": torch.cuda.get_device_name(), "torch": torch.__version__,
               "precision": args.precision if args.model != "vfg" else "SDK-defined",
               "input_dtype": str(candidate.dtype),
               "source_color_dtype": str(color_dtype),
               "source_size": [w, h], "network_size": [candidate.w, candidate.h],
               "precision_policy": ("SDK internal precision; float32 RGB API" if args.model == "vfg" else
                                    "BF16 with FP32 deconvolutions" if args.precision == "bf16" else args.precision),
               "corpus": str(corpus), "first": args.first, "stride": args.stride, "pairs": len(rows),
               "phase": args.phase,
               "checkpoint_sha256": hashlib.sha256(candidate.checkpoint.read_bytes()).hexdigest() if candidate.checkpoint else None,
               "vfg_mode": args.vfg_mode if args.model == "vfg" else None,
               "vfg_auto_cut": not args.vfg_no_auto_cut if args.model == "vfg" else None,
               "gpu_ms_median": statistics.median(gpu_ms), "gpu_ms_p95": float(np.percentile(gpu_ms, 95)),
               "wall_ms_median": statistics.median(wall_ms),
               "repeat_check": args.repeat_check, "repeat_pixel_max_error": repeat_error,
               "torch_peak_allocated_mib": torch.cuda.max_memory_allocated() / 1024**2,
               "psnr_db": 10 * math.log10(255**2 / max(statistics.mean(mse), 1e-9)),
               "per_pair": rows}
    if prepare_ms:
        summary["prepare_gpu_ms_median"] = statistics.median(prepare_ms)
        summary["render_gpu_ms_median"] = statistics.median(render_ms)
    for key in ("big_error_px_pct", "spurious_edge_mean", "spurious_edge_gt24_pct"):
        summary[key] = statistics.mean(row[key] for row in rows)
    (out / "metrics.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "per_pair"}, indent=2), flush=True)


if __name__ == "__main__":
    main()
