# Primary 165 Hz: native pacing and visible motion

**Visible smoothness is not accepted.** The owner rejected the isolated 24 fps
run as still jerky. The presentation bottleneck below is reduced, but increased
Present counts do not establish that the displayed motion is fixed.

## Measurement method

- RTX 5060 Ti 8 GB, driver 621.01; primary `DISPLAY6`, physical 2560x1440 at
  164.999 Hz. Secondary `DISPLAY7`, 1920x1080 at 60 Hz.
- A disposable Chrome 156.0.8078.73 profile plays muted local unprotected WebM.
  Native fullscreen starts with Chrome's CLI flag. CDP verifies the exact
  loopback URL and target ID before each run, native fullscreen bounds/DPI,
  page fullscreen, visibility and playback. No Windows UI automation is used.
- Engine logs verify a 2560x1440 WGC ring, active overlay and target refresh.
  Game/DRM gates remain enabled. Only the Computer Use helper is exempted from
  occlusion in the benchmark command, as in the earlier measurement rig.
- Each arm is 30 seconds. Successful Present/s uses ten SHOWN intervals ending
  between engine times 8 and 28 seconds, weighted by actual interval durations.
  Chrome frame counters and DXGI media statistics are recorded separately.
- CFR fixtures are 960x400, rendered at the display size; Meridian is an
  upscaled 1920x1080, 29.97 fps excerpt looping every 1.668 s. This is not a
  native 1440p source-quality gate or a continuous-movie test.
- Separate Desktop Duplication recordings capture a 2560x16 moving strip.
  They carry QPC timestamps, hashes and accumulated-update counts. Captures
  with accumulated/coalesced updates are marked censored. The recorder can
  perturb composition; these results are not called physical scan-out FPS.

## Initial native baseline

Executable SHA256: `5ccf98d1d01e40b211115b9375a7cef06533ea9b370825c7ff0f41e6dd145342`.

| Source | Successful Present/s | DXGI reported images/s | Chrome drops |
|---|---:|---:|---:|
| CFR24 | 140.80 | 140.79 | 0 |
| CFR30 | 137.28 | 137.28 | 0 |
| CFR60 | 161.15 | 161.09 | 0 |
| Meridian 29.97 | 135.72 | 135.71 | 0 |

The page-only controls sustain about 165 RAF callbacks/s and have zero Chrome
video drops. RAF is a scheduling control, not output or scan-out FPS. All native
arms were visible; no occlusion pause or engine error invalidated the runs.

## G88: buffer wait after compositor wait

CPU call timers isolate capture polling, motion submission, back-buffer acquire,
draw submission and Present. They measure CPU elapsed time, not GPU execution.
In the profiled CFR24 baseline, motion submission is about 0.4 ms/pair; acquiring
the back buffer averages about 1 ms and sometimes waits 6-8 ms after the main loop
has already waited for the compositor clock.

The controlled candidate changes the swapchain's maximum queued frames from one
to two. Acquire falls to roughly 0.04 ms in clean intervals. The final code uses
two for the compositor-paced output and retains one for a slower rate-gated
output. It also updates this setting when the monitor/topology rate changes;
failed updates log an error and disengage. The API is documented by Microsoft:
[SetMaximumFrameLatency](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency).

Final executable SHA256: `c74e2b91c1228e49061e2bc02efe4277e6ba43d3386550cd6dae39a816030525`.

| Source | Final successful Present/s | Chrome drops |
|---|---:|---:|
| CFR24 | 164.76 | 0 |
| CFR30 | 164.80 | 0 |
| CFR60 | 162.84 | 0 |

These final runs also contain a short recording probe, unlike the initial
throughput baseline. The separate no-recording candidate gives 164.80, 164.60
and 162.90 Present/s respectively. End-to-end latency and A/V sync were not
measured; two queued frames are a buffering change, not a zero-latency claim.

## N37: submissions are not a smoothness verdict

An isolated three-second moving CFR24 recording before the queue change has
139.51 distinct strip changes/s, zero recorder-coalesced updates, and a moving
edge change rate of 138.83/s. After the change it has 140.92 distinct strip
changes/s and an edge change rate of 138.92/s, also with zero coalesced updates.
The edge's median/p95 spatial step remains 4/8 pixels in both recordings.
Thus the experiment improves submission throughput but does not demonstrate
an improvement in this recorded motion measure.

Intermediate motion exists: the recorded leading edge takes positions such as
381, 384, 388 and 395 pixels, instead of only the source's approximately
26.67-pixel steps. This does not override the owner's visible-judder report.
The remaining task is to identify repeated display/content phases and verify
actual visible improvement, rather than promote the higher counter as a fix.

The fixture's moving rectangles leave the picture periodically; unchanged
source captures during those blank intervals are not automatically lost frames.
The three-second edge check uses the moving part around media times 5-8 s.
Raw per-poll DXGI same/skipped-refresh counters are sampling diagnostics and
must not be summed as a count of unique display frames.

## Video-overlay diagnostic

A separate test adds `--disable-direct-composition-video-overlays` only to the
disposable Chrome. Strip changes remain about 141/s in both default and flagged
runs. Chrome 156 returns no `GPU.DirectComposition` histograms here; the Chrome
152 histogram decoder cannot confirm the physical plane used. The MPO hypothesis
remains unverified, not a proven cause. No system or owner-browser setting changed.
The switch is described in Chromium's
[DirectComposition support interface](https://chromium.googlesource.com/chromium/src/+/dc6e8140c6e1f0/ui/gl/direct_composition_support.h).

## Evidence

- `harness/runs/refresh165-current/`: eight control/native runs, setup, source
  counters, continuous DXGI spans, `native-analysis.json`.
- `refresh165-24-only/`: the owner-rejected isolated baseline and edge trace.
- `refresh165-profile/`: CPU-stage baseline, full final build and native fault tests.
- `refresh165-latency2/`: controlled candidate without the recording probe.
- `refresh165-final/`: final native runs, strip recordings and `edge-check.json`.
- `refresh165-mpo-default/`, `refresh165-mpo-disabled/`: video-overlay probe.

Raw directories above are under `harness/runs/` and ignored by git. The native
fault test passes 24/24, including changing the queue 1 -> 2 -> 1. Actual TDR,
sleep/monitor recovery, long soaks, LS comparison and visible acceptance remain
open. DRM playback was not tested.

The full /W4 build, short G0 endpoint/G1 repeatability check and eight Python
playback/windowed tests pass. The unsigned installer was rebuilt for the final
executable; it was not installed or published.

The attempted post-change secondary windowed regression did not engage: the log
reports no `Chrome_RenderWidgetHostHWND` page area, before any swapchain/queue
setup. A later CDP wheel acknowledgement timed out. This is not a functional
PASS and does not establish a queue regression; windowed prerequisites need
investigation. The rig now aborts immediately when its initial overlay never
appears, rather than continuing into transition commands.
