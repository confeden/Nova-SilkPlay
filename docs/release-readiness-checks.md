# Secondary-monitor release checks

These checks precede G88. See the [primary-monitor follow-up](refresh-165-cdp-check.md)
for the newer executable and the remaining visible-smoothness failure.

The native application and unsigned installer build successfully. Short functional
checks pass; release acceptance remains open. DRM playback was excluded.

## Native presentation fix (G87)

A failed `Present1` previously returned the same `true` as a successful present
until eight consecutive failures accumulated. The app consequently increased its
FPS counters and could reveal the initial overlay without a successful present.

`Overlay::PresentResult` separates `Presented`, `Skipped` and `DeviceLost`.
A skipped submission neither increases FPS nor permits the first overlay show;
its judder baseline is reset. Eight consecutive failures still trigger recovery,
and device removal/reset remain immediate recovery cases. An isolated failure
retains the existing visible frame until the next tick.

The native fault test compiles an output copy of the real overlay implementation,
injecting errors only at the `Present1` call. It uses hidden windows on the
secondary monitor: **21/21 checks pass**, including a real successful submission
between failure streaks. This simulates HRESULTs; it does not induce an actual TDR.
The production binary contains no fault-injection hook.

## Live functional checks

Hardware: RTX 5060 Ti 8 GB. Selected monitor: secondary, **1920x1080 at 60 Hz**,
screen origin `(2560, 0)`. Muted local CFR WebM in an owned Chrome profile;
no foreground input or cursor movement. The rig now selects its window by browser
PID as well as title and verifies physical bounds on the selected monitor.

| Source | First show | Scroll hide / return | Pause hide / play return | Result |
|---|---:|---:|---:|---|
| 24 fps | 3.74 s | 0.10 / 2.11 s | 0.40 / 0.10 s | PASS |
| 30 fps | 3.44 s | 0.10 / 2.11 s | 0.50 / 0.10 s | PASS |
| 60 fps | 3.52 s | 0.10 / 1.93 s | 0.50 / 0.10 s | PASS |

Each run also passes theater, fullscreen and return-to-windowed transitions,
the existing geometry gate (all edges within 2 physical pixels of page truth),
log error checks and graceful exit. These are short transition checks, not a
30-minute soak, visual imperceptibility acceptance or proof of smooth scan-out.
The 60 fps page callbacks remain uneven (about 12/18 ms), and native judder
diagnostics remain nonzero. P25 is still open; functional PASS does not close it.

The Settings snapshot at monitor 2 was inspected: text fits, the Chrome and
general settings are readable, and the inactive PotPlayer row remains distinct.
The Python windowed/playback and soak test suites pass (8 + 6 tests).
Native G0 endpoint checks and G1 repeatability pass on a four-frame Meridian
sample (three synthesized midpoints, two passes).

## P20 color reconstruction experiment (N36)

`harness/probe_cubic_color.py` builds an isolated executable with exact separable
Catmull-Rom reconstruction (nine bilinear taps in linear light) replacing only
PSWarp's final A/B color fetches. Motion, candidate costs, cuts, endpoint fetches
and shipping source/binary stay unchanged.

Both executables were scored serially using the complete shipping arm
`mc:ofa+hints:bidir+cand`, A1/A3/A4/A5 and 31 midpoint pairs per real-content clip.
Per-scene results are mixed:

| Metric | Change from bilinear |
|---|---:|
| A1 visible in both frames, PSNR | -0.13 dB |
| A3 occluded region, PSNR | -0.07 dB |
| A4 visible in both frames, PSNR | +1.14 dB |
| Meridian PSNR | +0.22 dB |
| BBB large-error pixels | 2.9773% -> 2.9790% |
| Spring PSNR | +0.01 dB |

The candidate is **not promoted to the default**: it trades improvements for
regressions and does not establish better general quality. Its live pacing and
GPU cost were not measured. No new LS comparison or blind A/B was performed.

## Artifacts and remaining gates

- Executable used for these secondary checks (before G88), SHA256
  `5ccf98d1d01e40b211115b9375a7cef06533ea9b370825c7ff0f41e6dd145342`.
- Installer: `installer/out/Nova SilkPlay-0.9.0-dev-setup.exe`, unsigned;
  compiled but not installed during these checks.
- Raw native/fault/build logs: `harness/runs/release-secondary-fixes/`.
- Live JSON/logs: `harness/runs/release-secondary-fixed-cfr{24,30,60}/`.
- Quality matrices: `harness/runs/gate/release-secondary-{base,cubic}/gate.json`.
- Outstanding: sustained 165 Hz, 60 Hz visual cadence, motion-boundary quality,
  broader LS/blind A/B, CFR30/60 resource soaks and real system recovery.

The app remains `0.9.0-dev`; no signing or publication was performed.
