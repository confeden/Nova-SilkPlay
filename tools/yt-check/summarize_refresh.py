"""Summarize the local refresh rig without equating RAF or Present with scan-out."""
import argparse
import json
from pathlib import Path
import re
import statistics


def intervals(values):
    return [b - a for a, b in zip(values, values[1:]) if b > a]


def percentile(values, pct):
    values = sorted(values)
    if not values:
        return None
    at = (len(values) - 1) * pct / 100
    lo = int(at)
    return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (at - lo)


def summarize(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    raf = intervals(data['raf'])
    callbacks = intervals([f['expectedDisplayTime'] for f in data['rvfc']])
    row = {'run': data['run'], 'source': data['source'], 'arm': data['arm'], 'screen': data['screen'],
           'duration_s': data['elapsedMs'] / 1000,
           'raf_hz': len(data['raf']) / (data['elapsedMs'] / 1000),
           'raf_interval_median_ms': statistics.median(raf) if raf else None,
           'raf_interval_p95_ms': percentile(raf, 95),
           'video_callbacks_hz': len(data['rvfc']) / (data['elapsedMs'] / 1000),
           'video_callback_interval_p95_ms': percentile(callbacks, 95),
           'browser_video_total_delta': data['qualityAfter']['totalVideoFrames'] - data['qualityBefore']['total'],
           'browser_video_dropped_delta': data['qualityAfter']['droppedVideoFrames'] - data['qualityBefore']['dropped'],
           'page_errors': data['errors']}
    engine = path.with_name(data['run'] + '-engine.log')
    if engine.exists():
        text = engine.read_text(encoding='utf-8')
        records, scan, previous = [], None, 0
        for line in text.splitlines():
            sm = re.search(r'scan-out: DXGI reports (\d+) shown images across (\d+) samples .*same v-blank (\d+), skipped v-blanks (\d+), discontinuities (\d+)', line)
            if sm:
                scan = dict(zip(('shown', 'samples', 'same', 'skipped', 'discontinuities'), map(int, sm.groups())))
            m = re.search(r'^\[\s*([\d.]+)\] in ([\d.]+) fps \(est. source ([\d.]+) fps,.*out ([\d.]+) fps.*\| (SHOWN|hidden)$', line)
            if m:
                end, source, estimate, out = map(float, m.groups()[:4])
                records.append({'end_ms': end, 'duration_s': (end - previous) / 1000,
                                'source_capture_fps': source, 'source_estimate_fps': estimate,
                                'present_fps': out, 'state': m[5], 'scanout': scan})
                previous, scan = end, None
        stable = [r for r in records if 8000 <= r['end_ms'] <= 28000 and r['state'] == 'SHOWN']
        measured = sum(r['duration_s'] for r in stable)
        row['engine'] = {'shown_intervals': len(stable), 'intervals': stable,
            'present_fps_weighted_mean': sum(r['present_fps'] * r['duration_s'] for r in stable) / measured if measured else None,
            'present_fps_min': min((r['present_fps'] for r in stable), default=None),
            'present_fps_max': max((r['present_fps'] for r in stable), default=None),
            'dxgi_discontinuities': sum(r['scanout']['discontinuities'] for r in stable if r['scanout']),
            'paused_occlusion': 'generation PAUSED' in text,
            'engaged': 'overlay shown' in text,
            'errors': [line for line in text.splitlines() if 'ERROR' in line or 'failed' in line.lower()]}
        row['valid_for_output_measurement'] = len(stable) >= 3 and not row['engine']['paused_occlusion'] and row['engine']['engaged']
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', type=Path)
    args = ap.parse_args()
    rows = [summarize(path) for path in sorted(args.directory.glob('*-page.json'))]
    (args.directory / 'summary.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    for row in rows:
        engine = row.get('engine', {})
        print(row['run'], 'RAF', round(row['raf_hz'], 2), 'video', round(row['video_callbacks_hz'], 2),
              'browser video dropped', row['browser_video_dropped_delta'], 'Present', engine.get('present_fps_weighted_mean'),
              'shown', engine.get('shown_intervals'), 'paused', engine.get('paused_occlusion'))


if __name__ == '__main__':
    main()
