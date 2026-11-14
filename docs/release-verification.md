# Release verification

**Verdict: not release-ready.** Version remains `0.9.0-dev`. No release cut,
signing or publication. System recovery (TDR/sleep/lock/monitor recovery) and DRM
were excluded by the owner from this verification.

## Executable and scope

- Fresh `/W4` native build: PASS, no compiler warnings.
- Initial verification executable SHA256: `3e7df4f6da8b7c3cc5feae7a4654b5f4d2f7e36980c440393cb4f01818bcd51a`.
- Current shared-D3D11/cadence/RGB-cut/synchronized-flip executable SHA256: `2c29bc06fbf0c316571d67e5c599d4352671173d720db9979776dc56409ca34c`.
- Preserved pre-sync RGB baseline SHA256: `b8527af120385bb07be03161859303daaea767e36b5593e978c5469521588814`.
- RTX 5060 Ti; primary 2560x1440 at 165 Hz, secondary 1920x1080 at 60 Hz.
- Disposable Chrome profiles/owned PIDs for fixtures; owner's explicitly
  authorized authenticated video tab for native YouTube playback. Synthetic CFR
  fixtures use `--scout-always`; production media cases do not.
- Initial evidence: `artifacts/release-check-20261111/`. Subsequent source fixes
  and diagnostics: `artifacts/release-fixes/`.

## Passed checks

| Check | Observed result | Evidence under the evidence root |
|---|---|---|
| Native geometry, settings, autostart | PASS; settings 13/13, other suites zero failures | `*_test.log` |
| Python verdict/playback and soak logic | 10 + 6 tests PASS | `python-yt-tests-fixed.log`, `python-soak-tests.log` |
| Tray/settings | Full live suite 44/44 PASS; earlier headless subset 15/15 | `tray-live.log`, `tray-headless.log` |
| Endpoint identity and deterministic replay | G0 bit-exact A/B; G1 two additional repeats, 21 synthesized frames each | `g0g1-fixed.log` |
| Secondary CFR24/30/60 transitions | PASS: attach, visible scroll, clipped refusal/return, theater, pause/play, fullscreen/return, clean exit | `windowed-fixed-{24,30,60}/windowed_check.json` |
| Browser WebGL control | Real WebGL context, automatic generation refused | `media-smoke/webgl.json` |
| Muted media | Automatic generation refused; explicit ON engages; media gate disengages 20.006 s after ON | `media-smoke-grace/muted.json`, `muted-engine.log` |
| Player controls | Captions/controls stay engaged; pause hides; seek/play returns; no decoded-frame drops | `media-smoke/video.json` |
| Settings appearance | English/Russian snapshots inspected: readable, no observed clipping, PotPlayer inactive | `settings-en.png`, `settings-ru.png` |
| Installer | Install, payload equality, GPL/notices, Startup shortcut, launch, upgrade with graceful shutdown, uninstall PASS | `installer-smoke/result.json` |
| User state preservation | Settings survive uninstall; original Startup shortcut and legacy Run value restored; original log directory preserved | `installer-smoke/result.json` |
| Real game gate | PoE world: pause at 49% per-process 3D load, device yield after 10 s, resume and visible output after switching away | `game-gate-world3/result.json` |

The real-game cycle passed with the owned secondary source kept visible. After
the gate cleared, output became visible 10.9 s later; functional resume is
confirmed, immediate return is not claimed. The game and owned test source were
closed normally. The primary monitor was returned; further tests stay secondary.

The first muted-control probe mistook an unrelated geometry hide for grace
expiry. The focused fullscreen rerun instead requires the explicit
`no media session is playing (never was) - disengaging` event: ON at 10126.006 ms,
disengage at 30132.461 ms. The original early-hide result is not timeout evidence.
Early game observers also included harness failures (case-sensitive process-name
matching and an initial-display timeout while the game gate was already paused);
the complete `game-gate-world3` cycle is the acceptance evidence.

The first CFR transition runs expected cropped playback to reattach after a
240 px scroll. That expectation contradicted G105. The corrected test uses a
48 px visible scroll and separately requires clipped playback to hide, stay
hidden for three seconds, and return when fully visible. The verdict tests reject
missing transitions and any clipped playback that reappears prematurely.

## Current failures and limitations

### Authenticated native1440p24/30 follow-up (G122)

CHARGE `UXqq0ZvbOnk`: actual1440p rendition2560x1072@24 (cinema aspect),
primary2560x1440/165Hz. Ordinary repeat:164.67/156.86/161.77 image changes/s,
zero recorder losses and continuous visible output; source-only24.36/s.
YouTube dropped counter1->1 (final7126decoded). Initial attempt is retained:
recorder losses2/1/5, then a Chrome status popup obscures output before control.
Do not use that attempt for performance acceptance. Host CPU median/peak during
valid repeat60.2/90.6%; do not attribute the lower count solely to host load.
Evidence:`authenticated-charge-primary-{1,2}/`.

NASA Moon Tour `nr5Pj6GQL2o`: actual2560x1440@30, verified through Stats for
nerds. Physical browser client2560x1440 is independently checked. Ordinary
three takes have recorder losses1/8/6, deferred4/1/1: all censored. Source-only
controls29.52/29.98 changes/s. Native logs include real output156-157/s spans,
so recorder loss does not excuse all observed lower rates. Initial ordinary
source check has zero dropped frames (0of18564). Later large quality recordings
end at5of36628; drops are not localized to an arm. A seek/resume without our
engine raises the counter5->7. No-DDA preflight stays7->7 (38942->39920decoded),
all three short visibility windows PASS. Native30 functional behavior is
observed; stable165 acceptance remains open. `authenticated-moon-primary-2/`,
`authenticated-moon-deferred-1/`.

Fresh LS owner profile: LSFG3 Adaptive165, flow100, scalingOff, WGC. Owner PID
29528 preserved; output activated/disabled by its configured hotkey. Config
SHA256 remains `0cb43d49bee579aefb1abd8895822bfbf8d41c7eeb342144f4ea1093da44c88b`.
First page/ours/LS quality takes cover adjacent moments. A wider LS take covers
all496ours frames coherently (matching correlation0.99987); three matched pairs
look close on inspected terrain, without an obvious named tear. Matching is
approximate; no held-frame classification or ground truth. Recorder losses
and unlocalized source drops prevent a quality/parity PASS. Scope is640x360ROI,
not whole1440p. `live-moon-quality-match/{matched.png,result.json}` and
`authenticated-moon-quality-*` preserve all attempts.

Instrument boundaries: connected browser UI search/click works when direct
navigation times out. DOM fullscreen alone can leave a small physical client;
F11 reaches the required full monitor. Always enforce actual native geometry.
Unicode `--target-title` fails because narrow ANSI argv is decoded as UTF8;
the production binary was not changed. Harness uses an explicit ASCII filter
only with the complete Unicode title, single visible Chrome HWND/PID and native
selected-HWND guards. Failed title-only attempt produced no accepted capture.

First90s ETW no-DDA attempt:`presentmon-runs/run-052d905aa66346d8/`.
Owner approved UAC; helper exits0, but the45s post-UAC source-setup deadline
expires before native startup. No CSV, native process or measurement windows;
not accepted. No collector remains. `attempt-result.json` preserves the failure.

Replacement:`presentmon-runs/run-42e34ad3f04749c3/`. Source setup/proof is now
before elevation; native starts automatically after ETW readiness, without a
model/UI roundtrip deadline. Owned test tab loops the video, so awaiting UAC
cannot end the source. Actual run requires fresh(<300s) playing-source proof.
Helper90s/three5s QPC windows, pinned2C29 copy, no DDA. Policy12checks and
three-window non-elevated preflight PASS; PS1 BOM retained. Owner approved a
fresh one-off UAC; automatic elevated run and actual analyzer exit0 (G123).
All three source/visibility/single-swapchain windows valid:164.997/164.998/
164.997 display events/s; maximum gaps6.212/6.328/6.152ms, zero non-displayed
submissions. Hardware Composed: Independent Flip throughout. Native1440p30
source counter1of7323 ->1of14278: no added source drops, including the UAC
interval. Source proof, measurements, capture-exit.json and display-analysis.json
are retained in the run folder. Display events do not prove distinct pixels.

Owner subsequently confirmed the production counter visible (~30/165) in a
bounded normal-privilege demo: `authenticated-counter-demo/`. Performance runs
explicitly use `--no-badge`; that omission is not inactivity proof. Demo exits0
at its180s timer; no production/settings change. Owned source tab closed,
Chrome restored maximized; no native test/collector remains. Next: A1 quality.

### Historical LS image diagnostic (G121)

Current 2C29 offline synthesis was evaluated at the best-fit content times of
119 historical non-held LS desktop images: A1 42, A3 41, A4 36. Every case
passes endpoint identity and three-run repeat identity. Source PNGs match the
current analytic generator; re-scoring every archived LS image reproduces its
stored PSNR. Per-scene mean current-minus-LS PSNR in dB:

| Scene | All | Moving | Static | Occluded |
|---|---:|---:|---:|---:|
| A1 | -0.94 | -1.86 | -4.12 | +6.49 |
| A3 | +7.53 | +16.98 | +3.72 | +4.51 |
| A4 | +12.48 | +13.15 | +2.94 | +3.42 |

Reviewed explicit worst-moving-stratum contact sheets: A1 current textured
patch deforms more than the archived LS image; A3/A4 differences also remain
localized, so neither means nor individual winners imply universal parity.
LS times were fitted to maximize LS's own image match; current engine is run
at those phases. This conservative diagnostic compares offline current output
with historical live LS; it does not measure current LS, cadence or live parity.
Evidence: `archive-ls-matched-2c29/{summary.json,result.json,worst-moving.png}`,
`compare_archive_ls.py`, `review_archive_ls.py`. Authenticated native24/30 and
live Moon LS follow-up is recorded above (G122/G123), with its validity limits.

### A1 source-only proposal diagnostic (G124)

An instrumented artifact copy exports actual NVOFA/processed fields for three
selected worst archive cases. G0/G1 and production PNG identity PASS. In A1's
eroded textured patch, only5.02% of local5x5 candidate pools contain a vector
within0.5px of truth; widening scoring alone cannot recover absent candidates.
Dense source-only search with a wider window finds the correct basin in57/57
sampled points. Ground truth scores the estimates; it never selects them.

A CPU-only proposal proof retains original cut/visibility state, replacing only
midpoint vectors when source matching improves. All3 no-override controls match
production; G0/G1 PASS. A1 moving improves8.40dB and looks cleaner, but static
loses3.29dB; static >40-code pixels6->5 of460842. A3/A4 selected strata are mixed.
Not promoted: selected cases, CPU-only generation, no GPU/live/cut validation.
Expanded CPU/GPU checks and real-content rejection follow below. Evidence:`archive-field-diagnostic/`,
`mid-proposal-cpu/{result.json,review.png,worst-static.png}`.

### GPU dense-proposal laboratory (G125, not promoted)

Source-centre matching guard limits the static trade. All18 selected GPU cases
pass G0/G1, repeated-field identity and no-override production controls. A1
moving improves3.81dB mean; static loses.21dB mean. The full seven-corpus matrix
finds A1 both-visible+3.86dB, A3/A4+.03, adversarial A5 unchanged. However BBB
loses.087dB overall: pair17 loses.960dB and pair9 adds.252 percentage points of
large-error pixels. Reviewed arm/eye crops show worse hard fragments. N54:
unregularized midpoint replacement after coherence is rejected as a release fix.

Complete-sequence field readbacks reproduce all62 BBB gate PNGs. In the two
selected crops, >8px field jumps increase258->343 and111->144. Extra coherence
after the proposal still worsens pair17 by1.37dB; smoothing does not resolve it.
R2/R3/R4 stay separate; no aggregate quality verdict or live acceptance.

RTX5060Ti offline BBB2560x1440, six alternating runs: new-stage p50.305/.375/
.200ms, p95.815/1.017/.819ms; full motion p50 controls6.797/6.854/6.624ms versus
candidate7.369/7.619/6.887ms. Shutdown-only D3D11 queue timestamps, no recorder.
Host CPU median51.9%, peak73.6%; not an idle-host or end-to-end latency result.
Evidence:`dense-quality-1/`, `dense-real-field-diagnostic/`,
`bench-dense-proposal/`, `dense-cohere-quality/`. Production remains2C29.

### Independent source-hypothesis follow-up (G126)

Full-RGB source hypotheses are refined before selection and checked against
independently estimated opposite-direction fields. Keeping them until reciprocal
selection avoids prematurely discarding useful vectors for a photometric alias.
CPU proof on3 selected ROIs: A1 improves3.58dB; BBB pair9-.010/pair17+.002dB.
Initial native GPU A1 case passes G0/G1, three-field repeat identity and original
production control: moving+4.31dB, static+.007. Full seven-corpus scoring is
complete (G127): A1 both-visible+2.95dB, A4+2.09; BBB-.008dB, large-error+.0011pp.
Worst BBB pair loses.128dB; baseline ear/eye defects remain in reviewed crops.
No promotion or broad quality acceptance. Seven-corpus repeat/reference gate PASS
(363 hashes per arm); 177 control PNGs byte-identical to production-equivalent
controls. Rigid-pan8–128px G0/G1 PASS without regression. All313 contextual cut
outcomes unchanged; G0/G1/dimensions PASS at953x537,1281x721,2560x1439.
G129 local owner-provided Moon30 resumed verification: four control/candidate
arms, all14 source-window drop deltas0, no hidden samples. Six DDA windows
all censored by recorder misses (control4/4/9, candidate1/1/2). Without DDA,
after15s warmup, both arms still have native-output dips (~156fps); no ETW
or165 distinct-pixel acceptance. CPU medians57.5–64.4%, peaks75.4–85.6%.
Three new exact-GT Moon clips (30fps source decimated to15fps,16pairs each)
pass G0/G1 both arms; joint gains+.0376/+.0225/+.0126dB on average, with
small aggregate large-error/spurious-edge reductions. Reviewed crops retain
shredded changing numerals in both arms. Not actual30fps GT or LS parity;
no promotion. Evidence:`artifacts/release-fixes/local-moon-quality/` and
`local-moon-live-summary.json`. Owned player/engine/server stopped, screen returned.

Six alternating RTX5060Ti BBB2560x1440 runs: new-stage p50 2.352/2.367/2.208ms,
p95 7.541/7.430/6.449ms; full motion control p50 6.896/6.271/6.463 versus
9.756/10.484/9.899ms. Host CPU median63.3%,peak94.7%; later without native
compilation36.7/67.4%. Offline queue intervals, no live165Hz/latency verdict.
Timing is a material risk; fast-pan/cut/live checks and quality work remain.
Evidence:`joint-source-cpu/`, `joint-source-pilot/`, `joint-quality-1/`.

### Flip synchronization diagnostic (G119)

One overlay-only change, `Present1(1, 0, ...)`, removes the measured dropped
presentations in three alternating ETW-only film controls: 164.999/164.996/
164.997 display events/s, zero non-displayed submissions, versus baseline
139.946/140.230/140.900 and 72 non-displayed submissions in each common
272-275 s source window. Both use Hardware Composed: Independent Flip. All
source/visibility checks pass; the corrected helper and PresentMon exit zero.
RTX 5060 Ti, primary 2560x1440 at 165 Hz; decoded film 862x360 browser-scaled.
Median CPU-start-to-display latency increases by ~6 ms to 17.64-17.73 ms;
end-to-end A/V delay is not measured by that metric.

Separate clean ordinary CFR image recordings: 24p 164.68/164.17/164.63 changes/s,
60p 164.16/165.00/165.13. CFR30 clean ordinary 164.64/164.58 and deferred
165.14/164.80; one repeat in each recorder mode is censored. All three deferred
CFR60 repeats are censored by 1-2 accumulated updates and retained. Secondary
CFR30 lifecycle PASS. Sync cut compatibility PASS: three complete loss-free
local windows per cut, page controls, no named distortion on reviewed images.
Title: `primary-sync-cuts-ordinary/r{0,1,2}` m0; vent:
`primary-sync-cuts/r{0,1}` m1 and `primary-sync-vent-extra/r0` m1. Retain all
censored attempts. Production port `/W4` build PASS; 144 GPU cases and seven
corpus G0/G1 PASS, all 363 intermediate hashes unchanged from B852. The only
additional shader edit renames a shadowed RGB histogram loop variable; runtime
HLSL compilation is warning-free. Fresh production valid ordinary CFR image
recordings: 24p 164.68/164.65/164.63; 30p 164.71/165.13/164.63; 60p
164.60/158.59/162.22 changes/s. Full P29 remains open. Lower 60p span has CPU
capture/Present maxima 12.17/13.45 ms, loop 159/s; source rVFC remains 60 with
zero dropped-frame delta. Host CPU alone is not established as the cause.
Anonymous native-1440p YouTube controls reset without SilkPlay on both direct
and local-SOCKS routes (G51). The owner's authenticated Chrome plays normally;
use that authorized test tab rather than copying authentication/profile data.
CFR decoded source is 960x400. First CFR60 ETW attempt loses output after
Chrome shrinks by one pixel; cases 1-5 remain invalid. Corrected fullscreen
ETW repeat exits zero with all six source/visibility cases valid and zero
non-displayed submissions. Five give 164.995-165.001 display events/s; the first
gives 163.386 with one 18.191 ms gap. DDA-on image controls give
165.037/165.075/165.012 changes/s, no recorder losses. This removes persistent
60p queue discard as an explanation, without claiming absence of all hiccups.

Authenticated Chrome native 2560x1440@60 BBB playback on the primary is verified
through YouTube Stats for nerds. Current 2C29 defaults engage automatically
after Chrome's media session appears. Two ordinary two-second image captures
give 164.67/161.19 changes/s, no recorder losses, continuous overlay visibility;
source-only gives 59.85. A third capture misses one update and is censored.
YouTube reports zero dropped frames throughout (0 of 48958 at the final check).
No CDP/profile copy, game/DRM bypass or scout override. This is a short native
1440p60 functional/cadence check, not full 24/30/60 or broad quality acceptance.
The app exits through QuitEvent; original mute/1080p60 settings are restored,
Stats closed, owner tab left open and primary returned.
Evidence:
`artifacts/release-fixes/presentmon-runs/run-0c807ac2ccea48f6/`,
`presentmon-runs/run-ee17548c9f84486e/display-analysis.json`,
`authenticated-bbb-primary-2/result.json`,
`primary-plain-{ordinary-sync1,deferred-sync0}-{24,30,60}/`.

### Independent display-event diagnostic (G118)

PresentMon 2.6.0 targeted a uniquely named B852 test copy under one owner-approved
90-second elevated capture; Chrome/app stayed unelevated. Three DDA-off cases
show 140.952/140.852/140.852 display events/s versus DDA-on
140.950/140.850/140.952 in the same automatically intersected 273.25-274.75 s
moving-film interval. One Hardware Composed: Independent Flip swapchain/layer;
35-36 submitted frames per interval are not displayed. Source drops/hidden
samples zero; three DDA intervals have no accumulated loss, pixel changes agree.
This supports real presentation loss, rather than a DDA-only observer effect.
These are RTX 5060 Ti / 2560x1440 165 Hz display measurements; decoded film is
862x360, browser-scaled. ETW events alone do not prove pixel uniqueness.

Capture caveat: PresentMon reports Started/Stopped recording, stderr whitespace,
but the helper loses ExitCode after exit. No success code is invented. Retaining
the process handle passes controlled Windows PowerShell 5.1 exit 0/37 checks;
a subsequent ETW capture must verify it. Evidence:
`artifacts/release-fixes/presentmon-runs/run-0e6dbe9c5ed54524/`.

### G107: persistent adjacent split unions now refused

The original RectFinder replay accepted a 960x572 union of a 960x540 video and
an adjacent 960x32 animation. Preserve raw damage and require repeated witnesses
crossing each stable internal split. Vertical/horizontal strips, changing strip
subdivision and one union repaint are refused; split-video cases still pass.
Full rectfind suite PASS. Arbitrary already-merged live unions remain unverified.
Evidence: `../artifacts/release-fixes/rectfind-after.log`, `rectfind-centred.log`.

Startup staging moved to the parent of Startup. Isolated real ShellLink tests
PASS and assert that the temporary file is outside Startup. Evidence:
`../artifacts/release-fixes/autostart-final.log`.

### 165 Hz motion is not accepted

Three repetitions per source rate used the established 2560x16 DDA strip. The
clean CFR30 recording measured **134.72 distinct changes/s**, CFR60 **112.09**,
with no extra Chrome drops. All three CFR24 recordings missed recorder updates;
exclude their numerical results. Other repeats also had recorder misses and are
excluded. DDA records desktop changes, not physical scan-out. Submission-rate
counters do not establish smoothness. Evidence: `primary-ours-{24,30,60}/`.

### Scene-cut fallback partially repaired

The matcher minimizes its own midpoint residual, so a small residual can falsely
explain unrelated images. A fallback-only guard now requires an abrupt pair and
strong global plus spatial composition changes. Both blocks and NVOFA hold all
four Spring cuts exactly, with no false holds among 120 pairs. Analytic and
R2/R3/R4 metrics remain unchanged to displayed precision. Evidence:
`../artifacts/release-fixes/block-cuts-final.log`, `ofa-cuts-final.log`,
`matrix-first.log`.

The wider F1 replay contains 128 high-change candidates, not 128 independently
verified hard cuts. The anchored experiment holds 122 but misses the visually
confirmed cut at 1194; it is not promoted. The fallback is not broadly accepted
from the four-cut result alone. Evidence:
`../artifacts/release-fixes/cut-contexts/result.json`,
`cut-contexts-anchor/result.json`, `missed-anchor-cuts.png`.

### LS cadence comparison is censored

The fresh LS CFR30 source became paused during recording, advanced only about
1.22 s, and the recorder missed one update; the next seek failed. Exclude the
sample and do not rank it against Nova. The output stopped when the owned
browser closed. Read-only profile metadata: LSFG3 Adaptive, target 165, flow
scale 100, WGC. Evidence: `primary-ls-30.log`.

The unmuted real-film secondary control then completed three source-valid
moments, no additional Chrome drops. All full-crop recordings missed DDA updates
(183/190/116); a subsequent 1920x16 strip still missed updates (46/50/47).
Every `cadence_valid` is false: shrinking the copy did not establish clean LS
cadence evidence. Do not rank those counts. Sources:
`ls-film-secondary2/result.json`, `ls-film-secondary-strip/result.json`.
LS was stopped with its existing hotkey; the owner's original process/profile
was kept, and no LS output window remained.

`blind-ab.webm` is a 4.5 s, 60 fps, 270-frame image-quality review of the title and
vent transitions. The owner reported **no visible difference** in the blind
review. The recorded key identifies **A = Nova SilkPlay, B = Lossless Scaling**.
This finding applies to these two recorded cuts only; broader parity and 165 Hz
acceptance remain open. Both arms use the same 640x280 crop of their central 640x360
recordings; source cuts are aligned. The original MPEG-4 Part 2 MP4 decoded in
OpenCV but Chrome rejected it with `DEMUXER_ERROR_NO_SUPPORTED_STREAMS`.
The VP9 WebM was then verified playing and advancing in the existing Chrome
viewer, without moving the window the owner placed on the primary. This
resampled, compressed review cannot validate cadence, original-frame pixel
identity or 165 Hz smoothness. Broader LS parity remains open.

### YouTube fixed-header geometry and centred source-format change

Fresh profiles require cookie-consent handling and a progressing media clock /
decoded count; paused=false alone previously gave a false source verdict.

Hit testing proves that 48 CSS px scrolling places the picture at y=19.80 under
a masthead ending at y=55.99: ~41 physical px of upper occlusion at DPR 1.1.
Refuse that clipped picture instead of relaxing the geometry test. The finder
retains confirmed proportions for the same browser PID/window/title and can
relearn a centred source-format change.

Focused secondary YouTube PASS: 8 CSS px visible scroll (8.18 actual), 48 px
masthead clipping refused for 3 s, intact return, theater, pause/play, clean exit
and unchanged foreground. Explicit scope=windowed; fullscreen and native-1440p
quality are not covered by this focused rerun. The centred-format case passes
against measured changed-pixel bounds with the existing 2 px tolerance
(edge errors -1/0/+1/+2 px). Its DOM box differed by 2.41 px; that box is not
the visible-pixel oracle. Clipped refusal was verified with an opaque header;
the semi-transparent header case is not accepted. Evidence:
`../artifacts/release-fixes/youtube-clip-truth/dom-2.json`,
`youtube-geometry-fixed/windowed_check.json`, `format-geometry/result.json`.

## Quality matrix

Four analytic scenes completed (A5 informational), plus 31 withheld pairs each
from R2 Meridian, R3 BBB and R4 Spring. With `mc:ofa+hints:bidir+cand`, the real
corpora measured 44.68 / 25.34 / 32.43 dB respectively. Against `cuts-exact`,
real-clip values are unchanged to the displayed precision except a 0.0001
percentage-point reduction of Spring large-error pixels. Only A1 exists in that
baseline's analytic section; its rounded differences are within 0.01 dB. Do not
claim a historical comparison for the other analytic scenes.

This is a completed measurement matrix, not an LS-parity verdict. Evidence:
`quality-gate.log` and `harness/runs/gate/release-check-20261111/gate.json`.

Secondary Tears of Steel: two clean six-second recordings measured 59.22 and
59.92 distinct changes/s, zero additional Chrome drops. The title recording
missed one update and is excluded from cadence ranking. Inspected title/vent
contact frames show neither named primary artifact at this secondary geometry;
this does not validate 1440p165. Evidence: `film-secondary/`, `cut-title.png`,
`cut-vent.png`.

## Packaging gate

The fresh development installer was compiled unsigned and its production script
was exercised in an isolated installation directory. The post-cleanup signing
certificate exists, has its private key and expires in September 2031. Actual
RFC3161 signing remains unverified and conditional on release readiness (D40).
The development version was deliberately retained.

Fresh 2C29 unsigned installer smoke PASS, all 12 checks: payload identity,
notices, Startup creation, installed launch, graceful upgrade stop, removal and
owner settings/Startup/legacy Run restoration. No owner installation was
replaced. Evidence: `installer-sync-smoke/result.json`.

Current 2C29 CFR60 secondary resource soak was deliberately stopped after
20.12 minutes to free the single engine for authenticated Chrome verification;
exit zero, operator stop recorded. This is partial evidence, not a 30-minute
PASS or actual 60-to-60 frame-generation coverage. A fresh 24-to-60 secondary
30-minute generation/resource soak PASS:30.084minutes,178samples/150settled,
743active stats (450required),33overlay shows, native exit0, no log errors.
Settled working set96.61->99.65MB, private152.50->157.34MB, handles661->649,
GDI13 throughout; measured trends remain within the rig's leak thresholds.
This checks resource longevity with scroll/re-engagement, not continuous visual
absence of artifacts or excluded system recovery. Evidence:
`soak-sync-fg24-production/24/soak/20261113-003304_report.json`.


## GPU motion candidate

Buffered phase/DXGI traces and shutdown-only GPU query reads isolate motion
preparation as the 165 Hz bottleneck on RTX 5060 Ti: median/p95 5.055/5.386 ms.
Without coherence: 1.078/1.352 ms and three clean CFR30 captures at
164.49–164.78 desktop changes/s. Hints-off stays ~5.021 ms / ~135 changes/s.
No-coherence is diagnostic only, not a quality fix or physical scan-out proof.

Skipping adjacent equal candidates preserves the first tie winner. The actual
production shaders pass 144 deterministic GPU cases. All-field optimization
reduces motion to ~3.14 ms but breaks G1; blocking diagnostics hide the failure.
That scope and one-wave-per-cell parallel evaluation are rejected. Input luma /
hints match while converted forward flow varies upstream of coherence; the
underlying cause is unproved. No blocking workaround was retained.

The clean candidate restricts skipping to visibility fields. All 363 first-pass
intermediate hashes across A1/A3/A4/A5 and R2/R3/R4 match the reference path,
with G0 endpoints and G1 passing three repeats on both paths. This establishes
image identity on those cases. Its preserved executable is `candidate-side.exe`,
SHA256 `075aa30db3e8ea1537bed120f36b80bd8240f79f2bf639e1dd9c87f78cd0ed4a`.
Primary clean CFR30 reached 164.68 recorded changes/s, clean CFR24 161.68;
CFR60 recordings had recorder misses and cannot rank cadence. Motion median/p95
was 3.75/4.69 ms at CFR30. Full 165 Hz acceptance remains open. Primary title
letters and vent edges are visibly distorted in the new captures: P23 FAIL
despite zero Chrome drops. Recorder misses prevent absence-based acceptance,
but do not erase observed defects. The primary was returned. Evidence:
`../artifacts/release-fixes/cohere-test-final.log`,
`cohere-identity-final/result.json`, `primary-side-*`,
`primary-side-film/cuts-m0.png`, `primary-side-film/cuts-m1.png`.

The same candidate passed a 30.08 minute live secondary CFR60 resource soak:
752 active samples (450 required), exit 0, no monotonic working-set/handle leak.
Working set 87.2 -> 96.13 MB (max 97.85), private bytes 155.39 -> 159.58 MB
(max 166.52), handles 634 -> 621 (max 638), GDI 13 throughout. This is resource
stability evidence, not recovery/fault coverage. Evidence:
`soak-candidate/60/soak/20261112-024534_report.json`.

A further visibility specialization is bit-exact in the isolated shader suite
but fails full-pipeline G1 under D3D11. Retained repeat readbacks show RGB
deviations up to 207 levels; this is not rounding noise. Explicit zero hint
initialization and hint-state cleanup pass six hardware lifetime cycles, but
do not settle G1. This was the private-allocation control; shared allocations
below supersede it. Its historical executable SHA256 is
`a1db8ca062295efdd660a4a273621adf0c8223a87138342585a647495e9e999e`;
build and native geometry/settings checks PASS.

### Explicit-fence D3D12 diagnostic

The separate NVOFA backend shares the same render textures with D3D11 and uses
producer/consumer GPU fence points. Six hardware lifetime cycles PASS; an
8 px translation produces 8/0 px median flow and 6720/6720 central vectors
within 1 px. Quiet repeated-image diagnostics pass all nine combinations of
A1/A3/R3 and reference/all-skip/visibility-specialized shaders. A seven-corpus
G0/G1 run matches all 363 intermediate hashes between reference and visibility
specialization. No readback or CPU wait was inserted into the live pair path.

The API switch changes images. Against the original release matrix, real-clip
PSNR changes are Meridian +0.03 dB, BBB +2.37 dB, Spring +0.17 dB. Analytic
strata are mixed: A1 only_a -0.42 dB, A3 only_a -0.11 dB, A4 only_b -0.26 dB.
Three current D3D11 analytic control runs do not explain these losses away:
A1 only_a 23.79/23.79/23.81 dB versus D3D12 23.37 dB. This is not a blanket
quality PASS or an accepted production migration. Evidence:
`repeat-quant-explicit-fences-quiet/result.json`,
`cohere-identity-final-d3d12/result.json`, `quality-d3d12.log`,
`quality-d11-control-{1,2,3}.log`.

The secondary title capture has zero recorder/Chrome drops and looks clean;
the secondary vent capture has recorder misses. Both new primary D3D12 title
and vent recordings have zero Chrome/recorder drops, and inspected cut frames
show neither named artifact. This is one pass per cut under concurrent CPU
load, not full P23 acceptance. Host sampling records CPU mean 57.7%, peak
99.2%, physical memory minimum 3531 MB free. Evidence:
`secondary-d3d12-film-visible/`, `primary-d3d12-film-loaded/host-load.json`,
`primary-d3d12-film-loaded/cuts-m{0,1}.png`.

The owner reported high CPU load during primary cadence checks. Exclude the
new D3D12 CFR24 run from performance acceptance; CFR30/60 were not run. Offline
API timing comparisons lack an idle-host control and are diagnostic only.
All owned primary windows/engines are closed; the monitor is returned. Repeat
performance acceptance on an idle host, recording CPU/memory alongside it.

### Recorder diagnostic

`ddacap --defer-readback` preallocates narrow-crop staging textures and maps/
hashes after the capture loop. Four secondary CFR30 controls verify raw/CSV
frame counts and sampled pixel hashes. Ordinary captures miss 0/0 updates,
deferred captures miss 2/1: the option does not establish loss-free capture.
Loaded primary deferred samples also remain censored. The logical crop limit
rejects oversized allocations with exit 3; mapping/writing failures now return
a nonzero exit instead of silently validating a partial recording. Default
capture behavior is retained. Controlled LS capture remains unverified.
Evidence: `dda-deferred-check/result.json`, `dda-deferred-limit.log`.

### Shared D3D11 production candidate

Adding `SHARED_NTHANDLE|SHARED` to the NVOFA texture allocation is sufficient;
no production D3D12 migration. The same synthesis object in alternating
private/shared A/B fails 9/9 private cases and passes 9/9 shared cases. Shared
output is invariant with/without offline flow synchronization and matches the
D3D12 diagnostic. Actual production seven-corpus G0/G1 passes all 363 hashes,
the real shader suite passes 144 cases, and six hardware lifetime cycles pass
zero-hint/state cleanup and bidirectional 8 px translation checks. The exact
driver mechanism is unproved. G111's analytic quality losses still need review.

Saved PNG review confirms the local trade rather than dismissing it as rounding:
worst A1 only-A pair0/t0.5 loses2.54dB, >40-level error pixels2.68->6.26% of
that1119-pixel mask. A3 only-A and A4 only-B worst losses0.83/1.59dB. Every
saved PSNR is independently reproduced from the selected PNG and exact
(interval,phase) truth/mask. This compares one historical private-allocation
control with the shared RGB lab output; it is not a stable old-path baseline
or an LS-parity verdict. `analytic-trade-review-validated/{result.json,worst-strata.png}`.

Shared old-estimator baseline SHA256 is
`65c30dd44f923f5293cf1b039e5ff1b5da6fadaff84c6a66b26558813733ef51`.
Primary three-repeat CFR24 counts are 164.08/160.00/164.12 (last has one missed
update), CFR30 163.43/163.44/161.37, CFR60 162.45/146.66/158.42 changes/s.
Median/p95 GPU motion: 3.587/4.655, 3.547/4.190, 3.505/4.307 ms respectively
on RTX 5060 Ti. Host CPU means 22.1-38.6%, maxima 26.9-60.2%. These counts
do not establish full 165 Hz or physical scan-out acceptance. Six title/vent
recordings have zero Chrome drops and inspected first-eight postcut frames
look clean; two clips have one recorder miss each. Page control failed
fullscreen; full cut-window/control acceptance remains open. Evidence:
`shared-ab/result.json`, `nosync-ab/result.json`,
`cohere-identity-final-shared-production/result.json`, `primary-shared-*`.

### Estimator and acceptance corrections

At 60 fps/165 Hz, legitimate ~12/24 ms compensation intervals defeated the old
median-only upper trim, estimating 66-82 fps. The new pure period helper scales
the upper trim by a wider mean while preserving the lower trim. All 1051 native
checks pass; recorded replay leaves CFR24/30 unchanged and reduces mean absolute
fps error 10.13 -> 1.99 on the worst CFR60 trace. Current build is listed above.
Live improvement remains unverified: traced `primary-cadence-*` A/B recordings
are censored by recorder misses and/or owner Tk window occlusion.

The old rig accepted historical `overlay shown` log text after Nova correctly
hid for an overlapping window. Those source-only ~24 fps samples are invalid.
New no-trace rig observes real owned-overlay visibility throughout recording,
source clock/decoded/drop counters and recorder loss; six verdict tests pass.
It records steady fixture motion around 1.55-3.55 s. The older moving fixture
legitimately becomes blank near 4.03-4.60 and 8.52-9.10 s; recovery from that
interval needs separate coverage. Complete no-trace captures now exist, but
three-repeat full-165 acceptance remains open. On current RGB build, valid
CFR30 counts are 165.16/164.13 (ordinary) and 165.20/165.17 (deferred), CFR60
158.74/162.79 respectively. Other recordings remain recorder-censored. All
source-drop deltas and observed hidden counts are zero.
Evidence: `cadence-test.log`, `replay_cadence.py`, `cadence-verdict-tests.log`,
`primary_plain.py`, `capture_watch.py`, `cfr-band-probe.json`.

### Current cut acceptance

The RGB route adds only the two confirmed F1 NVOFA holds (1194/4827) in all
313 selected contexts; actual production and lab hold sets agree. Seven-corpus
G0/G1 and 363 intermediate hashes PASS; actual block Spring holds all four
cuts with no false holds among 120 pairs. Median motion adds .133 ms in the
six alternating BBB 1440p GPU-queue controls; this fixes quality, not speed.

Targeted primary title/vent image defect gate: PASS. Three complete +/-250 ms
windows per cut, loss-free inside that window, zero source drops, observed
overlay visible throughout, and matching page controls. Reviewed six precut /
eight postcut screen images are clean. Accepted title clips: narrow2 r0/r1/r2 m0;
vent: narrow2 r1/r2 m1 and extra r0 m1. Keep the two censored vent attempts:
narrow2 r0 miss -91.056 ms; extra m0 miss +229.226 ms. The 640x120 crop targets
the original defects; earlier shared 640x360 clips provide the wider view.
Full 165 Hz and LS parity remain separate gates. Evidence:
`primary-rgb-film-narrow2/`, `primary-rgb-vent-extra/`,
`cut-cover-rgb-production/result.json`, `spring-rgb-production.log`.

Initial DDA backlog is now reported separately from loss between first/last
captured image; raw CSV metadata is retained. Three boundary-accounting tests
PASS. Experimental early capture polling passes secondary lifecycle checks,
but the real motion window still gives ~144 versus ~141 baseline changes/s,
so it is not promoted. Default executable remains the RGB build listed above.
