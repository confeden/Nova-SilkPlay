# Kinohub / Alloha geometry and FPS check

Target: `https://tv.kinohub.vip/movie/1209839`, owner's Chrome, primary
2560x1440 at 164.999 Hz, NVIDIA GeForce RTX 5060 Ti. Selected Alloha 1080p;
the decoded video reports 1920x960. No page channel was added to the engine.

## Observations

- Initial windowed attach: 0.9 s, damage rect `[816,778 992x528]`, readout
  `24 -> 165` visible. Source estimate 24.00 fps; submission rate near 165 fps.
- Browser automation could toggle Alloha's fullscreen control but
  `document.fullscreenElement` stayed null. Alloha displayed its unsupported
  fullscreen message. Native window input failed activation twice; manual
  fullscreen was requested. This is not a fullscreen PASS.
- During the failed transition the scout accepted `[1047,716 900x676]`, then
  `[816,716 1132x676]`. Both reach y=1392, the clipped bottom of the page area
  `[60,152 2501x1241]`. The oversized rect placed the FPS badge outside the player.
- After the player was fully in view, the original engine found
  `[183,355 990x469]` on the restored window and showed the overlay again.
- A four-second 800x450 DDA crop measured 163.41 distinct changes/s but missed
  two updates. The recording is censored for cadence acceptance; counters alone
  establish neither smoothness nor physical scan-out.

Raw evidence: `artifacts/kinohub-1080p/engine.log`, `windowed.csv`,
`windowed.raw`, `windowed-frame.png`.

## Correction / G105

`RectFinder::Plausible` now refuses damage touching either vertical viewport
edge, rather than requiring a perpendicular horizontal edge as well. A clipped
repaint cannot prove the complete video geometry. Partially scrolled video waits
until fully visible; full-width theater layouts with both vertical edges visible
remain eligible. Fullscreen uses the separate whole-client path.

Regression cases replay both measured bad rectangles and the initial good one,
check a full-width theater rect, and change the old half-scrolled-video case to
expect refusal. `rectfind_test.exe`: PASS, zero failed checks.

Native rebuild: PASS (`fixed-build.log`). Post-change windowed run found
`[183,355 990x469]`, confirmed geometry and showed the overlay; the DDA snapshot
`artifacts/kinohub-1080p/fixed-visible.png` shows `24 -> 165` inside the picture.
The dump run exited normally after 12 s (`fixed-engine.log`, `fixed_fps.ppm`).
The clipped-transition correction is verified by replay, not a new live transition.

The test engine is stopped and the primary monitor is free. The player's stale
fullscreen toggle was reset to normal. Alloha's own fullscreen UI remains
unverified; controlled native transitions and cadence are assessed below.

## Alternate source

Owner requested another source, suggesting Rutube. Selected landscape video:
`https://rutube.ru/video/792e8e08355909154883ddc73c018afb/`.
The ordinary Chrome profile remained on its poster/loading spinner, with no DOM
video element; console reported network and banner creation errors. Its UI also
reported an ad-blocker warning. The official embed URL from Share timed out.
No Rutube playback, 1080p rendition, detection or cadence acceptance was obtained.

Owner authorized the project's CDP rig in a separate Chrome without extensions
(D43). Clean-profile Rutube also produced no video; direct embed navigation timed
out. The official Blender YouTube page loaded but decoded zero frames in 45 s.
Neither service received playback or performance acceptance.

## Controlled 1080p browser modes

Source: `https://download.blender.org/durian/trailer/sintel_trailer-1080p.mp4`,
downloaded from Blender's public directory (14,621,544 bytes). The unmuted HTML
video reports 1920x1080, 24 fps. A Range-capable loopback page uses a 960x540 video
surrounded by ordinary page content; only the test rig uses CDP. The engine still
gets no page channel. Chrome's global audio mute avoids disturbing the owner
without setting the element's muted flag.

Primary 2560x1440/164.999 Hz, RTX 5060 Ti: two cycles of page -> real DOM/native
fullscreen -> page completed. Native fullscreen rect was `[0,0 2560x1440]`;
page damage rects were 1056x594 (1058x596 after one return, including the surrounding
pixel). At devicePixelRatio 1.1 the 960x540 element displays as 1056x594. Both return
events report normal native window state and the original page element geometry.
Actual DDA crops show `24 -> 165` inside the video in fullscreen and on the page.
The source decoded frames without drops in all five sampled stages. Pause hide:
1.009 s; play re-show: 0.101 s in the final cycle run (earlier hide 0.506 s).

This is functional acceptance, **not 165-picture/s fullscreen smoothness acceptance**.
The first clean fullscreen crop measured 129.33 distinct changes/s, with zero
recorder misses (`first-result.json`). Final-run fullscreen crops missed 5 and 3
updates and are censored. The final returned-page crop measured 164.93 changes/s
with zero misses over 3.99 s. Other crops had misses or a geometry-guard interruption.
The readout measures submission rate; P29 remains open.

Evidence: `artifacts/kinohub-1080p/sintel/result.json`, `first-result.json`,
`engine.log`, `first-engine.log`, `fullscreen1-badge.png`, `returned2-badge.png`.
Raw/csv prefixes in `result.json` describe the final run; earlier-run JSON/log
retains that run's metrics. The initial apparent reattach failures were a harness
predicate bug: a successful 0.00 s wait was falsy. Direct log-presence checks fixed
the observer; no fullscreen detector change was justified.

## G106 — Exterior repaint incorrectly hid the video

Measured damage `[1,0 1744x201] [1,201 44x594] [1101,201 644x594]
[1,795 1744x352]` surrounds video `[45,201 1056x594]` without repainting a video
pixel. `GeometryGuard::Add` expanded the intersection by the 2 px exact-match
tolerance, treated the ring as foreign, and hid/re-showed the overlay unnecessarily.
Intersection now uses strict half-open bounds; exact matching keeps its tolerance.
Regression replay failed before the change and passes afterward, including a
one-pixel intrusion and whole-page scroll that must still trip the guard.

Final native build and rectfind tests PASS. Post-fix live validation ran on the
60 Hz secondary, with foreground unchanged: source 1920x1080, 8 s without guard
interruptions; a vertically clipped video hid in 0.103 s, stayed refused for 3 s,
and reattached in 1.413 s after returning fully into view, at the identical rect
`[2652,221 1056x594]`. Evidence: `secondary-guard/result.json`, `engine.log`, and
`adjacent-before-test.log` / `adjacent-after-test.log` under the same artifact root.
The primary cycles precede G106's guard-only change; no post-G106 primary rerun
was performed after returning that monitor. Test engines and owned Chrome windows
are closed; primary is free.
