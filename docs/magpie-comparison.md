# Magpie comparison

Reference: [Blinue/Magpie](https://github.com/Blinue/Magpie/tree/241e0a447d1c45fc95781f9cd4b5a9198e738a3b),
commit `241e0a447d1c45fc95781f9cd4b5a9198e738a3b`, GPLv3. This is a source comparison,
not a measured performance comparison.

| Area | Magpie | Nova SilkPlay | Disposition |
|---|---|---|---|
| Purpose | Spatial window upscaling and filters | Temporal video frame generation with continuous phase | No optical-flow/interpolation engine to import from Magpie |
| Startup | Scheduled task, with a user Startup shortcut fallback (`AutoStartHelper.cpp`) | Previously HKCU Run | Adopt known-folder + ShellLink design independently; use Startup directly, no elevation/task scheduler |
| Global shortcuts | Hook and RegisterHotKey together, 100 ms action debounce (`ShortcutService.cpp`) | Dedicated hook thread, key-up pairing, autorepeat suppression, AltGr exclusion, registration fallback | Retain dedicated thread and AltGr safeguards; reported failure was windowed geometry, not a missing key event |
| Capture | WGC; drain queue to newest frame (`GraphicsCaptureFrameSource.cpp`) | WGC; source-pair ring, frame-content test and timeline | Keep source-frame history: dropping intermediate video frames blindly harms interpolation/cadence |
| Duplicate detection | GPU comparison with dynamic/always/never policies (`FrameSourceBase.cpp`) | Dirty-region filter plus GPU content comparison of ambiguous captures | Already present; retain video-specific cadence rules |
| Borders and cursor | Scaling window tracking, cursor management and coordinate scaling | One persistent click-through DComp HWND; source pixels 1:1; OS cursor untouched | Keep Nova's I11-I13; cursor capture/rescaling is outside product requirements |
| Spatial effects | Anime4K, FSR, CRT and other spatial shaders | No spatial enhancement stage | These do not repair wrong motion or disocclusion; do not add sharpening to interpolation artifacts |
| Shader compilation | Effect cache and effect pipeline | Embedded HLSL compiled at engage | Potential later engage-time improvement; does not improve interpolated image quality |

## Root cause behind the reported windowed failure

The owner's log contains accepted Ctrl+Alt+Q events followed by failed geometry confirmation.
One video repaint is partitioned into `[326,195 1909x1001]`, `[326,1196 1909x15]`,
`[326,1211 1909x58]`, alongside an unrelated `[12,1196 145x15]` animation. Other
captures split the first region at height 989. The finder counted individual strips,
selected an incomplete video rectangle, and the guard could never confirm it.

`DamageSig::Normalized` joins only strips whose union is an exact rectangle, with
no gap, added pixel, corner fill or L-shaped bounding-box approximation. Both finder
and guard use that representation. Regression tests replay changing split heights,
unrelated animation and scroll rejection. Explicit ON also restarts scouting for
20 seconds without requiring a system media session or waiting for an old cooldown.

This does not solve arbitrary merged ambient-glow/sidebar damage (N31). When no
separate rectangular video evidence exists, the app still fails closed. Solving
that case requires content-based localization and independent geometry validation.

## Image-quality change in Nova

The midpoint motion field already received coherence regularization, while the
A- and B-anchored visibility fields stopped after the median pass. The new path
also regularizes those fields at their own anchors, once per source pair, and
accepts a replacement only with an 8% photometric improvement. This is independent
Nova code, not a spatial filter transferred from Magpie.

Measured on RTX 5060 Ti, 31 withheld midpoint frames per clip, shipping defaults:

| Clip | PSNR before / after | Large-error pixels before / after |
|---|---|---|
| Meridian | 44.68 / 44.68 dB | 0.0005 / 0.0006 % |
| BBB | 25.25 / 25.92 dB | 4.1034 / 2.9773 % |
| Spring | 32.35 / 32.43 dB | 0.9634 / 0.9566 % |

BBB's large-error share falls by 27.4%; this does not mean all visual artifacts
fall by that amount. Analytic A3 loses 0.21 dB overall and A1's only_B stratum
loses 0.37 dB. Live visual acceptance remains unverified. Evidence is retained in
`harness/runs/gate/startup-shipping-before/gate.json` and
`harness/runs/gate/startup-shipping-guarded/gate.json` (local, regenerated data).

Measured GPU cost on RTX 5060 Ti, 2560x1440 fixtures resized from BBB for timing only,
11 timed pairs (first pair skipped), D3D11 timestamps excluding NVOFA's separate engine:
PrepareMotion 2.790 → 5.207 ms per source pair; Warp 0.811 → 0.809 ms per output frame.
This is not a live 165 Hz throughput or judder claim. G0 endpoints passed, and G1
reproduced 9 synthesized frames over three repeats bit-exactly at 1440p. P23 held
4/4 hard cuts exactly with no false holds among 120 pairs. Timing/identity evidence:
`harness/runs/startup-final-checks/`; cut probe log retained locally.
