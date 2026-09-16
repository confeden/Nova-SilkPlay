# overlay-probe — S-M2 / S-M11 measurement harness

> **Results already obtained with this harness live in
> `.claude/kb/research/sm2-overlay-vs-capture.md`** (the overlay costs Chrome
> nothing; any WGC session costs it its overlay plane), and the decoder ring for
> the Chrome-side numbers is `.claude/kb/research/chrome-compositing-metrics.md`.
> Read those before re-running anything — several conditions are already settled.
>
> Full runs are driven by `run_sm2.py`, which owns the clock for both halves;
> `overlay_probe.exe` and `chrome_cdp.py` can also be used separately.

`overlay_probe.exe` is a **measurement instrument**, not product code. It exists to
settle spike **S-M2** (and to provide the negative control for **S-M11**) on the dev
machine, unelevated, with in-box Windows only: no PresentMon, no third-party
libraries, no downloads.

---

## 1. What S-M2 asks

Two questions that can kill the browser path outright:

1. **Does a DirectComposition overlay over Chrome cost Chrome its hardware plane?**
   This is gap **G5**, and it carries the "+1 refresh of latency" claim and the
   VRR rejection with it. If putting our overlay on top demotes Chrome's video
   from an MPO plane to a composed one, the latency budget and the VRR story
   both have to be rewritten.
2. **Does `Windows.Graphics.Capture` return real pixels while Chrome's video is
   on an MPO plane at all?** If WGC hands back a black or frozen frame in exactly
   the configuration we care about, no amount of overlay tuning saves the path.

`overlay_probe.exe` answers **question 2 directly** and produces **half of the
evidence** for question 1.

---

## 2. What this tool measures — and what it deliberately does not

### It measures

| Seam | Source | Written to |
|---|---|---|
| Our overlay swapchain's composition mode, per present | `IDXGISwapChainMedia::GetFrameStatisticsMedia` | `composition_mode` in the CSV, `composition_mode_histogram` in the JSON |
| Our present cost and vblank cadence | `Present1()` wall time, `SyncQPCTime`, `SyncRefreshCount` | `present_cpu_ms`, `mean_sync_qpc_interval_ms` |
| Whether the loop missed vblanks | `SyncRefreshCount` deltas between consecutive real presents | `missed_vblanks`, `missed_vblank_events`, `sync_refresh_intervals` next to `ticks` |
| What the run failed to observe | present attempts that produced nothing; WGC frames with no readable pixels | `presents.skipped/failed`, `captures.without_pixel_stats`, `wgc_frames_dropped_no_sample` |
| Whether WGC of the target window yields real pixels | `Direct3D11CaptureFramePool` (free-threaded) + a downsampled luma grid per frame | `mean_luma`, `std_luma`, `grid_hash`, `is_black`, `is_duplicate` |
| WGC frame rate, readback cost, content size drift | per-frame timing and `ContentSize` | `fps`, `p99_readback_ms`, `content_size_min/max` |
| What creation actually produced | `Overlay::CreationReport()`, `WgcCapture::StartupReport()` | echoed verbatim to stdout and into the JSON |
| Phase alignment markers | `PHASE_BEGIN` / `PHASE_END` lines on stdout | — |

### It does **not** measure

> **This probe cannot see Chrome's own composition mode.**

DXGI reports the presentation mode of *the swapchain you own*, and nothing else.
`GetFrameStatisticsMedia` on our overlay says whether **our** visual got a
hardware plane; it is silent about whether Chrome's video kept one. There is no
unelevated in-box API that reports another process's `PresentMode`.

The Chrome-side answer therefore comes from **`chrome_cdp.py`**, which samples
Chrome's own `GPU.DirectComposition.*`, `GPU.OutputPresenter.*` and `Compositing.*`
histograms over CDP with `delta=True`, phase by phase. The two tools are joined
after the fact by their **matching phase names** and their `PHASE_BEGIN` /
`PHASE_END` markers. Neither half is a finding on its own.

Also not measured here: scan-out latency (S-M4), geometry accuracy (S-M9),
audio sync, and anything about NVIDIA VSR (S-M12).

---

## 3. Build

```cmd
cd /d "D:\Documents\Coding\Nova SilkPlay\tools\overlay-probe"
build.cmd
```

`build.cmd` calls `vcvars64.bat`, compiles the four TUs
(`main.cpp`, `probe_common.cpp`, `overlay_win.cpp`, `wgc_capture.cpp`) into
`obj\`, and links `overlay_probe.exe`.

Flags: `/nologo /EHsc /std:c++20 /W4 /bigobj`, linking
`d3d11 dxgi dcomp dwmapi user32 dxguid windowsapp`.

**`/await` is intentionally not passed.** Under `/std:c++20` coroutines are on by
default and MSVC answers a bare `/await` with `Command line warning D9047:
option 'await' has been deprecated`. The house rule is a warning-free `/W4`
build, so the flag is dropped; C++/WinRT's `co_await` still compiles. If a future
toolchain ever needs it, use `/await:strict`, never bare `/await`.
`/bigobj` is kept: the C++/WinRT projection headers push `wgc_capture.obj` past
the 65535-section object limit.

Run everything **unelevated**. Elevation changes what WGC and the DWM will let
you observe and would make the numbers non-representative.

---

## 4. Finding the target window

```cmd
overlay_probe.exe --list-windows
```

Prints every visible, titled top-level window with `HWND`, PID, process image
name, class, window rect, client rect mapped to screen, and its DWM cloaked
state, then exits. Chrome typically has **several** `Chrome_WidgetWin_1`
top-level windows; note the PID of the one actually playing the video.

Without `--target-pid`, the probe takes the **largest client area** among the
windows that match class/title/pid, which is usually — but not always — the
maximised playback window. On a machine with more than one Chrome window open,
**pass `--target-pid` explicitly.**

---

## 5. The three runs that matter

Prepare the page first (a local MP4 avoids network jitter):

```cmd
python make_testpage.py --video "D:\path\to\clip1440p.mp4" --out testpage.html
```

Then, in each case, start `chrome_cdp.py run` on the same phase names in a second
console so the two reports can be aligned. The phase names below are chosen to
match; keep them identical on both sides.

### Run A — full client geometry, hidden vs shown (the primary S-M2 case)

```cmd
overlay_probe.exe ^
  --target-pid <CHROME_PID> ^
  --geom client --inset 0 ^
  --phases base_a:hidden:20,overlay_on:shown:40,base_b:hidden:20 ^
  --csv sm2_A_client.csv --json sm2_A_client.json
```

The overlay covers Chrome's whole client area. This is the harshest case for
Chrome's plane assignment.

### Run B — video-sized rect (the geometry we would actually ship)

```cmd
overlay_probe.exe ^
  --target-pid <CHROME_PID> ^
  --geom rect --rect 320,180,1280,720 ^
  --phases base_a:hidden:20,overlay_on:shown:40,base_b:hidden:20 ^
  --csv sm2_B_rect.csv --json sm2_B_rect.json
```

`--rect X,Y,W,H` is in **target-client pixels**, not screen pixels; the probe
translates it. Read the real video rect off the test page (or from
`--list-windows` plus the page layout) — do not guess it. Add `--inset 4` if you
need to see the page's own border underneath while eyeballing the run.

A rect that does not fit the client area is **not** silently accepted: it is
intersected with the overlay HWND rect, a `!!` warning names both rects, and the
JSON carries `visual_rect_requested` alongside the committed
`visual_rect_screen` with `visual_rect_clipped: true`. A rect entirely outside
the client area is a fatal error. (DComp would clip it away either way; the point
is that the report never claims coverage the run did not have.)

Runs A and B together are the "**both** candidate overlay geometries" the S-M2
directive asks for.

### Run C — the `WS_EX_TRANSPARENT` negative control (S-M11)

```cmd
overlay_probe.exe ^
  --target-pid <CHROME_PID> ^
  --geom client --no-transparent ^
  --phases base_a:hidden:20,opaque_overlay:shown:40,base_b:hidden:20 ^
  --csv sm11_C_opaque.csv --json sm11_C_opaque.json
```

Invariant **I9** says the overlay must always carry
`WS_EX_TRANSPARENT | WS_POPUP`, because Chromium's
`gfx::IsWindowVisibleAndFullyOpaque()` treats `WS_EX_TRANSPARENT` as an
unconditional early-out and would otherwise mark the tab occluded and throttle
it. This run deliberately removes it.

**The verdict for run C is not in this tool's output.** It is in
`chrome_cdp.py`'s rAF / rVFC counters: with `WS_EX_TRANSPARENT` the counter must
keep climbing during the shown phase; without it, it must stall. This probe's
job in run C is only to put the right window on screen at the right time and
emit the phase markers.

### Locked-step variant

Add `--stdin-sync` to this tool and `--wait-for-stdin-phases` to `chrome_cdp.py`,
and have an orchestrator feed both a newline per phase boundary. The probe keeps
ticking, presenting and polling WGC while it waits — the overlay does not freeze
during the handshake. If stdin reaches EOF the probe logs a warning and falls
back to each phase's timer rather than hanging.

---

## 6. Reading the output

### stdout

Every line carries a `[   1234.567]` prefix: milliseconds since process start.
The machine-readable markers are, after that prefix, exactly:

```
PHASE_BEGIN <name> <kind> <seconds>
PHASE_END <name>
```

A parser should match `PHASE_BEGIN\s+(\S+)\s+(\S+)\s+(\S+)` rather than
`startswith`, because of the timestamp prefix. Phase names may not contain
spaces, commas or colons; the probe rejects such names at parse time.

`Overlay::CreationReport()` and `WgcCapture::StartupReport()` are printed
verbatim, unprefixed, between rule lines. Read them: they say what the final
window styles actually were, whether `IDXGISwapChainMedia` was available at all,
which output the window landed on and that output's
`CheckHardwareCompositionSupport` flags, and — for WGC — whether the
`IsBorderRequired = false` request was accepted (gap **G14**: the setter succeeds
and is silently ignored without package identity, so believe the report, not the
HRESULT).

### The summary table

Two blocks, one row per phase.

* **presents** — `RAN` is `yes` only if the phase actually started; a phase that
  never ran (WM_QUIT, Ctrl-C, or an aborted run) shows `NO` and must not be read
  as "0 presents observed". `TICKS` is compositor clock ticks, `PRESENTS` is how
  many of them produced a **real** present (`--present-every N` decimates), and
  `SKIP` / `FAIL` are the two ways a present attempt produced no data: `SKIP`
  never reached `Present1()` (no back buffer / no RTV), `FAIL` reached it and got
  a failure HRESULT. Neither is in `PRESENTS`, in `CPU_*` or in the mode
  histogram — including them would pull the CPU percentiles toward 0 ms and add
  a fake `NA` bucket. `CPU_MEAN/P50/P99` is wall time inside `Present1()`,
  `SYNC_MS` is the mean interval between distinct `SyncQPCTime` values (on a
  165 Hz output an undecimated run should sit near 6.06 ms), `MISSED_VBL` is
  `missed/intervals` from `SyncRefreshCount` deltas between consecutive real
  presents — a delta of D > 1 means D-1 refreshes carried nothing of ours, and
  the denominator is how many deltas backed the number, so `0/0` means *no
  evidence*, not a clean run. `OVL/CMP/NONE/FAIL/NA` is the composition-mode
  histogram in the order OVERLAY / COMPOSED / NONE / COMPOSITION_FAILURE /
  not-available.
* **captures** — `FRAMES` is what WGC actually delivered (so `FPS` is an arrival
  rate), `NOSTATS` is how many of those frames arrived with unreadable pixels:
  their arrival fields are real but the map was skipped rather than waited for,
  so `%BLACK`, `%DUP` and `LUMA_*` are computed over `FRAMES - NOSTATS` only.
  Then the min/max `ContentSize` seen. Frames that were dequeued and produced no
  sample at all are counted separately, on their own line below the table.

Above the tables, a run that did not go to plan prints `!!` lines: a clipped
`--rect`, a target that moved, or fewer phases run than requested.

Then one explicit line:

```
WGC VERDICT: real pixels | black | frozen | no pixel data | no frames
```

derived from the whole run's capture stats with the thresholds printed
underneath it (≥90 % black → `black`; ≥95 % duplicates → `frozen`; zero frames →
`no frames`; frames arrived but none readable → `no pixel data`, which is an
*unanswered* question, not a pass). Those thresholds are **reporting heuristics
chosen in this file**, not measured constants.

Finally, the tool prints the caveat from §2 in full. It is repeated on every run
on purpose: the composition-mode column is about *our* swapchain, and a reader
skimming the table will otherwise read it as Chrome's answer.

### The files

* **`--csv`** (default `sm2_samples.csv`) — one flat file, 22 columns; the
  columns belonging to the other record type are left empty. The raw file is
  **complete**: samples the aggregates drop are still written, with a `rec` that
  says so.

  | `rec` | meaning |
  |---|---|
  | `present` | a real present; in every statistic |
  | `present_invalid` | no present happened (`SKIP`/`FAIL` above); in no statistic |
  | `capture` | a WGC frame with readable pixels |
  | `capture_nostats` | a WGC frame that arrived but whose pixels were never mapped: the arrival columns are real, every pixel column is blank |

  Every field of `PresentSample` and `CaptureSample` is present, plus
  `phase_index`, `phase_name`, `t_ms` (ms since process start) and
  `composition_mode_name`.
* **`--json`** (default `sm2_report.json`) — the options echoed back, the
  environment (DPI awareness, COM apartment, which compositor-clock path was
  taken), the resolved target, per-phase aggregates and the overall verdict.
  Hand-rolled; non-finite values are emitted as `null`. Fields worth knowing:

  | key | meaning |
  |---|---|
  | `target.visual_rect_screen` | what DirectComposition **committed** — inset applied, clipped to the overlay HWND. This is the coverage the run actually had. |
  | `target.visual_rect_requested` / `visual_rect_clipped` | what `--geom`/`--rect` asked for, and whether it had to be clipped to fit the frozen overlay HWND |
  | `target.client_rect_last_seen` / `dpi_last_seen` | the target's client rect at the last phase boundary |
  | `target_moved` / `target_moved_detail` | the target moved, resized or changed DPI mid-run, so the run was aborted |
  | `phases_requested` / `phases_run`, per-phase `ran` | how much of the schedule actually happened |
  | `presents[].skipped` / `.failed` | present attempts excluded from the statistics |
  | `presents[].missed_vblanks` / `.missed_vblank_events` / `.sync_refresh_intervals` / `.ticks` | missed-vblank accounting, with the tick count next to it |
  | `captures[].with_pixel_stats` / `.without_pixel_stats` | denominator for the pixel figures, and how many frames were excluded from them |
  | `wgc_frames_dropped_no_sample` | frames dequeued that produced no sample at all |

### Exit codes

| Code | Meaning |
|---|---|
| `0` | completed run (also used when Ctrl-C ended it early — check `"interrupted"` in the JSON) |
| `2` | fatal setup error (bad CLI, no matching window, overlay creation failed, an output file could not be written) **or an aborted run**: the target moved out from under the frozen overlay HWND, so the phases that did run cannot be qualified. The CSV and JSON are still written and `"target_moved"` is set. |
| `3` | the overlay reported device loss (`RenderAndPresent` returned false); the CSV and JSON up to that point are still written |

Closing the console window does **not** lose the outputs: the Ctrl handler blocks
for up to 4 s waiting for the run thread to flush both files (verified — closing
the console 6 s into a 25 s phase left a complete CSV and a parseable JSON).
Windows kills the process ~5 s after the handler is entered regardless, so a run
whose files take longer than that to write would still lose them.

---

## 7. Full CLI

```
--target-class STR      window class, case-insensitive exact. Default "Chrome_WidgetWin_1".
--target-title STR      title substring, case-insensitive. Default: any.
--target-pid N          exact pid. Default: any. Largest client area wins among matches.
--geom client|rect      what the DComp visual covers. Default client.
--rect X,Y,W,H          visual rect in target-client px; implies --geom rect.
--inset N               shrink the visual by N px per side. Default 0.
--no-transparent        drop WS_EX_TRANSPARENT (S-M11 negative control).
--no-topmost            drop WS_EX_TOPMOST.
--no-wgc                do not start a capture session.
--wgc-min-update-ms F   GraphicsCaptureSession::MinUpdateInterval, ms. Default 1.0.
--no-borderless         do not attempt IsBorderRequired = false.
--wgc-cursor            enable IsCursorCaptureEnabled. Default off.
--phases SPEC           NAME:KIND:SECONDS,... or the short KIND:SECONDS,...
                        Default "hidden:10,shown:20,hidden:10".
--present-every N       present once every N compositor ticks. 0 or 1 = every tick.
--stdin-sync            advance phases on stdin lines instead of the timer.
--csv PATH              default sm2_samples.csv
--json PATH             default sm2_report.json
--list-windows          dump candidate windows and exit.
--help                  usage.
```

---

## 8. Design notes worth knowing before you trust a number

* **One HWND, created once** (invariant **I11**). All geometry lives in the
  DirectComposition visual's offset and clip. `--rect` and `--inset` move the
  *visual*, never the window. If a run shows the window moving, that is a bug in
  `overlay_win.cpp`, not a measurement.
* **`overlay_win.cpp` owns the inset.** `main.cpp` hands `SetVisualRect()` the
  un-inset rect and reads `Overlay::CommittedVisualRect()` back for the report,
  so the inset is applied (and clamped) exactly once and the report quotes what
  DComp got, not what it was asked for.
* **The HWND is frozen at the target's client rect, so the target must not
  move.** `main.cpp` re-reads `GetClientRect` + `MapWindowPoints` (and the DPI)
  once per **phase boundary** — never on the tick path, where two extra USER32
  round-trips per tick would perturb the present cadence being measured. Any
  change aborts the run with exit 2 and `"target_moved"`, because an overlay
  sitting over stale coordinates while the report says "shown" is exactly the
  silently-wrong number this tool exists to avoid.
* **Pacing** is `DCompositionWaitForCompositorClock(0, nullptr, 100)`, resolved
  by `GetProcAddress` from `dcomp.dll`. If the export is missing the probe falls
  back to `Sleep(1)` — and says so, on stdout and in the JSON's
  `environment.compositor_clock`. A `Sleep(1)` run is not vblank-locked and its
  `SYNC_MS` column should be treated as suspect.
* **The message queue is pumped with `PeekMessage`, never `GetMessage`**, so the
  loop never blocks and the overlay keeps presenting across phase boundaries.
* **The process is `PER_MONITOR_AWARE_V2`**, set at startup. Every rect in the
  output is physical pixels. On the secondary 1920x1080 monitor at a different
  scale factor this matters; if the call ever fails the probe warns and the rects
  become untrustworthy.
* **The apartment is MTA.** `wgc_capture.cpp` uses the free-threaded frame pool
  and is polled from the main thread, so no `DispatcherQueue` is needed. The
  `CoInitializeEx` HRESULT (including a survivable `RPC_E_CHANGED_MODE`) is
  recorded in `environment.com_init`.
* **The capture device is separate from the presentation device**, by design —
  see the directives' "device topology hazard". Do not "optimise" them into one.
* **Gap G4**: `MinUpdateInterval` defaults to 0 and anything under 1000 µs is
  reported to cap WGC near 50 fps on build 26100. `--wgc-min-update-ms` defaults
  to 1.0 for that reason. If a run reports ~50 fps, check the startup report
  before concluding anything about the capture path.

---

## 9. Status

**Everything below is `unverified`.** No S-M2 run has been performed and recorded
yet; this section exists so that nobody mistakes the tool's existence for a result.

* `unverified` — whether Chrome keeps its MPO plane with our overlay shown, at
  either geometry.
* `unverified` — whether WGC returns real pixels while Chrome's video is on an
  MPO plane.
* `unverified` — the WGC frame rate actually achievable with
  `MinUpdateInterval = 1 ms` on this machine.
* `unverified` — whether `IsBorderRequired = false` is honoured for an
  unpackaged, per-user install (expected to fail per G14, but not yet observed).
* `unverified` — the S-M11 claim that dropping `WS_EX_TRANSPARENT` stalls
  Chrome's rAF counter.
* `unverified` — every latency, fps and VRAM number anywhere in this directory.
  The probe's own compile-and-run has been exercised against stub
  `overlay_win.cpp` / `wgc_capture.cpp`; it has **not** been run against the real
  implementations.
