# 165 Hz monitor check

Native follow-up: [CDP pacing and visible-motion checks](refresh-165-cdp-check.md).
The tables below retain the earlier native baseline and neural budget evidence;
the overall visible-smoothness gate remains open.

**Result: sustained 165 Hz playback is not accepted.** The current native engine
misses the output target in the measured desktop environment. Exact-phase RIFE
with the current reference renderer also exceeds the per-refresh budget.

## Display and scope

- RTX 5060 Ti 8 GB; actual active mode reported by `silkplay.exe --list-displays`:
  `\\.\DISPLAY6`, 2560x1440, **164.999 Hz**, primary. Secondary is 1920x1080 at 60 Hz.
- Owner explicitly requested this primary-monitor check, superseding the earlier
  secondary-only restriction for these bounded measurements (D38).
- Native executable SHA256: `306D5D8BF7A7E4BFFD3F9A9B8E85D4AFD2D701CCF11B7232BAFDFB6637CA03E0`.
- Chrome plays local muted WebM in a real F11 fullscreen window. Native logs
  confirm a **2560x1440** WGC ring, hardware NVOFA, and a visible DComp overlay.
  The browser debugging banner remains above the video. The engine retains its
  game and DRM gates; no protected content was tested.
- Shared desktop, existing browser extensions and other applications remain
  running. No display mode, user settings or unrelated process was changed.
- These are short cadence measurements, **not a visual quality gate**, a long
  movie soak, a Magpie/LS comparison, or a neural Chrome integration.

## Current native engine

Each run lasts 30 seconds. The table uses ten SHOWN statistics intervals ending
between engine time 8 and 28 seconds, weighted by their actual interval durations.
The figures are **successful Presents/s**, not a claim of unique scanned-out or
correctly interpolated frames. DXGI media statistics, holding/judder, source
cadence and page callbacks remain in the raw evidence.

| Source | Mean successful Presents/s | Chrome video drops in run |
|---|---:|---:|
| CFR 24 fps | 157.06 | 0 |
| CFR 30 fps | 151.09 | 0 |
| CFR 60 fps | 141.21 | 63 |
| Meridian 1920x1080, 29.97 fps | 152.07 | 0 |

Control without the engine: the same CFR60 source reports **22** Chrome video
drops, versus 63 with generation; Meridian reports zero in both arms. This is
evidence of an additional load problem in this run, not proof that all drops are
caused by one specific engine stage. RAF is only a page scheduling diagnostic:
it averaged 136.56/s on CFR60 without the engine, 114.42/s with it. It is not the
monitor frequency and is never substituted for scan-out.

Sources: CFR fixtures are deterministic 12-second clips. Meridian is a **1.668 s
real live-action excerpt**, encoded from every second SDR corpus frame; it loops
during the 30-second run. Loop boundaries can disturb the timeline, so this does
not establish cadence on a continuous movie. Chrome `droppedVideoFrames` is a
browser playback counter, not a complete decoder or display-drop diagnosis.

Valid native run identifiers:

- `20261030-215503-24-ours`
- `20261030-215543-30-ours`
- `20261030-215643-60-ours`
- `20261030-215738-meridian30-ours`

Controls: `20261030-215852-meridian30-page`, `20261030-215938-60-page`.

Three preliminary runs are explicitly excluded: `215206` was before native F11;
`215303` never engaged because page fullscreen alone did not make the native
window fullscreen; `215350` was paused by the transparent Computer Use cursor
overlay. Subsequent native runs ignore **only `codex-computer-use.exe` in the
measurement command**, without changing shipping occlusion behavior. Checking
the overlay's SHOWN state prevents accepting a hidden 165-Present/s loop.

## Neural synthesis budget

Budget at the observed refresh is **6.06064 ms per output**. Serial reference
measurements use RIFE 4.26 via TensorRT-RTX 1.7, BF16 with FP32 deconvolution
islands, exact phase-conditioned fields and original FP32 source RGB. The model
input is 768x448 in all rows. There are 15 warmups followed by 330 outputs with
varying phases corresponding to a 30 fps source and the display refresh.

| Source-color / render size | GPU median / p95, ms | Submission-to-completion median / p95, ms | Completion over 6.06 ms |
|---|---:|---:|---:|
| 1280x720 | 5.61 / 7.43 | 5.98 / 7.56 | 153 / 330 |
| 1920x1080 | 7.56 / 9.06 | 8.01 / 9.42 | 325 / 330 |
| 2560x1440 | 11.03 / 12.35 | 11.62 / 13.14 | 330 / 330 |

This is a necessary budget check, **not measured neural playback FPS**. The
source pair is resident on the GPU; larger RGB sizes are timing-only resizes of
the same BBB pair. Capture, upload, D3D/CUDA interop, presentation and scan-out
are excluded. Outputs are synchronized individually; inference and reference
rendering are included. GPU event time may include host submission gaps.
Every output is synthesized in this conservative workload; a future scheduler
may bypass exact source-frame phases, but that does not remove the measured
cost of each intermediate phase.

At 1440p, the source warp alone has a 5.70 ms median and 6.78 ms p95; optimizing
only network inference cannot provide the necessary margin. Earlier 4.30 ms
midpoint timings were a short 720p benchmark, not evidence of 1440p165 support.
No neural provider is wired into the native executable.

Required next work: profile native capture/synthesis/pacing under the real
refresh; reduce full-resolution neural rendering and allocation/submission
costs; then measure GPU interop and actual display cadence. Preserve source RGB,
P23 cut handling and the existing safety gates. Do not promote a midpoint-only
shortcut already disproved at t=0.75.

## Evidence and reproduction

Raw JSON, engine logs, active display modes and per-output neural timings live
in `harness/runs/refresh165-v1/` (ignored). Summaries are generated by
`tools/yt-check/summarize_refresh.py`; invalid native arms are marked explicitly.

```powershell
# Existing CFR fixtures: python tools/yt-check/make_cfr_fixtures.py
.\.venv-torch\Scripts\python.exe tools/yt-check/refresh_probe.py --out "D:\Documents\Coding\Nova SilkPlay\harness\runs\refresh165-v1" --prepare-media
# Open the printed local URL, enable native F11 on the target display, then use Run.
python tools/yt-check/summarize_refresh.py harness/runs/refresh165-v1
.\.venv-torch\Scripts\python.exe harness/learned_deadline.py --corpus harness/corpus/R3_bbb60_720crop --out harness/runs/refresh165-v1/rife-1440.json --size 2560x1440
```

No native source or binary was changed by this measurement. Test playback and
the owned engine/server were stopped, and Chrome was returned from F11.
