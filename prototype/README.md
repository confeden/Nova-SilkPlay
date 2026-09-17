# prototype — the browser path, end to end

`novasilk.exe` is the first thing in this repo that actually generates frames.
It captures a browser window, keeps the last two source frames on the GPU,
estimates motion once per source pair and presents a synthesized frame on a
DirectComposition overlay every compositor tick.

This is **ROADMAP P4**, not the product: the video rect comes from the command
line, and the source cadence is inferred from capture arrivals instead of read
from the page (I8 says the product must read it from the page). What it does
prove is that the pipeline shape works and that the numbers land where the
architecture said they would.

## Build and run

```cmd
build.cmd
silkplay.exe
```

That is the whole command. With no arguments it looks for a **Google Chrome
window in fullscreen** and attaches to it: the FOREGROUND one if several qualify,
otherwise the largest. Fullscreen only is the owner's rule until the video rect
comes from the page (ROADMAP P11): on a windowed browser the overlay would cover
the whole page, and every interaction with it would be shown a source period
late. Fullscreen means what Chrome does for `F` on a video or for `F11` — the
window rect is its monitor's, with no caption, no sizing frame and not maximised
(a maximised window with an auto-hidden taskbar also covers the monitor, so the
frame bits are checked too). Leaving fullscreen detaches on the same iteration.
`--allow-windowed` restores attaching to a windowed browser, and `--rect` implies
it. It filters on the process image (`chrome.exe`) as well as the window
class, because `Chrome_WidgetWin_1` is shared by every Electron and CEF app on
the machine — the Claude desktop app answers to it here, and class alone would
happily attach to that instead.

If no target exists yet it **waits** and retries once a second rather than
exiting, saying so once and then every 30 attempts, so it can be started before Chrome and left running in the background;
the same loop re-attaches when a window closes or the video moves. Override any
of it with `--target-exe`, `--target-class`, `--target-title` or `--target-pid`
(an explicit pid clears the exe filter), and `--list-windows` still prints the
candidates.

Everything is unelevated, in-box Windows only: D3D11, DirectComposition,
Windows.Graphics.Capture, `D3DCompile` at startup. No SDK, no downloads.

With no `--rect` the overlay covers the whole client area, which in fullscreen IS
the video. For windowed video pass `--rect X,Y,W,H` in target-client physical
pixels; reading the real video rect off the page is ROADMAP P11 and is not built.

The overlay stays hidden until a source pair arrives and hides again after 400 ms
with no new frame, so a paused video or a static page shows the browser's own
output and nothing of ours.

## The pointer, and windows over the video

**The pointer on the video changes nothing.** The overlay is
`WS_EX_LAYERED | WS_EX_TRANSPARENT`, and that combination — not `WS_EX_TRANSPARENT`
alone, and not answering `WM_NCHITTEST` with `HTTRANSPARENT` — is what makes a
top-level window transparent to input from another process. Measured with real
`SendInput` over a shown overlay (`tools/clickthrough-probe`): without LAYERED the
page received none of a click, four moves and a wheel notch and the overlay took
38 hit tests; with it the page received all of them, the overlay took none, and
Desktop Duplication still read the overlay's pixels off the screen. The old
behaviour — hide while the pointer is on the video — made fullscreen video, where
the pointer always is, almost never generated. It survives as `--cursor-gate`,
and as an automatic fallback: a click-through window never receives a hit test,
so if ours receives three the engine logs it and pauses under the pointer again.

**The overlay sits one z-order slot above the browser, not in the topmost band.**
Whatever the user puts in front of the browser is therefore drawn over our
picture by DWM on its very first frame — a topmost overlay painted over it until
the occlusion test noticed. Activating the browser raises it above us; the loop
puts the overlay back every iteration. Getting back is not obvious: from a
background process `SetWindowPos(HWND_TOP)` returns success and does nothing when
the browser is in the foreground, so the overlay is inserted after the window
directly above the browser (Chrome's own invisible IME window), or promoted to
topmost and demoted, which lands it at the top of the normal band. The result is
checked, and after 60 failed attempts in a row the overlay falls back to topmost
for that engagement and says so. `--overlay-topmost` forces the old placement.

**Generation pauses while any window covers any part of the video**, and resumes
when it is gone. The test walks the windows above the browser using their
*extended frame bounds* (`GetWindowRect` includes an invisible 8 px resize border —
a maximised window on the primary reaches 8 px into the secondary), their window
region, and skips cloaked, minimised and fully transparent layered windows — and
layered click-through windows with no alpha of their own, the overlays and helper
windows that draw nothing (every winit/Tao app keeps a visible 16x16 one at 0,0);
whatever they do draw is above the browser and so above us anyway. A
window touching the edge of the picture must reach at least 8 px into it: a
resident topmost utility here sits one pixel column over the primary. It runs the
moment windows change (WinEvent hooks: foreground, show/hide, cloak, minimise,
move/size), on every iteration while a window is being dragged, and ten times a
second regardless; a minimised or cloaked browser counts as covered, and the
browser moving hides the overlay on the same iteration. Measured
reaction: 1-18 ms for a window that appears, ~26-29 ms for one a program slides in
without any event, 0.01-0.2 ms per test.

**Widgets do not pause it** (owner's decision): an always-on-top window no bigger
than a tenth of the picture — a resident ticker or clock, a toast, a tooltip, a
picture-in-picture player. It is above the browser and so above our overlay, so it
is drawn correctly anyway; generation simply keeps running around it, and the
first time each one is seen the log says `ignoring always-on-top widget`. Without
this, the Quotty widget over the secondary monitor's bottom-left corner kept
fullscreen video there paused for the whole run (measured). A bigger always-on-top
window, and any ordinary window of any size, still pauses. `--pause-for-widgets`
restores the strict rule; `--occlusion-ignore quotty.exe,other.exe` exempts named
processes whatever their size (spaces, paths and a missing `.exe` are tolerated;
the list is logged at startup). Only one engine runs per session:
two would take turns stealing the slot above the browser.

## Which captures are video frames

Capture delivers a frame whenever anything in the browser window changes, and a
player changes a lot besides its video: the controls fade in when the pointer
moves and out a few seconds later, the clock and progress bar tick, a play/pause
flash animates. Treating each of those as a new source frame is what used to make
the pointer ruin generation even when nothing was hidden. Measured on the test
page's YouTube-like controls with a real `<video>` (`dirty_experiment.py`):

| | est. source rate | judder (mean, share of presents > 4 ms) |
|---|---|---|
| pointer moving, `--no-frame-test` (old) | 165 / 110 fps | 12.1 ms, 25/31 |
| pointer moving, default | 23.7 fps | 1.1-2.7 ms, 2-8/30 |
| controls up, old | 43 → 27 fps | 15.7-17.9 ms, 28/30 |
| controls up, default | 23.6 fps | 1.0-1.4 ms, 2-4/30 |
| controls fading out, old | 165 fps | up to 21.7 ms, 30/30 |
| controls fading out, default | 23.6 fps | 1.0-2.5 ms |

Each capture is tested twice, cheapest first. DWM reports dirty regions with every
frame (build 26100). Damage that misses the middle band of the picture — the
clock, the window border — cannot be a video frame. Damage that is small for this
picture is dismissed too unless it has the shape of the picture itself: one
`<video>` damages the same rect with every frame, while a play/pause flash or a
spinner changes shape every frame (until that shape is known, a small capture
arriving when the next frame is due gets the benefit of the doubt). Damage alone
cannot separate a fade from a video frame, because Chrome reports one union rect
per compositor frame and a top and bottom bar fading together damage the whole
picture; so a compute shader then compares EVERY pixel of the middle band (6-94 %
of the width, 20-72 % of the height, clear of title bars, controls and captions),
in 16x16 blocks sampled every second pixel, against the last video frame as it was
first captured. A sparse 64x24 grid was tried first and missed real frames: a small
object's edges moved between the samples and scene A3 advanced 13.7 of 24 times a
second. The same decoded frame recomposited
under a fading bar is bit-identical there, and comparing against the first capture
rather than the latest refresh lets slow change add up. A capture that passes
advances the pair; one that fails becomes the newest picture but keeps its
timestamp, so the controls stay current while the pair and the cadence do not
move. The answer is read back ASYNCHRONOUSLY (`D3D11_MAP_FLAG_DO_NOT_WAIT`, collected
on a later poll; no further capture is taken meanwhile, so verdicts stay in order):
a blocking map waits behind the warp and NVOFA and measured 3.5-4.4 ms on average and
20 ms at worst at fullscreen. The `capture:` stats line reports how long answers
took. `--log-dirty` prints every verdict.

A still shot — or animation held on one drawing — does not advance the pair
either, and two things keep that from showing. The source counts as alive while
the browser keeps repainting the picture, not only while it advances: before that,
a video alternating 2 s of motion and 1.5 s of stillness hid the overlay three
times in 14 s, each hide and re-show a visible jump between our picture (a period
late) and the browser's. And when the first moving frame lands the pair is
measured from one period before it rather than from the frame before the still,
so motion starts from the picture already on screen instead of holding and then
jumping. Measured with `still_experiment.py`: 0 hides.

A vertical video in a fullscreen player damages only a third of the band while a
controls fade damages all of it; measured with `?vw=32`, its frames all survive the
fades (23.4-23.7 fps estimated, 0.2-1.1 ms mean judder in every phase except the
click).

Global hotkeys while it runs:

| Key | Effect |
|---|---|
| `Ctrl+Alt+Q` | frame generation on / off — the honest A/B, off = the browser's own output (`Ctrl+Alt+O` does the same) |
| `Ctrl+Alt+M` | cycle mc → blend → passthrough |
| `Ctrl+Alt+←/→/↑/↓` | nudge the capture rect by one pixel (alignment check) |
| `Ctrl+Alt+X` | quit (**X**, not Q) |

A hotkey another program already owns fails to register; that is logged at
startup and nothing else breaks.

## The frame-rate readout

`24/60` — source fps / output fps — in plain white pixel-font text with a thin dark
outline, fixed 12 px in from the top-right corner of the **video rect**, the way
Lossless Scaling shows its counter. It never moves and never reacts to the cursor:
the overlay is `WS_EX_TRANSPARENT` and must never take the pointer (I9, I12). The
numbers carry hysteresis so they do not flicker between two neighbours. Glyphs are
2x the 5x7 font (3x at 1400 px of video height and up), drawn by the same shader
pass system as everything else (the string and a 0-9 and `/` font table packed into
the constant buffer), so it needs no font, no Direct2D and no texture.

`--no-badge` turns it off, which is required for a bit-exact passthrough alignment
check.

## The three modes

* **passthrough** — a bit-exact `CopyResource` of the newest captured frame onto
  the overlay. Nothing is synthesized, so anything visible here is a geometry or
  colour bug, not an interpolation artifact. This is the I13 check.
* **blend** — a linear-light cross-fade at the continuous phase t. Smooth, and
  it ghosts on motion. It is the reference the real synthesizer has to beat.
* **mc** — motion-compensated. Motion comes from NVOFA by default (`--motion
  ofa`, one execute per source pair, both directions and per-vector cost, 4 px
  grid) or from the pyramid block matcher (`--motion blocks`: three passes on a
  /8, /4, /2 luma pyramid, 8 px cells, final search step one full-res pixel,
  median smoothing plus a smoothness penalty, and a MAGNITUDE prior at the
  coarse level — see G30: without it the matcher locks onto "true motion plus one
  texture period" on anything that repeats along its own direction of travel,
  which is invisible to the matching cost and catastrophic in the interpolated
  frame. On NVOFA, hint mode with a zero hint buffer is what keeps A4 right —
  G54: our own seeds never reached the hardware, and when they do they are
  worse). Either way the field is re-anchored to the intermediate frame and
  given the smoothness prior the hardware lacks (G24); on the hardware path a
  **coherence pass** then lets every cell re-choose its vector from its 5x5
  neighbourhood by match quality plus a truncated pull towards its neighbours,
  twice per pair (`--no-field-cohere` turns it off). The symmetric warp at phase
  t takes eight candidate vectors per pixel and **blends their colours by a
  softmin over how well each one matches**, each colour with the occlusion test
  below applied along its own vector. It used to keep only the cheapest
  candidate, and on real footage that drew hard-edged fragments along every
  moving edge (G50, P21): near-ties between very different vectors flipped from
  pixel to pixel. Measured against that version: spurious edges on real 1440p
  footage -67..-73 %, analytic scenes +1.05 / +4.08 / +3.36 dB (A1 / A3 / A4).

  **Scene cuts** (P23): once per pair the GPU compares tone histograms of the two
  frames and how much of the picture the field still cannot explain. A hard cut
  holds the frame before it until the next one is due — the cut stays a cut
  instead of a mosaic of both shots — and a pair that is merely unexplainable
  (something sweeping in from off-frame) is cross-faded. No readback: the
  decision is a 1x1 texture the warp samples. `NSP_CUT_STATS=1` logs it per pair.

  `--warp-lab N [--warp-lab-p F]` and `--field-lab N [--field-lab-p F]` run the
  variants P21 compared (the pre-P21 warp is `--warp-lab 99 --no-field-cohere`);
  `harness/warp_lab.py` scores them on real footage.

### The occlusion test (`--occ`)

The single thing that separates a usable MEMC from a broken one. A field hands
the pixels *around* a moving object the object's vector, so the warp fetches the
object where the background should be — that is the halo, and trusting those
fetches is what produces black holes and grey ghost wings.

The warp samples a motion field again **at both fetch sites**. Where the pixel
really travels along the vector used, the field there agrees with it; in a halo
it does not, and that disagreement decides which single source frame to believe
instead of averaging both. (Before P21 a pixel where neither side was consistent
fell back to a re-fetch and then to the unwarped cross-fade; the softmin over
candidates replaced that branch.)

*Which* field is sampled is the whole argument, and it is switchable because it
is a quality claim:

| `--occ` | What the two consistency samples come from |
|---|---|
| `self` | the intermediate field, twice. It can say "this trajectory is inconsistent" but never *which* frame lost sight of the pixel — both answers come from one estimate |
| `bidir` | the A-anchored field at the A fetch site, the B-anchored one at the B fetch site: independent evidence, so the two answers can actually disagree |
| `bidir+cand` | default. As `bidir`, and both fields are also offered to the per-pixel vector search — where an object has just uncovered background, every neighbouring intermediate cell carries the object's vector and only the anchored fields still hold the background's |

`bidir`/`bidir+cand` need the hardware path: the block matcher produces one field
and has nothing to be bidirectional about, so it silently uses `self`.

Measured on the analytic occlusion scene (`harness/run_analytic.py`, scene A3,
the generator's own occlusion map as the mask): **47.88 → 48.61 → 49.14 dB** over
the 99.5 % of the frame that is not occluded. Inside the occlusion band all three
sit near 20 dB while simply holding the frame that can see the pixel would give
41 — so the evidence is right and the decision rule is not. On the photographic
corpus the whole change is neutral (44.90 vs 44.91), because 0.21 % of pixels
move by up to 106 levels and a frame mean cannot see that.

### Output rate follows the monitor

A desktop can mix refresh rates, and the DirectComposition clock is desktop-wide
— it ticks at the FASTEST output. On a 165 Hz primary with a 60 Hz secondary,
dragging the window to the secondary would otherwise keep generating 165 frames
a second for an output that scans out 60. The prototype reads the refresh of the
monitor the video rect is on (`QueryDisplayConfig`, rational, falling back to
`EnumDisplaySettings`), re-reads it whenever the window moves or the mode
changes, and paces presents down when that monitor is slower than the fastest
one. On the fastest monitor there is no gate at all — gating there beats against
the compositor clock and costs ~20 fps for nothing (measured).

`--list-displays` prints what it sees.

## What was measured on the dev machine (RTX 5060 Ti, 2560x1440 @165 Hz)

Source: a local page compositing at 23.57 fps, and a 60 fps H.264 `<video>`,
both in a 1745x983 Chrome app window.

| | 23.57 fps source | 60 fps source |
|---|---|---|
| capture arrivals | 23.5 fps | 61 fps |
| estimated source rate | 23.57 fps | 60.00 fps |
| presented | 165.0 fps | ~161 fps (mc), 165.0 (blend) |
| our swapchain's plane | `OVERLAY` | `OVERLAY` |
| phase clamped ("held") | ~11 % of ticks | ~15 % (mc), 2 % (blend) |

Synthesis correctness, from `--dump` (a 77 px white bar moving 42 px per source
frame, measured on the written PPMs):

```
A            x = 711..787  (w77, luma 254)
B            x = 753..829  (w77, luma 254)
mc  t=0.50   x = 733..809  (w77, luma 253)   <- midpoint, sharp, 1 px error
mc  t=0.25   x = 722..798  (w77)             <- quarter point
blend t=0.50 x = 711..829  (w118, luma 208)  <- the two bars smeared together
```

So the motion-compensated frame really is the frame that belongs between the two
sources, not an average of them.

## Known defects (all deliberate, none of them blocking at this stage)

* **The NV12→BGRA8 flicker at engage.** Starting a WGC session moves Chrome off
  its hardware overlay plane and latches BGRA8 (G17), which can flicker once.
  Accepted for now — see D23 in ROADMAP.md.
* **The occlusion band is still wrong, and now by a measured amount.** The warp
  picks ONE vector per pixel and then weights A against B; in an occlusion band
  no single vector is right, because the pixel exists in only one of the two
  frames. Measured on scene A3: on pixels visible only in A, holding A scores
  41.13 dB and the engine 19.6–20.5; only in B, holding B scores 36.81 against
  22.2. The bidirectional fields made this evidence correct without fixing the
  rule that consumes it (ROADMAP P14). NVOFA's per-vector cost is computed by
  the same execute and is still unused.
* **The cadence is inferred, not told.** The period is a trimmed mean of arrival
  intervals, because the median is wrong for any source that beats against the
  refresh (a 60 fps source on 165 Hz arrives 3,3,3,2 refreshes apart; the median
  says 55 fps, the mean says 60). Frame stamps are presentation times, not content
  times — Chrome shows 24 fps on 60 Hz 33 and 50 ms apart — so the content timeline is
  a least-squares line through the last 32 frames and the picture is shown one period
  plus the capture delay plus a jitter margin (95th percentile of lateness) behind it,
  with a frame of history (and the previous pair's fields) kept so the moment on screen
  is always inside a pair already in hand. Measured with `tools/ls-compare` at 60 Hz:
  recorded output frames that are held source frames fell from about half to 30 % (A3)
  and 26 % (A4), against Lossless Scaling's 32 % and 23 %. The `pacing:` stats line
  reports the lock, the margin and how often the older pair was used.
* **No DRM detection, no game gate, no audio.** None of that is in this binary.
* **Activating the browser costs a frame or two of its own output.** A click
  raises Chrome above the overlay until the next loop iteration puts it back, so
  for that moment the viewer sees the un-generated picture. Not measured by eye.
* **The video-frame test has blind spots.** UI that changes inside the middle
  band while the rest of the window is repainted — measured: the play/pause flash
  in the half second after a click that also activates the browser, 4-10 changed
  samples against ~200 for a frame — counts as a video frame while it animates
  (judder ~5 ms, source estimate briefly 25-47 fps). A seek-preview thumbnail or
  captions placed high would do the same. A
  video whose middle band is truly static while only its edges move (a slide
  with a moving ticker) is not advanced and holds instead of being interpolated.
  Windowed video without `--rect` puts the band over page content. All unverified
  on real YouTube; measured only on the test page.
* **A bigger window left permanently over the video pauses generation
  permanently** — widgets are exempt, anything larger than a tenth of the picture
  is not. `--occlusion-ignore` is the escape hatch.
* **Windowed video is not generated at all** without `--allow-windowed` or
  `--rect`, by the owner's choice, until P11 reads the video rect from the page.
* Moving or resizing the target window tears everything down and re-engages
  (the overlay HWND is frozen by I11).

## Files

| File | What it owns |
|---|---|
| `main.cpp` | CLI, engage/disengage, the cadence estimator, the pacing timeline, the phase, the loop |
| `nsp_overlay.*` | the D3D11 device, the overlay HWND, DComp, the composition swapchain |
| `nsp_capture.*` | WGC → a five-slot GPU ring of video-rect crops (older, previous, reference, newest, pending), and the test that tells a video frame from a UI-only recomposition |
| `nsp_winwatch.*` | WinEvent hooks that re-run the occlusion test the moment windows change |
| `nsp_synth.*` | the luma pyramid, the block matcher, the three motion fields (and the held copy of the previous pair's), the warp, the cross-fade and the frame-rate readout |
| `nsp_ofa.*` | NVOFA through its D3D11 entry point: our device, our textures, both directions in one execute |
| `nsp_offline.*` | `--offline`: the same `Synth` over frames from disk, with the G0/G1 gates |
| `nsp_dump.*` | texture → PPM, the only way to judge synthesis off-screen |
| `ppm2png.py` | PPM → PNG with crop and box downscale (stdlib only) |
| `testmotion.html` | a deterministic low-frame-rate motion source: `?fps=24&speed=900`; `&input=1` counts pointer input into the title, `&controls=1` adds YouTube-like controls, `&video=record` records the animation to WebM and plays it through a real `<video>` |
| `live_windows_test.py` | real input and real windows over the running engine (with `--allow-windowed`): click-through, pointer on the video, z-order after activation, occlusion pause/resume and its reaction time, widgets ignored and big always-on-top windows not (17 checks) |
| `dirty_experiment.py` | drives the pointer through idle / moving / controls up / fade-out / click and prints per-phase verdicts, source-rate estimate and judder; pass `--no-frame-test` for the A/B |
| `fullscreen_gate_test.py` | toggles the test page in and out of fullscreen with F11 under the engine's default rules: waits while windowed, attaches and shows in fullscreen, detaches on leaving (8 checks) |
| `still_experiment.py` | 14 s with the pointer away on a video that alternates motion and stillness (`?move=2000&still=1500`): overlay hides, verdicts, judder |

## Judging a change

```cmd
novasilk.exe --target-title "..." --mode mc --dump shot --dump-after 5
python ppm2png.py shot_mc50.ppm view.png --crop 600,100,500,400
```

`--dump` writes `_a`, `_b`, `_mc50`, `_mc25` and `_blend50` PPMs of the two
source frames and what the synthesizer puts between them, then exits. A hard
moving edge in `_mc50` must be ONE edge at the midpoint; two faint edges mean
the motion path silently fell back to blending.

Window and pointer behaviour, and pacing under player UI, are judged live. Both
scripts move the real cursor and click the test page, so open it on the secondary
monitor first:

```cmd
python run_testpage.py --url "file:///.../testmotion.html?fps=24&input=1&controls=1&video=record&seconds=8"
python live_windows_test.py
python fullscreen_gate_test.py
python dirty_experiment.py
python dirty_experiment.py --no-frame-test
```

Every stats line now carries the judder metric — how far the displayed content
time moved from the wall time between two presents, with presents where the
source is holding still and restarts after a still counted apart — and a `capture:` line
(video frames vs UI-only refreshes, content-test readback) and a `windows:` line
(z-order corrections, occlusion tests and their cost, hit tests on the overlay).
