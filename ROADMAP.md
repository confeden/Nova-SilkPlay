# Nova SilkPlay — project state

Real-time video frame generation (24/30/60 fps → the display refresh, 165 Hz) for browsers and players on NVIDIA RTX 50. The browser path runs end to end on real YouTube (`prototype/`); the work now is the first beta (D30).

## Status
| ID | Component | State | Evidence |
|---|---|---|---|
| S1 | Technology research, 14 dimensions, adversarially verified | ok | `kb/research-findings.md`, `kb/research/*.md` |
| S2 | Dev environment | ok | `kb/dev-environment.md` |
| S3 | Lossless Scaling reference analysis | ok | `kb/lossless-scaling.md` |
| S4 | Architecture decision | ok | `kb/architecture.md` — 4 designs, 5 judges, 3 red-teams |
| S6 | Delivery plan to v0.9 | ok (written before D30: assumes the extension) | `kb/plan-v09.md` |
| S7 | Owner directives D-A/D-B/D-C researched | ok | `kb/research/directives-2026-09.md` |
| S8 | GPU/OS capability probe built and run on the dev GPU | ok | `tools/caps-probe/`, `kb/dev-environment.md` |
| S9 | Five gaps closed; NVOFA measured on GB206 | ok | `kb/research/gaps-closed-2026-09.md`, `tools/ofa-probe/` |
| S10 | S-M2 harness: overlay+WGC probe, Chrome-side CDP driver | ok | `tools/overlay-probe/README.md` |
| S11 | Owner directive D-D — imperceptibility is a release gate, tray settings | ok | I12, I13, D22 |
| S12 | **Browser path end to end: WGC → GPU ring → motion per pair → warp per tick → DComp overlay** | ok | `prototype/README.md` |
| S13 | Quality harness: `--offline` replay (G0/G1), per-frame scoring, `--strata`, true occlusion maps | ok | `kb/quality-harness.md` |
| S14 | P13-a bidirectional evidence `--occ`: +1.95 dB off the band, nothing in it | ok (measured, mixed) | G31 |
| S15 | Oracle-flow reference (`--offline-inject`, arm `oracle:<motion>`), G0 bit-exact | ok | `kb/quality-harness.md` |
| S16 | Anti-overfit scenes + `gate.py`, `panprobe.py`, `costscape.py` | ok | `kb/quality-harness.md` |
| S17 | G30 fixed on both motion paths: refine-before-score, NVOFA external hints ON | ok (measured) | G30, N23, G32 |
| S18 | Click-through overlay, one z-slot above the browser, occlusion pause with widgets exempt, fullscreen-only attach | ok (17/17 + 8/8 live) | G35, G36, G38 |
| S19 | Player-UI recompositions do not advance the pair | ok (test page; fullscreen YouTube in S25) | G37, G40 |
| S20 | Competitor arm vs LS on analytic clips at 60 Hz: ahead per generated frame on A3/A4 | ok (measured) — **overturned on real content by S25** | `kb/quality-harness.md`, G41-G43, D28 |
| S21 | P17 pacing: regularised content timeline + jitter margin + older pair — held frames at LS's level | ok (measured, 60 Hz) | `kb/quality-harness.md` |
| S22 | Tool survey: learned flow, visibility, NVIDIA VFX SDK Video Frame Generation | ok (research) | `kb/research/browser-quality-tools-2026-09.md`, G45, N24 |
| S23 | Learned-flow evaluation environment (CUDA PyTorch venv, SEA-RAFT verified) | ok | `kb/dev-environment.md#learned-flow-evaluation-environment-s23-p18` |
| S24 | Score nondeterminism localised to the hint-seeded NVOFA arm | ok (measured) | G49, N25 |
| S25 | **Stage 0, real YouTube fullscreen on the 165 Hz primary**: mechanics solid (source 24.00/60.00 exact, 163-165 out, judder ≤ 0.1 ms, UI refreshes rejected, 0 Chrome drops, engage 1.5 s); **pictures worse than LS** — hard-edged fragments at motion boundaries | ok (measured, 1440p) | `kb/quality-harness.md` (real content), G50, `tools/yt-check/` |
| S26 | Repository `github.com/confeden/Nova-SilkPlay` (public): sources, tools, docs; data and builds stay local | ok | `.gitignore` |
| S5 | Remaining spikes S-M3..S-M12 | planned | `directives-2026-09.md#6` |

The KB index at the end is the map. Re-run `tools/caps-probe` and `tools/ofa-probe/*.py` after any driver bump; `kb/dev-environment.md` holds what this machine reports, not what docs claim.

## Now
**First beta (D30): Chrome only, no browser extension, signed with the owner's local certificate; PotPlayer after it.** Beta gate, all on the 165 Hz primary with 1440p sources: YouTube in Chrome, fullscreen and windowed, 24/30/60 fps, runs 30 min with no crash and no D22 defect; not worse than LS on the analytic matrix at 165 Hz, on real content side by side (`tools/yt-check/yt_arms.py`) and in the owner's blind A/B; protected video skipped; a game pauses generation; autostart and tray settings; a signed installer.

**Where we stand (S25).** The machinery is beta-grade on real YouTube: cadence, pacing, UI-refresh rejection and engage hold at 165 Hz for 24p and 60p, and Chrome drops nothing. The pictures are not: on real 1440p footage our generated frames carry hard-edged fragments along moving edges (G50) where LS is soft and clean. The analytic scenes ranked us ahead (S20) because flat hard-edged layers hide a wrong per-pixel choice inside a mean. Quality work therefore restarts from real content: find the switch that draws those edges (P21) with a measurement that can see them (P22), then cuts (P23), then motion estimation (P18) and visibility (P14).

**Engine facts that still hold.** Motion estimation dominates the error on every analytic scene (S15); the occlusion band needs a real visibility signal (G31). Driver 616.92 changes nothing. The engine is deterministic except the hint-seeded NVOFA arm (G49): run NVOFA arms serially, repeat them, report both outcomes (N25). The picture is shown one source period plus a ~4.5 ms margin late (24p ≈ 46 ms), unmeasured against LS.

## Next
Beta order. Rows after the divider come after the beta or are blocked.
| ID | Task | Why / blocked on |
|---|---|---|
| P21 | **Remove the hard-edged fragments on real content (G50)**: a debug output colouring each pixel by the warp branch it took (A-only / B-only / re-fetch / cross-fade fallback / candidate index) on a recorded real pair; then continuous weights instead of switches and a spatially coherent candidate choice; recheck against LS with `yt_arms.py` at 1440p | the one measured reason we are below LS; root cause unknown |
| P22 | **A real-content measurement that sees sparse hard edges**: textured 1440p pairs with ground truth (high-fps footage decimated, or R2 Meridian) and a metric for edges present in the output and in neither source, in the `gate.py` matrix | until then P21 is judged by eye; frame means cannot see 0.x % of pixels |
| P23 | **Scene cuts**: detect a cut inside the pair and hold instead of interpolating across it | real footage cuts every few seconds; nothing handles it |
| P18 | **Learned flow once per pair, offline first**: SEA-RAFT S/M, NeuFlow v2, GMFlow, MEMFOF, RIFE@t=0.5 fields through `--offline-inject` (full-res A/B-anchored fields + visibility map); the winner goes to a TensorRT-RTX probe | A1 field 32.6 dB vs oracle 50.6. Env ready (S23); D29 lifts the weight gate; SEA-RAFT-M at 1080p fp32 does not fit 8 GB. Blocked on the harness taking A/B-anchored fields |
| P14 | **A real visibility signal for the warp** (forward splatting, or a source-anchored band swept by t) | G31; ceilings `only_a` 41.13, `only_b` 36.81; options in `kb/research/browser-quality-tools-2026-09.md#4` |
| P20 | **Sharper generated frames**: Catmull-Rom colour fetch in `PSWarp` instead of bilinear | at 24→165 six of seven shown frames are generated |
| P11 | **Video rect, cadence and DRM without an extension (D30)**: windowed rect from dirty regions or UI Automation (lifts D27); cadence stays video-frame test + timeline (I8); L3 DRM from outside the page (tab URL via UI Automation, Chrome's CDM utility process) — unresearched | beta gate: windowed video, protected video skipped (I7) |
| P6 | Game/GPU-load gate — beta cut: positive game signal first, the target's process tree excluded (N14) | I1, I2, D3 |
| P7 | Installer (Inno, per-user), autostart, tray settings window, signed with the owner's local certificate (D30) | beta gate; D14's Velopack updater can follow |
| P15 | Remaining live checks: captions, seek previews and controls over the video; windowed once P11 lands; the engage flicker by eye | S25 covered fullscreen playback only |
| P24 | **Beta hardening**: 30-min sessions at 24/30/60p, TDR / sleep / lock / monitor-change recovery, then the owner's blind A/B against LS on 1440p YouTube | the last gate item |
| — | *after the beta, or blocked* | |
| P5 | PotPlayer path (`NovaSilk.ax`) | after the beta (D30); validates D10 and A/V sync |
| P19 | NVIDIA VFX SDK Video Frame Generation as an arm | blocked on an NGC AI Enterprise entitlement (G46); licence itself allows use |
| P13 | Umbrella — match LSFG 3, then beat it (D28): now P21, P22, P23, P14, P18 | quality is the differentiator |
| P9 | S-M13 — is the NV12→BGRA8 switch at engage visible, can the engage sequence hide it? Deferred by D23 | G17 + D22 |
| P2 | Remaining spikes S-M3..S-M12 in D20's order | `directives-2026-09.md#6` |
| P3 | Toolchain: OF SDK headers, Vulkan SDK, vcpkg; CUDA/TensorRT if P18 wins | G3 |
| P8 | Harness calibration: M by defect injection, the G2 veto on, photographic corpus at 24p | every tier verdict is unfalsifiable without it |

## Knowledge base
Map, invariants, decisions, gotchas, negative knowledge and deep-dive files live in
`F:\Claude\Nova SilkPlay\` — start at `INDEX.md`. Both sides are maintained by the `roadmap` skill.
