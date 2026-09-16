"""score.py – score screen recordings of frame-generation engines against analytic ground truth."""
import argparse, json, math, os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../harness/gen'))
import analytic

BORDER = 8
GSS_ITERS = 7
GR = (math.sqrt(5) - 1) / 2  # golden ratio conjugate
STEP = [1.0]  # scene time per source frame (clip.json 'step'); set in main()


def block_mean_4x4(rgb_f32):
    h, w = rgb_f32.shape[:2]
    bh, bw = h // 4, w // 4
    r = rgb_f32[:bh*4, :bw*4].reshape(bh, 4, bw, 4, 3)
    return r.mean(axis=(1, 3))


def psnr(a, b, mask):
    """PSNR over masked pixels (float64 RGB). Returns 99.0 if MSE==0, NaN if empty."""
    if not mask.any():
        return float('nan')
    diff = a[mask].astype(np.float64) - b[mask].astype(np.float64)
    mse = float((diff ** 2).mean())
    return 99.0 if mse == 0.0 else 10 * math.log10(255.0 ** 2 / mse)


def build_coarse_table(scene, w, h, frames, cache_dir):
    key = f"{scene.scene_id}_{w}x{h}_s{scene.seed}_f{frames}_step{STEP[0]:g}_q8.npy"
    path = os.path.join(cache_dir, key)
    n = 8 * (frames - 1) + 1  # j in 0 .. 8*(frames-1)
    if os.path.exists(path):
        table = np.load(path)
        if table.shape[0] == n:
            return table
    os.makedirs(cache_dir, exist_ok=True)
    bh, bw = h // 4, w // 4
    table = np.zeros((n, bh, bw, 3), dtype=np.float32)
    for j in range(n):
        t = j / 8.0
        r = analytic.render(scene, t * STEP[0])
        table[j] = block_mean_4x4(r['rgb'].astype(np.float32))
        if j % 8 == 0:
            print(f"  coarse table {j}/{n-1} (t={t:.3f})", flush=True)
    np.save(path, table)
    return table


def coarse_match(frame_f32, table):
    blk = block_mean_4x4(frame_f32)
    diff = table - blk[None]
    mse = (diff ** 2).mean(axis=(1, 2, 3))
    return int(np.argmin(mse))


def golden_section_search(frame_f32, scene, t_lo, t_hi, scoring_mask, render_cache):
    def score(t):
        t = round(t, 6)
        if t not in render_cache:
            render_cache[t] = analytic.render(scene, t * STEP[0])
        gt = render_cache[t]['rgb'].astype(np.float64)
        return psnr(frame_f32, gt, scoring_mask)

    # Probe integer t values in the window first — the PSNR spike at an exact
    # source frame is infinitely sharp; GSS misses it unless seeded there.
    best_t, best_p = t_lo, score(t_lo)
    for k_int in range(math.ceil(t_lo), math.floor(t_hi) + 1):
        if t_lo <= k_int <= t_hi:
            p = score(float(k_int))
            if p > best_p:
                best_p, best_t = p, float(k_int)

    a, b = t_lo, t_hi
    c = b - GR * (b - a)
    d = a + GR * (b - a)
    fc, fd = score(c), score(d)
    for _ in range(GSS_ITERS):
        if fc < fd:
            a, b, d, fd = a, d, c, fc
            c = b - GR * (b - a)
            fc = score(c)
        else:
            a, b, c, fc = c, b, d, fd
            d = a + GR * (b - a)
            fd = score(d)
    t_gss = round((a + b) / 2.0, 6)
    if t_gss not in render_cache:
        render_cache[t_gss] = analytic.render(scene, t_gss * STEP[0])
    p_gss = score(t_gss)

    t_best = best_t if best_p >= p_gss else t_gss
    t_best = round(t_best, 6)
    if t_best not in render_cache:
        render_cache[t_best] = analytic.render(scene, t_best * STEP[0])
    return t_best, render_cache[t_best]


def load_csv(prefix):
    rows = []
    with open(prefix + '.csv') as f:
        header_seen = False
        for line in f:
            line = line.rstrip('\r\n')
            if line.startswith('#') or not line:
                continue
            if not header_seen:
                header_seen = True
                continue
            parts = line.split(',')
            rows.append({'index': int(parts[0]), 'qpc_ms': float(parts[1])})
    return rows


def load_raw_frame(prefix, idx, h, w):
    offset = idx * h * w * 4
    with open(prefix + '.raw', 'rb') as f:
        f.seek(offset)
        data = f.read(h * w * 4)
    arr = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 4)
    return arr[..., 2::-1].astype(np.float64)  # BGRA -> RGB float64


def score_arm(name, prefix, scene, clip, max_frames, cache_dir):
    w, h = clip['size']
    frames = clip['frames']
    fps = clip['fps']

    rows = load_csv(prefix)
    total = len(rows)

    # Select subset: skip first 10, take up to max_frames consecutive
    start = min(10, total)
    subset = rows[start:start + max_frames]
    if not subset:
        subset = rows[:max_frames]

    print(f"\n[{name}] Building coarse table...", flush=True)
    table = build_coarse_table(scene, w, h, frames, cache_dir)

    # Scoring border mask
    base_mask = np.zeros((h, w), dtype=bool)
    base_mask[BORDER:h-BORDER, BORDER:w-BORDER] = True

    render_cache = {}
    frame_records = []

    print(f"[{name}] Scoring {len(subset)} frames...", flush=True)
    for row in subset:
        idx = row['index']
        qpc_ms = row['qpc_ms']

        frame_f32 = load_raw_frame(prefix, idx, h, w)

        # Coarse match
        j_star = coarse_match(frame_f32, table)
        t_coarse = j_star / 8.0

        # Seam rule: wrap-around contamination
        if t_coarse < 1.0 or t_coarse > frames - 2.0:
            frame_records.append({
                'index': idx, 'qpc_ms': qpc_ms,
                't_best': None, 'psnr_all': None, 'psnr_moving': None,
                'psnr_static': None, 'psnr_occluded': None,
                'held': None, 'seam': True
            })
            continue

        # Refine
        t_lo = max(0.0, t_coarse - 0.125)
        t_hi = min(float(frames - 1), t_coarse + 0.125)
        t_best, best_render = golden_section_search(
            frame_f32, scene, t_lo, t_hi, base_mask, render_cache)

        gt_rgb = best_render['rgb']
        occl = best_render['occl']
        static_mask = best_render['static_mask']

        # Strata masks
        mask_moving   = base_mask & (static_mask == 0) & (occl == 0)
        mask_static   = base_mask & (static_mask == 1) & (occl == 0)
        mask_occluded = base_mask & (occl > 0)

        p_all      = psnr(frame_f32, gt_rgb.astype(np.float64), base_mask)
        p_moving   = psnr(frame_f32, gt_rgb.astype(np.float64), mask_moving)
        p_static   = psnr(frame_f32, gt_rgb.astype(np.float64), mask_static)
        p_occluded = psnr(frame_f32, gt_rgb.astype(np.float64), mask_occluded)

        held = abs(t_best - round(t_best)) < 0.02

        frame_records.append({
            'index': idx, 'qpc_ms': qpc_ms,
            't_best': t_best,
            'psnr_all': p_all, 'psnr_moving': p_moving,
            'psnr_static': p_static, 'psnr_occluded': p_occluded,
            'held': held, 'seam': False
        })

    # Pacing over consecutive scored (non-seam) frames
    scored = [r for r in frame_records if not r['seam']]
    pacing_errs = []
    generated_count = sum(1 for r in scored if not r['held'])
    wall_span_ms = 0.0

    for i in range(1, len(scored)):
        prev, cur = scored[i-1], scored[i]
        wall_ms = cur['qpc_ms'] - prev['qpc_ms']
        if wall_ms >= 100.0:
            continue
        content_ms = (cur['t_best'] - prev['t_best']) * 1000.0 / fps
        pacing_errs.append(abs(content_ms - wall_ms))

    if len(scored) >= 2:
        wall_span_ms = scored[-1]['qpc_ms'] - scored[0]['qpc_ms']

    wall_span_s = wall_span_ms / 1000.0
    generated_per_s = generated_count / wall_span_s if wall_span_s > 0 else float('nan')

    judder_mean = float(np.mean(pacing_errs)) if pacing_errs else float('nan')
    judder_gt4  = float(np.mean([e > 4.0 for e in pacing_errs])) if pacing_errs else float('nan')

    def nanmean(vals):
        v = [x for x in vals if x is not None and not math.isnan(x)]
        return float(np.mean(v)) if v else float('nan')

    def nanp10(vals):
        v = [x for x in vals if x is not None and not math.isnan(x)]
        return float(np.percentile(v, 10)) if v else float('nan')

    summary = {
        'frames_total': total,
        'frames_scored': len(scored),
        'psnr_all':      nanmean([r['psnr_all'] for r in scored]),
        'psnr_moving':   nanmean([r['psnr_moving'] for r in scored]),
        'psnr_static':   nanmean([r['psnr_static'] for r in scored]),
        'psnr_occluded': nanmean([r['psnr_occluded'] for r in scored]),
        'psnr_all_p10':  nanp10([r['psnr_all'] for r in scored]),
        'held_pct':      100.0 * sum(1 for r in scored if r['held']) / len(scored) if scored else float('nan'),
        'generated_per_s': generated_per_s,
        'judder_mean_ms':  judder_mean,
        'judder_gt4_pct':  100.0 * judder_gt4 if not math.isnan(judder_gt4) else float('nan'),
    }

    out = {'summary': summary, 'frames': frame_records}
    with open(prefix + '_score.json', 'w') as f:
        json.dump(out, f, indent=2)

    return summary


def print_table(results):
    hdr = ('arm', 'frames', 'scored', 'PSNR all', 'PSNR mov', 'PSNR sta',
           'PSNR occ', 'PSNR p10', 'held%', 'gen/s', 'judder ms', 'jud>4%')
    rows = []
    for name, s in results:
        rows.append((
            name,
            str(s['frames_total']),
            str(s['frames_scored']),
            f"{s['psnr_all']:.2f}",
            f"{s['psnr_moving']:.2f}",
            f"{s['psnr_static']:.2f}",
            f"{s['psnr_occluded']:.2f}",
            f"{s['psnr_all_p10']:.2f}",
            f"{s['held_pct']:.1f}",
            f"{s['generated_per_s']:.2f}",
            f"{s['judder_mean_ms']:.2f}",
            f"{s['judder_gt4_pct']:.1f}",
        ))
    widths = [max(len(h), max(len(r[i]) for r in rows)) for i, h in enumerate(hdr)]
    fmt = '  '.join(f'{{:<{w}}}' for w in widths)
    print(fmt.format(*hdr))
    print('  '.join('-' * w for w in widths))
    for r in rows:
        print(fmt.format(*r))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene-dir', required=True)
    ap.add_argument('--arm', action='append', default=[], metavar='NAME=PREFIX')
    ap.add_argument('--max-frames', type=int, default=90)
    ap.add_argument('--cache', default='./cache')
    args = ap.parse_args()

    clip_path = os.path.join(args.scene_dir, 'clip.json')
    with open(clip_path) as f:
        clip = json.load(f)

    w, h = clip['size']
    STEP[0] = float(clip.get('step', 1.0))
    scene = analytic.build_scene(clip['scene'], size=(w, h), seed=clip['seed'])

    results = []
    for arm in args.arm:
        name, prefix = arm.split('=', 1)
        print(f"\n=== ARM: {name} ({prefix}) ===", flush=True)
        summary = score_arm(name, prefix, scene, clip, args.max_frames, args.cache)
        results.append((name, summary))

    print()
    print_table(results)


if __name__ == '__main__':
    main()
