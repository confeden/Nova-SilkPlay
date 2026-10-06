# drm-probe — measuring the P11 DRM signals on this machine

Written 2026-09-20, **not yet run**. Every claim the DRM gate rests on is `unverified`
until this produces numbers on the RTX 5060 Ti.

The probe reads process metadata, a window title, one accessibility property and a PPM
the prototype already writes. It never captures, decodes or works around protection.
I7/D30: protected video is **recognised and skipped**, never circumvented.

## Why each run matters

| Q | Run | What it decides |
|---|---|---|
| Q1 | `tree`, `watch` | Does Chrome here use a **MediaFoundation CDM** (hardware DRM, capture goes black) or only the **library CDM** (Widevine L3, capture returns real pixels)? If L3 only, the pixel branch has **zero** coverage and the process scan is the entire gate. |
| Q2 | `tree` | Are *all* chrome.exe children's command lines readable unelevated, and how long does a scan take? Decides whether a partial scan may be treated as CLEAR. |
| Q3 | `watch` | How long a CDM utility process lingers after navigating away — sets `kCdmGoneDwellSec` (design assumes 10 s). |
| Q4 | `uia` | Does a UIA URL read work on a **fullscreen** window, how long, and does Chrome's CPU rise afterwards? Decides whether UI Automation is in the beta at all. |
| Q5 | `band` | What WGC actually returns over protected video, measured with the shipping band constants — sets `kDeadBandFrac`. |

## Recipe

Use the **60 Hz secondary monitor** and do not disturb the owner's foreground work.

```
cd "D:\Documents\Coding\Nova SilkPlay\tools\drm-probe"

:: 0. baseline — YouTube playing, nothing protected anywhere
python drm_probe.py tree

:: 1. Q1/Q3. Start this FIRST, then in Chrome start Netflix (or Disney+ / Prime),
::    let it play ~30 s, then navigate THAT TAB to about:blank and wait.
::    The navigate-away -> GONE gap is the answer to Q3.
python drm_probe.py watch --seconds 240

:: 2. Q4. With the protected tab in FULLSCREEN (F11 or the player's own button):
python drm_probe.py window --title "Netflix"
python drm_probe.py uia    --title "Netflix"
::    Repeat with --title "YouTube" and record the exact fullscreen titles.

:: 3. Q5. The pixel half, using the prototype that already exists — no second
::    WGC harness. With protected playback fullscreen on the secondary:
cd "D:\Documents\Coding\Nova SilkPlay\prototype"
silkplay.exe --target-title "Netflix" --allow-windowed --dump drm --dump-after 5
cd "..\tools\drm-probe"
python drm_probe.py band ..\..\prototype\drm_a.ppm
python drm_probe.py band ..\..\prototype\drm_b.ppm
::    Then the false-positive margin, on UNPROTECTED content:
::      - a genuine night scene on YouTube
::      - a frame inside a fade to black
::      - a 2.39:1 film, so the letterbox bars are in shot
::    Their bright-block fractions must sit well ABOVE whatever step 3 reported.
```

`--dump` ends the run, so each dump is its own short launch. `band` is deliberately a
pure-Python reference of the shipping rule — it is slow (seconds per 1440p frame) and
that is fine.

## Reading the result

- **Q1 = hardware CDM present** → the black branch is real. Set `kDeadBandFrac` an order
  of magnitude above the measured protected fraction, and keep `kDeadSec` at 3.0 s so no
  fade-to-black can reach it.
- **Q1 = library CDM only** → say so in `negative.md`: on this machine the pixel branch
  never fires, and the **CDM utility process is the only thing standing between the beta
  and generating over protected video**. The gate must then never ship with the process
  scan optional.
- **Q2 shows unreadable children** → the "partial scan is still CLEAR" concession in the
  plan has to be revisited; a scan that cannot see part of the tree cannot be
  authoritative, and the gate falls back to fail-closed on that browser.
- **Q4 fails any of its three tests** → drop UIA from the beta, scope by window title,
  and record the reason.

Record everything in `F:\Claude\Nova SilkPlay\kb\` (a new `kb/drm-gate.md`), with the
gotchas in `gotchas.md` and anything disproven in `negative.md`.
