# Nova SilkPlay — project state

Real-time video frame generation (24/30 fps → up to the display refresh, 165 Hz) for browsers and players on NVIDIA RTX 50. Research and architecture done, the measurement spikes are running, and **the browser path exists end to end and generates frames** (`prototype/`).

## Status
| ID | Component | State | Evidence |
|---|---|---|---|
| S1 | Technology research, 14 dimensions, adversarially verified | ok | `kb/research-findings.md`, `kb/research/*.md` |
| S2 | Dev environment | ok | `kb/dev-environment.md` |
| S3 | Lossless Scaling reference analysis | ok | `kb/lossless-scaling.md` |
| S4 | Architecture decision | ok | `kb/architecture.md` — 4 designs, 5 judges, 3 red-teams |
| S6 | Delivery plan to v0.9 | ok | `kb/plan-v09.md` |
| S7 | Owner directives D-A/D-B/D-C researched | ok | `kb/research/directives-2026-09.md`; corrected N3/N11 |
| S8 | GPU/OS capability probe built and run on the dev GPU | ok | `tools/caps-probe/`, `kb/dev-environment.md` |
| S9 | Five gaps closed; NVOFA **measured** on GB206 | ok | `kb/research/gaps-closed-2026-09.md`, `tools/ofa-probe/` |
| S10 | S-M2 harness: overlay+WGC probe, Chrome-side CDP driver | ok | `tools/overlay-probe/README.md` |
| S11 | Owner directive D-D — imperceptibility is a release gate, tray settings | ok | I12, I13, D22 |
| S12 | **Browser path end to end: WGC → GPU ring → motion per pair → warp per tick → DComp overlay** | ok | `prototype/README.md` |
| S13 | **Quality harness**: `--offline` replay (G0/G1 pass), per-frame scoring, `--strata`, true occlusion maps | ok | `kb/quality-harness.md` |
| S14 | **P13-a bidirectional evidence** `--occ`: +1.95 dB off the band, nothing in it (G31) | ok (measured, mixed) | G31 |
| S15 | **Oracle-flow reference** (`--offline-inject`, arm `oracle:<motion>`), G0 bit-exact | ok | `kb/quality-harness.md` |
| S16 | **Anti-overfit scenes + `gate.py`**, **`panprobe.py`** (caught N23), **`costscape.py`** | ok | `kb/quality-harness.md` |
| S17 | **G30 fixed on both motion paths**: refine-before-score (A4 18.25 → 43.74 vs 44.91 oracle), NVOFA external hints ON (18.90 → 38.34) | ok (measured) | G30, N23, G32 |
| S18 | **Click-through overlay, one z-slot above the browser, occlusion pause with widgets exempt, fullscreen-only attach** (D25-D27) | ok (17/17 + 8/8 live, secondary) | G35, G36, G38 |
| S19 | **Video-frame test: player-UI recompositions no longer advance the pair** (adversarially reviewed) | ok (test-page `<video>`), YouTube unverified | G37, G40 |
| S20 | **Competitor arm vs Lossless Scaling: byte-exact display, recorded and scored per stratum** — per generated frame we win A3 (+3 dB) and text-over-motion A4 (+5.4), lose A1 moving (-2.7) | ok (measured, 60 Hz) | `kb/quality-harness.md`, G41-G43, D28 |
| S21 | **P17 pacing: regularised content timeline + jitter margin + older pair** — held frames 52→30 % (A3), 48→26 % (A4), LS 32/23; A1 still 51 % | ok (measured, 60 Hz) | `kb/quality-harness.md` |
| S22 | **Tool survey for browser quality**: learned optical flow, once-per-pair visibility, gate tools, NVIDIA video FG — licences and weight provenance verified; NVIDIA VFX SDK 1.3.0 Video Frame Generation found (continuous t) | ok (research, nothing measured) | `kb/research/browser-quality-tools-2026-09.md`, G45, N24 |
| S23 | **Learned-flow evaluation environment**: CUDA PyTorch venv, five flow repos, seven checkpoint sets; SEA-RAFT loads and is correct to 0.035 px on a synthetic pair | ok (verified) | `kb/dev-environment.md#learned-flow-evaluation-environment-s23-p18` |
| S24 | **Score nondeterminism localised**: the hint-seeded NVOFA arm only, two discrete outcomes on the masked columns; every proposed CPU-side sync makes it worse | ok (measured) | G49, N25 |
| S5 | Remaining spikes S-M3..S-M12 | planned | `directives-2026-09.md#6` |

The KB index at the end is the map. Re-run `tools/caps-probe` and `tools/ofa-probe/*.py` after any driver bump; `kb/dev-environment.md` holds what this machine reports, not what docs claim.

## Now
**The prototype runs** (`prototype/README.md` has the detail and the accepted defects): one device for capture, synthesis and presentation, motion once per pair, a symmetric warp with per-pixel candidates, on a DComp overlay sized exactly to the video rect. 23.57 → 165.0 fps on the primary, 60.0 on the 60 Hz secondary (I14), our swapchain on a hardware `OVERLAY` plane. NVOFA is still the default motion source, but only just — see the comparison in `kb/quality-harness.md`.

**Quality (detail in `kb/quality-harness.md`).** The oracle-flow reference (S15) showed motion estimation, not synthesis, dominates the error on every scene (8-26 dB), except the occlusion band, where even a perfect field leaves 18.3 dB (G31: cA ~ cB there, so the weight collapses to `w = t`; the fix is a real visibility signal, P14). G30 is fixed on both motion paths (S17, N23 holds the five failed attempts).

**Where we stand against Lossless Scaling (S20).** Measured on byte-exact analytic clips at 60 Hz: per generated frame we are ahead on two of three scenes and behind on A1's moving interior; both engines fail the occlusion band alike; pacing is at LS's level since S21. Windows and the pointer no longer break generation (S18: click-through overlay, z-slot, occlusion pause; S19: UI recompositions do not advance the pair). Next is A1's motion estimation (engine field 32.6 dB vs oracle 50.6 vs cross-fade 34.1) — the paused hand-built experiments and the learned-flow arms of P18 through the same harness — then P14 with a full-resolution field (public-source ranking in `kb/quality-harness.md`); 165 Hz and real YouTube are unverified (P15).

**Driver 616.92 changes nothing** (`kb/dev-environment.md`): `caps-probe` is byte-identical to 616.56 and every NVOFA probe agrees with its old record. **The engine is deterministic** — seven serial gate runs and three concurrent ones reproduce the baseline, and `--offline-repeat` passes G1 bit-exactly — **except on the hint-seeded arm** (G49), whose masked columns have a second, better outcome that appears under heavy GPU contention. Our hint-buffer write is not ordered against the OFA's read and the D3D11 NVOFA API has no primitive that can order it; every CPU-side wait tried makes it worse (N25).

## Next
| ID | Task | Why / blocked on |
|---|---|---|
| P11 | **Auto target + the real video rect and cadence from the page** (extension or CDP) — lifts D27's fullscreen-only rule; `rVFC` frame events would also replace G37's content test | I8, D12, P6 |
| P15 | **Verify S18/S19 on real YouTube, fullscreen, on the 165 Hz primary with NVOFA** — the classifier's band against real controls, captions and seek previews; the content-test readback under 1440p load; the activation hitch by eye | everything so far ran on the 60 Hz secondary with the test page |
| P18 | **Learned-flow arms, offline**: SEA-RAFT S/M, NeuFlow v2, GMFlow, MEMFOF (ceiling) and RIFE@t=0.5 fields through `--offline-inject`, extended to full-resolution A/B-anchored fields plus a visibility map; judged by the `gate.py` matrix + R2; only the winner goes to a TensorRT-RTX probe | owner ask: really better than LS in the browser, modern tools welcome. **Environment is in place and verified (S23)** and D29 lifted the weight-licence gate, so the choice is quality and cost alone. The nondeterminism is no longer a blocker but a rule: it belongs to the hint-seeded NVOFA arm alone (G49), so NVOFA arms are run serially and repeated and both outcomes are reported, while every learned arm is unaffected. Blocked only on the harness taking full-resolution A/B-anchored fields plus a visibility map. First finding: SEA-RAFT-M at full 1080p fp32 does not fit in 8 GB |
| P19 | **NVIDIA VFX SDK 1.3.0 Video Frame Generation as an arm**: cost per mode at 1080p/1440p on the 5060 Ti, quality in the harness, licence and redistribution | **blocked on an NGC entitlement (G46)**. The network 403 is lifted for the browser by the owner's VPN, the NVIDIA account is signed in and in the Developer Program, but the SDK Core and `nvvfxvideoframegeneration` still answer "Subscribe to get access — NVIDIA AI Enterprise": Download disabled, File Browser empty. Owner action: an AI Enterprise entitlement, or the SDK from elsewhere. Note VFG is licensed for commercial/non-commercial use (Open Model License), so only access is in doubt. The appeal (continuous t, shot-change detection) and the risk (a network per generated frame, N5) are unchanged |
| P14 | **Give the warp a real visibility signal** — cA/cB carry none in the band (G31) and the vector is already right; build a visibility map once per pair (forward splatting, or a source-anchored band swept by t). Ceilings `only_a` 41.13, `only_b` 36.81 | owner priority; NVOFA per-vector cost still unused; ranked options (coverage map, flow uncertainty, depth tie-break, learned occlusion head): `kb/research/browser-quality-tools-2026-09.md#4` |
| P20 | **Sharper generated frames**: Catmull-Rom colour fetch in `PSWarp` instead of bilinear, judged on the oracle arm + R2 | at 24→165 six of seven shown frames are generated, and a bilinear sub-pixel fetch softens each one |
| P13 | **Owner priority: match LSFG 3, then beat it** (D28: never its code). (a) → P14; (b) NVOFA done; (c) static-UI masking; (d) competitor arm done (S20); public-source ranking: visibility masks, full-res field, cut detection, overlay guard | quality is the differentiator |
| P9 | S-M13 — is the NV12→BGRA8 switch visible, can the engage sequence hide it? **Deferred by D23**, owed before release | G17 + D22 |
| P2 | Remaining spikes S-M3..S-M12 in D20's order (S-M2 done for composition mode, not latency) | four of five blocking conflicts need one measurement each (`directives-2026-09.md#6`) |
| P3 | Install toolchain (OF SDK headers, Vulkan SDK, vcpkg; CUDA/TensorRT only if a neural gate opens) | G3 |
| P5 | Prototype the PotPlayer filter path | validates D10 and A/V sync |
| P6 | Game/GPU-load gate and auto target detection | D3, I1, I2 |
| P7 | Installer, signing, engine/app versioning | D6, D7, I5 |
| P8 | Quality harness — gates pass (S13), oracle arm and scene matrix exist (S15, S16). Owed: calibrate M by defect injection, turn the G2 veto on, photographic corpus at 24p | D19's ladder and every tier verdict are unfalsifiable without it |


## Knowledge base
Map, invariants, decisions, gotchas, negative knowledge and deep-dive files live in
`F:\Claude\Nova SilkPlay\` — start at `INDEX.md`. Both sides are maintained by the `roadmap` skill.
