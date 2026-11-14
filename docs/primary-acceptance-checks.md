# Primary acceptance checks (G103)

**Release verdict: FAIL.** Production still corrupts both reproduced film cuts.
Windowed transition checks pass; these results do not establish visual quality,
physical scan-out cadence, or whole-app recovery.

## Reproducible baseline

- Shipping executable SHA256:
  `217f90071fbf9a8271a12f5f01db71f59c211001c6413aca5f696e9893191857`.
- NVIDIA RTX 5060 Ti, driver 621.01; primary 2560x1440 at 164.999 Hz;
  secondary 1920x1080 at 60 Hz. Displays enumerated before testing.
- Disposable Chrome profiles, exact browser PID, unchanged engine executable.
  Synthetic CFR runs explicitly use `--scout-always`; film runs use the media gate.
- Only `codex-computer-use.exe` is exempted from laboratory occlusion checks.
- Evidence root: `artifacts/primary-check-20261106/`.

## Production cuts still fail

`film_arms.py`, local Tears of Steel, primary monitor, page/ours arms,
moments 36 and 303, two-second 640x360 center crops. Media clocks advance,
both engine recordings have zero coalesced updates and zero extra Chrome drops.
The engine shows its overlay and exits cleanly.

| Transition | Page control | Production output |
|---|---|---|
| Title, around 41.8 s | Clean text | Warped/fragmented text after the cut |
| Vent, around 307.5 s | Clean vent | Dark warped patches after the cut |

Contact sheets:

- `film/cuts_page_m0_cut56.png`, `film/cuts_ours_m0_cut238.png`.
- `film/cuts_page_m1_cut43.png`, `film/cuts_ours_m1_cut47.png`.

Page recordings each coalesced one update: useful as visual source controls,
excluded from numerical cadence comparisons. Engine recordings coalesced none.
G101's hidden secondary holds remain evidence for that scenario only. The cause
of the production/primary mismatch is unverified. Trace the actual capture
geometry, pair/field identities and cut flags before changing cut thresholds.

## Captured cadence

Same established CFR fixtures, source media 5-8 seconds, 2560x16 strip at
monitor-local (0,660), three three-second recordings per rate. All nine engine
recordings have zero coalesced updates and zero additional Chrome drops.

| Source | Recorded distinct pictures/s, three repeats |
|---|---|
| CFR24 | 140.63 / 140.76 / 140.70 |
| CFR30 | 134.98 / 134.76 / 134.99 |
| CFR60 | 108.39 / 112.39 / 113.58 |

Desktop Duplication observes captured changes, not physical scan-out. The CFR60
engine log also shows an unstable source-rate estimate and held phases; these
recordings do not isolate the cause. Present counters do not close P29.

The owner's Steam copy is LS 3.2.0 (earlier comparison used 3.2.2). Observed
profile: LSFG 3.1, Adaptive, target 165, flow scale 100%, Performance off;
profile unchanged. All three fresh CFR24 LS recordings coalesced updates
(2/12/11); exclude them from numerical rankings. Scaling was stopped and the
instance started for testing closed.

## Primary windowed mechanics

`windowed_check.py --local --monitor 1 --fps N`, exact owned PID.

| CFR | First show | Scroll hide / return | Pause hide | Verdict |
|---|---:|---:|---:|---|
| 24 | 3.12 s | 0.10 / 1.91 s | 0.40 s | PASS |
| 30 | 2.92 s | 0.10 / 1.71 s | 0.40 s | PASS |
| 60 | 2.82 s | 0.10 / 1.61 s | 0.50 s | PASS |

Theater, play, fullscreen and return also pass the rig's geometry checks.
All runs preserve foreground and exit cleanly, with no logged errors.
Full-screen rectangles are exactly (0,0,2560,1440). This is functional
acceptance, not an owner-by-eye smoothness or imperceptibility verdict.

## Secondary and hidden checks

- Current-source hidden Present fault test: **24/24 PASS**. This tests Overlay's
  rejected-Present/device-loss contract, not `App::Tick` or real TDR/sleep/lock.
- CFR30/60 resource diagnostics: **30 minutes each, PASS** (G104), with
  owner-authorized secondary-only topmost fixtures. See
  [resource results](secondary-soak-checks.md). Earlier covered runs remain invalid.
- Three visible 1920x1080@60 film repeats show neither named cut artifact in
  the recorded crops. This does not close the 2560x1440@165 failure: capture
  geometry and refresh changed together; their effects are not isolated.
- Remaining: actual system recovery, production primary cuts/pacing and
  owner blind A/B. DRM validation remains excluded (D35).

All owned test browsers and engines have stopped. The primary monitor has been
returned to the owner (D41); further checks stay secondary/hidden until renewed
primary access. Version remains `0.9.0-dev`; no installer/signing/publication.
