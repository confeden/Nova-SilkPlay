# Visible secondary resource checks (G104)

**CFR30 and CFR60: 30 minutes each, PASS under the existing resource limits.**
This closes the missing rate-specific resource diagnostics, not primary visual
acceptance or actual TDR/sleep/lock recovery.

## Scope

- RTX 5060 Ti, NVIDIA 621.01; secondary 1920x1080 at 60 Hz.
- Unchanged shipping EXE SHA256:
  `217f90071fbf9a8271a12f5f01db71f59c211001c6413aca5f696e9893191857`.
- Serial windowed CFR30/CFR60 fixtures, one scroll cycle per minute;
  exact owned Chrome PID. Synthetic muted fixtures explicitly use `--scout-always`.
- Owner-authorized `--topmost` raises only the owned secondary browser above
  other windows, with `SWP_NOACTIVATE` and unchanged-foreground checks. Primary
  and invalid monitor selections are rejected before launching a browser.
- Only the known `codex-computer-use.exe` cursor helper is ignored for occlusion;
  real covering apps retain the normal pause behavior.

Evidence: `artifacts/primary-check-20261106/secondary-soaks-topmost-v2/`.
Each rate directory contains page metadata/console output, raw CSV, engine log,
JSON report and a short desktop recording of the active image.

## Measured results

| Metric | CFR30 | CFR60 |
|---|---:|---:|
| Elapsed including shutdown, min | 30.084 | 30.084 |
| Scroll cycles | 30 | 30 |
| Overlay shows | 62 | 60 |
| Active nonzero-output intervals (required >=450) | 835 | 834 |
| Raw resource samples | 179 | 179 |
| Settled samples before run-warmup filtering | 154 | 160 |
| Working-set fit, MiB/hour (limit 50) | +29.66 | +13.90 |
| Private-byte fit, MiB/hour (diagnostic) | +1.78 | -2.51 |
| Handle fit, per hour (limit 200) | -2.26 | +1.31 |
| GDI object fit, per hour (limit 50) | 0 | 0 |
| USER object fit, per hour (limit 50) | 0 | +0.29 |
| Engine log errors | 0 | 0 |
| Engine exit code | 0 | 0 |

Fits retain samples from engaged pipelines at least two seconds after engagement,
then discard the first 60 seconds of the run. Raw samples remain available.
Working set is not flat; PASS means the measured growth is below the unchanged
limit. This is not proof of zero leakage under every condition.

In CFR60, Snipping Tool covered the secondary for about 4.7 seconds. The engine
logged PAUSED, then resumed and showed its output again. The covering app was
not exempted. CFR30 had no covering-window events.

Active-image recordings (`30/active-proof*`, `60/active-proof*`) show moving
fixtures with the output badge. Both recordings have zero coalesced updates;
their counts are not a physical-scan-out or smoothness verdict.

## Test rig change

`windowed_soak_page.py --topmost` and `film_arms.py --topmost` are opt-in,
secondary-only laboratory options. `HWND_TOPMOST` is passed as `wt.HWND(-1)`;
passing a plain negative Python integer without a pointer-sized HWND produced
WinError 1400 on this 64-bit process. Foreground remains unchanged and the owned
browser is closed during cleanup. No engine behavior or resource thresholds changed.

Remaining release gates are tracked in [primary acceptance](primary-acceptance-checks.md)
and `ROADMAP.md`: production primary cuts/pacing, broader visual/LS/blind A/B,
block-fallback cuts and real system recovery.

## Visible secondary cut controls (G103)

The unchanged production EXE was also checked on Tears of Steel, primary failure
moments 36/303, visible fullscreen on the 1920x1080@60 secondary, topmost owned
Chrome with normal media-session gating. Three serial repeats, two-second
640x360 center recordings per cut: the named warped-title/vent artifacts do not
reproduce in any of the six engine recordings. All six have zero coalesced
updates and zero additional Chrome drops; foreground unchanged, clean exit.

Recorded distinct pictures/s for title/vent: 59.34/58.52, 58.78/52.56,
58.98/58.27. These numbers are not a smoothness PASS. The page title control
coalesced 14 updates and is excluded from cadence comparison; the page vent
control coalesced none. Both remain visual source controls.

Evidence directories under `artifacts/primary-check-20261106/`:
`secondary-cuts-topmost/`, `secondary-cuts-topmost-r2/`,
`secondary-cuts-topmost-r3/`, including JSON, raw recordings and cut sheets.
The primary 2560x1440@165 recordings still fail. Resolution/capture geometry
and refresh changed together, so this experiment does not identify which
causes the difference and does not establish a fix.
