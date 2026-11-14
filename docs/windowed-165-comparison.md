# Windowed checks and primary 165 Hz comparison

**Acceptance: FAIL on the current production primary path (G103).** New checks:
[Primary acceptance](primary-acceptance-checks.md). Primary windowed transitions
now PASS, but both film cuts still corrupt. The evidence below predates that recheck.

Windowed startup failures were test-window occlusion (G100).
Native cut detection now handles the two reproduced cuts in secondary dumps (G101).
Primary 165 Hz motion and post-fix visual acceptance remain open.

## Setup and scope

RTX 5060 Ti, driver 621.01 (queried with `nvidia-smi`), primary DISPLAY1
2560x1440 @ 164.999 Hz; secondary DISPLAY2 1920x1080 @ 60 Hz. Desktop scale
110%. Original executable SHA256:
`8cc42eec3414f737964abf0fea971f7546c0e1d1476fae70cbf92b209bf28d42`.
The temporary deferred-motion build was rejected and the original executable restored.
The deferred-motion change remains rejected. The subsequent cut fix is described below.
Its checks use the secondary monitor or hidden capture only; the owner reserved the
primary for work. No post-fix 165 Hz result is claimed.
Final built executable SHA256:
`217f90071fbf9a8271a12f5f01db71f59c211001c6413aca5f696e9893191857`.

Bounded primary checks use D38. Disposable Chrome profiles, unprotected local
media, no DRM test. Synthetic muted sources explicitly use `--scout-always`;
this is a lab override, not evidence that the production media gate accepts them.
The native primary rigs exempt only `codex-computer-use.exe` from occlusion,
as in the earlier primary rig. An initial film arm without that exemption was
OBSCURED, never showed, and is excluded.

The Startup shortcut supplied by the owner resolves to
`E:\Downloads\NovaGram\0e697781f91418e5\Lossless Scaling\LosslessScaling.exe`,
version 3.2.2. Observed profile: LSFG 3.1, Adaptive, target 165, flow scale 100%,
Performance off, spatial scaling off, WGC, queue target 1, default sync, maximum
frame latency 3. Cursor options off. Profile options were not changed. LS was
started for the check, scaling was stopped, and the owned instance was closed.

## P28: windowed mechanics

`windowed_check.py --local --monitor 2 --fps 24` and 30/60 all exit 0.
Scroll, theater, pause/play, fullscreen and return all pass the rig's geometry
and transition assertions; no engine error and clean engine exit in all three.

| CFR source | First show | Scroll hide / return | Pause hide | Maximum edge error |
|---|---:|---:|---:|---:|
| 24 | 3.64 s | 0.10 / 1.81 s | 0.40 s | 1.41 px |
| 30 | 3.70 s | 0.10 / 1.71 s | 0.40 s | 1.41 px |
| 60 | 3.66 s | 0.10 / 1.71 s | 0.50 s | 1.41 px |

This passes the existing <=2 px functional tolerance, not the final I13 visual
gate. Other sites, animated sidebars, primary-windowed motion and long soaks remain open.
Evidence: `harness/runs/windowed-resume/cfr{24,30,60}/windowed_check.json`.

Fresh exact-PID CFR24 checks (`windowed-pid-check/`, `windowed-pid-repeat/`)
failed while the test Chrome was covered. Chromium's `HideImpl` reparents its
dummy `Chrome_RenderWidgetHostHWND` to a hidden window; this was an occluded
test window, not evidence of a bad video-rect fallback. See the
[Chromium implementation](https://raw.githubusercontent.com/chromium/chromium/main/content/browser/renderer_host/render_widget_host_view_aura.cc).
The rig now raises only its owned secondary window with `SWP_NOACTIVATE`, saves
DOM/widget geometry and checks the foreground HWND. CFR24/30/60 transitions
passed again, all three `foreground_unchanged=true`, clean exits. Evidence:
`harness/runs/secondary-cause/windowed-clear{,-30,-60}/`. No UIA/MSAA viewport
fallback was added to the engine. Owner topmost windows still take precedence;
subsequent OBSCURED film arms are excluded rather than moved out of the way.

## G97: owned browser binding

A repeated primary probe selected a leftover fullscreen Chrome with the same
`NSP windowed page` title rather than its own process; the engine was OBSCURED
and never showed. This attempt is not a motion result. Bind engine startup to
the browser PID returned by CDP (or the verified owned HWND), not just a title.
Applied to `windowed_check.py`, `film_arms.py`, and `yt_arms.py`.

`film_arms.py` now enumerates the selected monitor (`--monitor`, default 2),
rechecks its presence before each browser launch, resolves output paths to
absolute paths, rejects invalid arms/crops and fails if the overlay never shows
or the recorder fails. Errors/invalid playback yield nonzero exit. Its new
`cadence_valid` field separately excludes coalesced/incomplete recordings.
`yt_arms.py --ls-exe` selects the owner's actual LS copy; paths are made absolute.
`refresh_probe.py --engine-args=--scout-always` explicitly enables synthetic lab playback.

## G98: output count versus changing pictures

Desktop Duplication crops measure recorded image changes, **not physical scan-out
FPS or a subjective smoothness PASS**. The recorder itself may perturb composition.
Coalesced recordings are censored. Source counters showed zero additional Chrome
drops during every repeated capture below; startup/seek drops are excluded.

| Probe | Recorded distinct pictures/s | Recorder-coalesced updates |
|---|---|---|
| CFR24 page control | 23.98 | 0 |
| CFR24 native MC, 30 s run / 3 s crop | 140.78; engine submits 164.55/s | 0 |
| CFR24 native blend control | 164.75; engine submits 164.76/s | 0 |
| CFR24 MC, same page as LS, three repeats | 140.72 / 140.44 / **141.11** | 1 / 1 / **0** |
| CFR30 MC, three repeats | **134.74** / 134.58 / 134.62 | **0** / 3 / 1 |
| CFR60 MC, three repeats | 106.70 / 108.13 / 107.38 | 8 / 6 / 11; all censored |
| CFR24 LSFG, three repeats | 148.61 / 162.73 / 161.73 | 0 / 0 / 0 |

Baseline/control crops: 2560x16 at monitor-local (0,660), moving part of the
960x400 CFR fixture around media 5-8 s. Repeated MC/LS recordings use the same
local page and seek interval around 4.8-8.1 s. Refresh-server and repeated-page
controls are separate runs. MC gaps >9.1 ms recur with median 42.4 ms in the
clean 24p baseline, close to the source period; this does not prove causality.

These small synthetic tests show an unresolved MC-path deficit and higher LS
change counts on this fixture. They establish neither overall LS quality parity
nor latency, A/V sync, native-1440p source quality, or owner blind acceptance.
Do not turn the censored 60p range into an accepted performance number.

Evidence under `harness/runs/`: `refresh165-resume/`,
`refresh165-blend-control/`, `ours165-repeated/`, `ls165-resume/`.
Ignored run directories retain drivers, logs, setup, source counters and raw crops.

## N40 / N41: rejected pacing candidates

Moving `PrepareMotion` after Present when output uses an already prepared older
pair increased capture updates to 164.06/s but distinct pictures remained
140.05/s, with 6 coalesced updates. No acceptance evidence; source change rolled
back. Do not equate more desktop updates with repaired motion (N40).

The process GPU scheduling HIGH probe succeeded (`NTSTATUS 0`) after the engine
had shown its overlay. One clean 2560x4 crop still gives 140.74 distinct
pictures/s; other repeats give 140.30 / 140.44 with 5 / 6 coalesced updates.
The thinner crop prevents an effect-size comparison with the 16-row baseline,
but this setting alone did not reach 165 changes/s in the clean sample (N41).
Calling the API before GPU initialization had returned `0xc000000d`; that failed
attempt is not a priority-performance result. No priority/timer default was adopted.

## G99: real-film cut artifacts at 165 Hz

Local Tears of Steel, 862x360 source upscaled by Chrome to the primary,
24p, moments selected at 36 and 303 s, recordings around 40-42 and 307-309 s.
Native and LS use the same film, monitor and center crop (640x360, 2 seconds).
Source playback advances and there are zero additional Chrome drops.

Recordings are unsuitable for a numerical cadence ranking: native has 25 / 14
coalesced updates and LS has 1 / 2. Captured pictures still provide direct
artifact evidence against the page control:

- Title cut: page switches to clean `40 YEARS LATER`; Nova frames 257/258 have
  broken letters, recovering by frame 263. Page frames 272/273 are already clean.
- Vent cut: page frame 50 is clean; Nova frames 51/52 contain dark warped patches
  on the vent/wall. LS blends scenes before its corresponding transition, then
  has cleaner vent frames. This is two cuts, not a whole-film verdict.

This reopens P23's primary live gate; the earlier offline 127/128 result remains
historical evidence. The cause and bounded fix are isolated below; primary
post-fix visual acceptance is still owed.

Evidence: `film165-resume/` page control (its initial hidden native arm is invalid),
`film165-valid/` shown native, `lsfilm165-resume/` LS, `film165-sheets/`.
`cut-control-m0.png` and `cut-control-m1.png` show original-source and generated
frames across corresponding cuts. Indices are selected separately per arm;
they are not a latency measurement. `dda_compare.py` nearest-frame sheets are
qualitative and do not imply exact phase alignment.

## G101: midpoint residual falsely explains cuts

Secondary native WGC capture, hidden with `--manual`, reproduces the title and
vent failures without using the primary. The diagnostic stages copies on the
GPU and maps them only at shutdown. Blocking readbacks change the failure;
they are not an acceptance control. Actual blocking `SetSyncAfterFlow(true)`
also fails to fix it. D3D debug layer is absent (`0x887a002d`).

Prepared/rendered A/B match byte-for-byte and source roles match. The GPU's
decision matches its measured inputs: this is not a wrong pair or stale cut
flag. On the same title cut, still=0.069662 and hist=0.374415, yet warped/still
is either 0.2492 (miss) or about 0.94 (hold). Arbitrary cross-cut flow sends
both midpoint samples into dark regions. Forward/backward fields disagree by
tens or hundreds of pixels; 98-99% of changed luma lacks a consistent round trip.

`CSCutResidual` now accumulates that unmatched change separately. The old
hold/fade routes stay in place. The additional hard-cut route requires all of:
unmatched/still >=0.85, still >=0.010, a >=2.5x jump over the preceding pair,
spatial tone distance >=0.30 and global tone distance >=0.15. Source-to-source
round trips allow one full-resolution pixel plus 5% of displacement. This adds
no CPU readback/wait. Missing side fields disable the additional route;
hardware-to-block fallback clears side indices before cut detection.

Rejected controls (N42/N43): source-anchored residual alone still misses the
title (share 0.614); replacing the old metric with a consistency penalty causes
BBB fades (25.28 ->24.34 dB; large errors 3.03 ->4.84%). Max-of-directions also
adds fades. Spatial-only gating falsely holds the continuous animated hand at
TOS pair14264; the global-tone guard excludes it.

Verification on RTX 5060 Ti:

- Title and vent: three native traces each, every cross-cut output holds A
  exactly. This includes title share=0.2492 and vent share=0.1945 misses under
  the old rule. Hidden render verification is not physical 165 Hz scan-out.
- Context replay of all 128 labelled TOS cuts: 127/128 held; four selected
  abrupt-motion controls keep the old decisions. Source pair14264 is not held.
- Default-hint A1/A3/A4/A5 and R2/R3/R4 matrix: R2/R3 identical metrics;
  A1 mask deltas -0.01..-0.04 dB, A4 +0.02..+0.27 dB, Spring +0.0024 dB mean.
  No claim of cross-build bit identity for the hint arm (G53/N25).
- Shipping build `/W4`: no warnings. G0/G1, three repeats, t=0/0.1/0.5/0.9/1
  pass with NVOFA and blocks. Spring NVOFA: four exact cut holds, zero false
  holds in 120 pairs. Nine Python tests pass, including rejection of a focus change.
- Block fallback misses all four Spring cut holds on both the original and
  new executable (G102); the NVOFA fix does not close that existing defect.

Evidence root: `harness/runs/secondary-cause/`. `async-title/` and
`fieldtrace-native-{1,2,3}/` isolate the cause; `guardcut-final-native-{1,2,3}/` and
`guardcut-final-vent-{1,2,3}/` are final native fix controls (16 exact A outputs).
`native-final-verdict.json`, `film-candidates-summary.json`,
`gate/cut-{baseline,guard}-default/`, `production-*-{g01,p23}.log` retain gates.
Diagnostic trace and blocking synchronization code are not in production.

## N44: cohere deduplication is slower

Skipping repeated candidate vectors preserves the candidate set, but the
per-cell comparison/divergence cost loses here. Six alternating offline GPU
timings on 2560x1440 BBB, 15 pairs/run: current PrepareMotion 5.616/5.302/5.436 ms;
dedup 5.764/5.637/5.976 ms. Median 5.436 ->5.764 ms (+6.0%). Warp 0.797-0.855 ms.
D3D11 queue timing excludes NVOFA's own engine; it is not live frame latency.
Candidate rejected, production cohere unchanged. `cohere-summary.log`.

## Magpie and remaining work

The official [Magpie FAQ](https://github.com/Blinue/Magpie/wiki/FAQ-%28EN%29)
still says frame generation is not planned. Magpie was not run here; its spatial
scaling/FPS counter is not a temporal-generation control. The pinned source
comparison remains in `magpie-comparison.md`. Timer/priority/cache ideas remain
separate experiments; the GPU HIGH result above is not a rejection of every
Magpie scheduling technique.

Next: trace picture identity and GPU/display completion around P29 source-period
repeats. Repeat post-fix primary cuts, uncensored 24/30/60 and alternating LS
arms when the primary is available; then primary windowed video, soaks and owner
blind A/B. Investigate G102 block cut detection separately. No signing,
installer rebuild, commit or publication.
