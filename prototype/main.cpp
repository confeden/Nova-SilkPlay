// main.cpp — Nova SilkPlay browser-path prototype (ROADMAP P4).
//
// What it does: captures the target window with Windows.Graphics.Capture, keeps
// the last two source frames on the GPU, and presents a synthesized frame on a
// DirectComposition overlay that sits exactly over the video rect, once per
// compositor tick. At 165 Hz over a 24 fps source that is ~6.9 generated frames
// per source frame, all from a continuous phase t (I4).
//
// What it deliberately is NOT yet:
//   * driven by browser telemetry. The video rect comes from the command line
//     (default: the whole client area, which is exactly right for fullscreen
//     video), and the source cadence is inferred from capture arrivals rather
//     than read from the page (I8 says the product must read it from the page).
//   * flicker-free at engage. Starting a WGC session moves Chrome off its
//     hardware overlay plane and latches BGRA8 (G17), which can flicker once.
//     That is a known, accepted defect at prototype stage — see D23.

#include "nsp_capture.h"
#include "nsp_cadence.h"
#include "nsp_mediawatch.h"
#include "nsp_rectfind.h"
#include "nsp_autostart.h"
#include "nsp_common.h"
#include "nsp_drm.h"
#include "nsp_gamegate.h"
#include "nsp_dump.h"
#include "nsp_image.h"
#include "nsp_offline.h"
#include "nsp_overlay.h"
#include "nsp_settings.h"
#include "nsp_settings_window.h"
#include "nsp_tray.h"
#include "nsp_synth.h"
#include "nsp_ui_text.h"
#include "nsp_version.h"
#include "nsp_winwatch.h"

#include <dwmapi.h>
#include <dxgi.h>      // the render adapter's LUID, for the game gate
#include <shellapi.h>  // CommandLineToArgvW

#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nsp {
namespace {

// ------------------------------------------------------------------- options

enum class Mode {
    Passthrough,  // bit-exact copy of the newest source frame (alignment check)
    Blend,        // linear-light cross-fade at the continuous phase t
    Mc,           // motion-compensated interpolation
};

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Passthrough: return "passthrough";
        case Mode::Blend: return "blend";
        case Mode::Mc: return "mc";
    }
    return "?";
}

Mode NextMode(Mode m) {
    switch (m) {
        case Mode::Mc: return Mode::Blend;
        case Mode::Blend: return Mode::Passthrough;
        default: return Mode::Mc;
    }
}

struct AppOptions {
    std::wstring targetClass = L"Chrome_WidgetWin_1";
    std::wstring targetTitle;
    std::wstring targetExe = L"chrome.exe";
    DWORD targetPid = 0;
    // Any --target-* flag is a developer override: while one is given, the Settings window's
    // choice of browser does not decide what is captured.
    bool targetFromCli = false;

    bool haveRect = false;
    RECT rectClient{};  // video rect relative to the target's CLIENT origin
    bool rectIsScreen = false;

    Mode   mode = Mode::Mc;
    double minUpdateMs = 1.0;
    bool   borderless = true;
    double statsEverySec = 3.0;
    double stallMs = 400.0;   // no new source frame for this long -> hide (I10)
    bool   autoShow = true;
    // Hide while the pointer is on the video. OFF by default since the overlay
    // became genuinely click-through (WS_EX_LAYERED, measured); kept as an opt-in
    // and as the automatic fallback when the hit-test tripwire fires.
    bool   cursorGate = false;
    bool   overlayTopmost = false;  // legacy WS_EX_TOPMOST placement
    // Owner 2026-09-13: attach to a browser only while it is FULLSCREEN, until the
    // video rect comes from the page (P11). A windowed browser without --rect
    // would put the overlay over the whole page, and with the cursor gate gone
    // every interaction with that page would be shown a source period late.
    bool   allowWindowed = false;   // --allow-windowed
    bool   fullscreenOnly = false;  // --fullscreen-only: no windowed video (P11 off)
    bool   scoutAlways = false;     // --scout-always: lab, no media session needed to treat a picture as video
    bool   pauseForWidgets = false; // --pause-for-widgets: the strict occlusion rule
    bool   overlayLayered = true;   // --overlay-unlayered: measurement of LAYERED's cost only
    bool   logDirty = false;        // --log-dirty: per-capture video-frame test verdicts
    bool   videoFrameTest = true;   // --no-frame-test: every capture is a video frame (A/B)
    std::vector<std::wstring> occlusionIgnore;  // --occlusion-ignore a.exe,b.exe (lower-case)
    bool   listWindows = false;
    bool   listDisplays = false;
    double gateProbeSec = 0.0;   // --game-gate-probe N: watch the gate for N seconds, then exit
    bool   badge = true;         // the "24/165" readout in the top-right corner of the video
    bool   badgeFromCli = false; // --no-badge given: the Settings counter switch does not override it
    // P6: pause generation while a game has the GPU (I1, I2, D3). Off only for
    // an A/B against another frame generator, which is itself a fullscreen GPU
    // load and would otherwise stop us the moment it starts.
    bool   gameGate = true;      // --no-game-gate
    // P11 / I7: protected video is recognised and SKIPPED. Off only for a
    // measurement run, which must not be silently gated by a browser state the
    // measurement did not ask about.
    bool   drmGate = true;       // --no-drm-gate
    int    cellPx = 8;           // motion field cell, full-res pixels
    // MEASURED default. On the first real corpus — Meridian 59.94p decimated to
    // stride 2, 49 triples, each middle frame withheld as ground truth — NVOFA
    // won 49 of 49 and led on every summary statistic: mean 44.92 dB against
    // the block matcher's 42.78 and a plain cross-fade's 43.04, and worst-case
    // 41.39 against 37.69 and 40.52. The block matcher is BELOW the cross-fade
    // on mean and far below it on the worst triple.
    //
    // The eyeball verdict on the synthetic striped clip said the opposite. That
    // clip is adversarial for any estimator (G24) and it is exactly why a
    // corpus exists. Switch back with --motion blocks.
    bool   useOfa = true;
    int    ofaGrid = 4;          // NVOFA output vector grid: 4, 2 or 1
    // Occlusion evidence: see Synth::SetOcclusionMode. Hardware path only — the
    // block matcher has one field and nothing to be bidirectional about.
    int    occMode = 2;

    // The offline quality instrument. Nothing below this line touches capture,
    // the overlay or the compositor: frames come from disk and go back to disk.
    std::string offlineDir;
    std::string offlineOut;
    std::vector<double> offlineTs;
    int  offlineRepeat = 1;
    bool offlineCheck = false;
    bool offlineWarmup = true;
    std::string offlineInject;  // oracle-flow arm: dir of raw f32 fields
    bool   ofaHints = true;     // seed NVOFA with our coarse field (G30); --no-ofa-hints opts out
    int    warpLab = 0;         // --warp-lab N: PSWarpLab branch maps / ablations (P21)
    float  warpLabParam = 0.05f;  // --warp-lab-p F
    int    fieldLab = 0;          // --field-lab N: per-pair field coherence lab (P21)
    bool   offlineGpuTime = false;  // --offline-gpu-time
    bool   fieldCohere = true;      // --no-field-cohere turns P21's field pass off
    bool   ofaSeedHints = false;    // --ofa-seed-hints: real seeds in NVOFA's hint buffer (G54)
    float  fieldLabParam = 0.02f; // --field-lab-p F

    // Tray and Settings (P7).
    std::string settingsFile;  // --settings-file: instead of %LOCALAPPDATA%\Nova SilkPlay\settings.json
    bool defaultSettings = false;  // --default-settings: built-in defaults, no file read or written
    int  uiMonitor = 0;        // --ui-monitor N: where Settings opens; 0 = under the pointer, 1 = primary
    std::string uiSnapshot;    // --ui-snapshot FILE.png: draw the Settings window to a file and exit
    int  uiLang = -1;          // --ui-lang en|ru: this run's interface language, not saved

    // Development aid: after `dumpAfterSec` of steady playback, write the two
    // source frames and the synthesized in-between frame, then exit.
    std::string dumpPrefix;
    double dumpAfterSec = 4.0;
};

void PrintUsage() {
    printf(
        "Nova SilkPlay " NSP_VERSION_TEXT " — frame generation over a browser window.\n"
        "\n"
        "  --list-windows           dump candidate windows and exit\n"
        "  --target-class STR       window class (default Chrome_WidgetWin_1)\n"
        "  --target-title STR       title substring, case-insensitive\n"
        "  --target-pid N           exact pid\n"
        "  --rect X,Y,W,H           video rect in target-CLIENT physical px\n"
        "                           (default: the whole client area — correct for\n"
        "                            fullscreen video, press F on YouTube first)\n"
        "  --rect-screen X,Y,W,H    same, in screen physical px\n"
        "  --mode mc|blend|passthrough   default mc (motion-compensated)\n"
        "  --motion ofa|blocks      motion source, default ofa (hardware flow)\n"
        "  --occ self|bidir|bidir+cand   occlusion evidence (ofa only, default\n"
        "                           bidir+cand): one field sampled twice, or the\n"
        "                           A- and B-anchored fields, or those also\n"
        "                           offered to the per-pixel vector search\n"
        "  --min-update-ms F        WGC MinUpdateInterval (default 1.0, gap G4)\n"
        "  --no-borderless          do not request the Borderless capability\n"
        "  --stall-ms F             hide the overlay after this long with no new\n"
        "                           source frame (default 400)\n"
        "  --manual                 start hidden; Ctrl+Alt+Q or O shows it\n"
        "  --allow-windowed         lab: treat a windowed browser's WHOLE client area as the\n"
        "                           video (--rect implies it)\n"
        "  --fullscreen-only        attach to a fullscreen browser only; by default a video\n"
        "                           playing in a windowed browser is found from DWM's\n"
        "                           damage (P11) and generated in place\n"
        "  --pause-for-widgets      small always-on-top windows (widgets, toasts,\n"
        "                           tooltips) pause generation too; default: ignored\n"
        "  --cursor-gate            pause while the pointer is on the video (old\n"
        "                           behaviour; the overlay is click-through now)\n"
        "  --overlay-topmost        WS_EX_TOPMOST overlay instead of one slot above\n"
        "                           the browser (paints over covering windows)\n"
        "  --no-frame-test          count every capture as a new video frame, even\n"
        "                           when only player UI changed (old behaviour, A/B)\n"
        "  --occlusion-ignore LIST  process names that never pause generation when\n"
        "                           they cover the video, e.g. quotty.exe,foo.exe\n"
        "  --log-dirty              one line per capture: damage, content test, verdict\n"
        "  --no-badge               no source/output fps readout in the top-right\n"
        "                           corner (needed for a bit-exact passthrough check)\n"
        "  --no-drm-gate            generate even when the browser has a content decryption\n"
        "                           module running (measurement runs only - I7 says\n"
        "                           protected video is skipped)\n"
        "  --game-gate-probe N      watch the game gate for N seconds and exit: no window,\n"
        "                           no overlay, no capture, so it can run for hours beside\n"
        "                           ordinary work to prove it never pauses by mistake\n"
        "  --console                open a console for this run; implied by every switch\n"
        "                           that prints (the build is windowless, so autostart\n"
        "                           never flashes one)\n"
        "  --no-game-gate           keep generating while a game runs (A/B runs only)\n"
        "  --nudge-hotkeys          claim Ctrl+Alt+arrows to nudge the source offset by a\n"
        "                           pixel (alignment aid; off, so the chords stay free)\n"
        "  --warp-lab N             warp laboratory: branch maps / ablations (P21)\n"
        "  --field-lab N            per-pair field coherence lab (P21)\n"
        "  --no-field-cohere        turn off the per-pair field coherence pass (P21)\n"
        "  --settings-file PATH     settings JSON (default %%LOCALAPPDATA%%\\Nova SilkPlay\\\n"
        "                           settings.json); the Settings window saves here\n"
        "  --default-settings       built-in settings for this run: settings.json is neither\n"
        "                           read nor written (measurement tools pass this, so the\n"
        "                           owner's choices never change what they measure)\n"
        "  --ui-monitor N           open Settings on monitor N (1 = primary, 2 = the next);\n"
        "                           default: the monitor under the pointer\n"
        "  --ui-lang en|ru          interface language for this run, not saved\n"
        "  --ui-snapshot FILE.png   draw the Settings window (default on monitor 2, without\n"
        "                           taking the focus) to FILE.png and exit\n"
        "  --stats-every F          stats line interval in seconds (default 3)\n"
        "  --dump PREFIX            write PREFIX_a/_b/_mc/_blend .ppm after a few\n"
        "                           seconds of playback, then exit (dev aid)\n"
        "  --dump-after F           when to dump, in seconds (default 4)\n"
        "\n"
        "Tray icon: left click opens Settings, right click the menu.\n"
        "\n"
        "Hotkeys (global): Ctrl+Alt+Q or Ctrl+Alt+O  frame generation ON/OFF\n"
        "                  Ctrl+Alt+M                cycle mc -> blend -> passthrough\n"
        "                  Ctrl+Alt+arrows           nudge the capture rect by 1 px\n"
        "                  Ctrl+Alt+X                QUIT (X, not Q)\n");
}

bool ParseRect(const char* s, RECT* out) {
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf_s(s, "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return false;
    if (w <= 0 || h <= 0) return false;
    *out = RECT{x, y, x + w, y + h};
    return true;
}

std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

// The four arrows nudge the capture's source offset by a pixel: an alignment
// tool, not a feature, and Ctrl+Alt+<arrow> is a combination other software
// (Intel's display rotation, several editors) also claims. A shipping run must
// not take four global chords away from the machine for a lab switch, so they
// are off unless --nudge-hotkeys asks for them.
bool g_nudgeHotkeys = false;

bool ParseArgs(int argc, char** argv, AppOptions* opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char** v) {
            if (i + 1 >= argc) return false;
            *v = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (a == "--help" || a == "-h" || a == "/?") {
            PrintUsage();
            return false;
        } else if (a == "--list-displays") {
            opt->listDisplays = true;
        } else if (a == "--game-gate-probe" && next(&v)) {
            opt->gateProbeSec = atof(v);
        } else if (a == "--list-windows") {
            opt->listWindows = true;
        } else if (a == "--target-class" && next(&v)) {
            opt->targetClass = Widen(v);
            opt->targetFromCli = true;
        } else if (a == "--target-exe" && next(&v)) {
            opt->targetExe = Widen(v);
            opt->targetFromCli = true;
        } else if (a == "--target-title" && next(&v)) {
            opt->targetTitle = Widen(v);
            opt->targetFromCli = true;
        } else if (a == "--target-pid" && next(&v)) {
            opt->targetPid = static_cast<DWORD>(strtoul(v, nullptr, 10));
            opt->targetExe.clear();  // an explicit pid is the whole answer
            opt->targetFromCli = true;
        } else if (a == "--rect" && next(&v)) {
            if (!ParseRect(v, &opt->rectClient)) {
                LogErr("bad --rect '%s'", v);
                return false;
            }
            opt->haveRect = true;
            opt->rectIsScreen = false;
        } else if (a == "--rect-screen" && next(&v)) {
            if (!ParseRect(v, &opt->rectClient)) {
                LogErr("bad --rect-screen '%s'", v);
                return false;
            }
            opt->haveRect = true;
            opt->rectIsScreen = true;
        } else if (a == "--mode" && next(&v)) {
            if (strcmp(v, "passthrough") == 0) {
                opt->mode = Mode::Passthrough;
            } else if (strcmp(v, "blend") == 0) {
                opt->mode = Mode::Blend;
            } else if (strcmp(v, "mc") == 0) {
                opt->mode = Mode::Mc;
            } else {
                LogErr("bad --mode '%s'", v);
                return false;
            }
        } else if (a == "--min-update-ms" && next(&v)) {
            opt->minUpdateMs = atof(v);
        } else if (a == "--no-borderless") {
            opt->borderless = false;
        } else if (a == "--stall-ms" && next(&v)) {
            opt->stallMs = atof(v);
        } else if (a == "--manual") {
            opt->autoShow = false;
        } else if (a == "--cursor-gate") {
            opt->cursorGate = true;
        } else if (a == "--overlay-topmost") {
            opt->overlayTopmost = true;
        } else if (a == "--allow-windowed") {
            opt->allowWindowed = true;
        } else if (a == "--fullscreen-only") {
            opt->fullscreenOnly = true;
        } else if (a == "--scout-always") {
            opt->scoutAlways = true;
        } else if (a == "--pause-for-widgets") {
            opt->pauseForWidgets = true;
        } else if (a == "--overlay-unlayered") {
            opt->overlayLayered = false;
        } else if (a == "--log-dirty") {
            opt->logDirty = true;
        } else if (a == "--no-frame-test") {
            opt->videoFrameTest = false;
        } else if (a == "--occlusion-ignore" && next(&v)) {
            // Forgiving on purpose: an entry that silently never matches is the
            // worst outcome (found in review). Spaces are trimmed, a path keeps
            // only its file name, and a bare name gets ".exe".
            std::wstring list = Widen(v);
            std::transform(list.begin(), list.end(), list.begin(), ::towlower);
            size_t start = 0;
            for (;;) {
                const size_t comma = list.find(L',', start);
                std::wstring item = list.substr(
                    start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                const size_t b = item.find_first_not_of(L" \t\"");
                const size_t e = item.find_last_not_of(L" \t\"");
                item = (b == std::wstring::npos) ? std::wstring() : item.substr(b, e - b + 1);
                const size_t slash = item.find_last_of(L"\\/");
                if (slash != std::wstring::npos) item = item.substr(slash + 1);
                if (!item.empty() && item.find(L'.') == std::wstring::npos) item += L".exe";
                if (!item.empty()) opt->occlusionIgnore.push_back(item);
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        } else if (a == "--stats-every" && next(&v)) {
            opt->statsEverySec = atof(v);
        } else if (a == "--motion" && next(&v)) {
            if (strcmp(v, "ofa") == 0) {
                opt->useOfa = true;
            } else if (strcmp(v, "blocks") == 0) {
                opt->useOfa = false;
            } else {
                LogErr("bad --motion '%s' (ofa|blocks)", v);
                return false;
            }
        } else if (a == "--occ" && next(&v)) {
            if (strcmp(v, "self") == 0) {
                opt->occMode = 0;
            } else if (strcmp(v, "bidir") == 0) {
                opt->occMode = 1;
            } else if (strcmp(v, "bidir+cand") == 0) {
                opt->occMode = 2;
            } else {
                LogErr("bad --occ '%s' (self|bidir|bidir+cand)", v);
                return false;
            }
        } else if (a == "--ofa-grid" && next(&v)) {
            opt->ofaGrid = atoi(v);
        } else if (a == "--cell" && next(&v)) {
            opt->cellPx = atoi(v);
        } else if (a == "--no-badge") {
            opt->badge = false;
            opt->badgeFromCli = true;
        } else if (a == "--nudge-hotkeys") {
            g_nudgeHotkeys = true;
        } else if (a == "--no-game-gate") {
            opt->gameGate = false;
        } else if (a == "--no-drm-gate") {
            opt->drmGate = false;
        } else if (a == "--console") {
            // Handled before the options are parsed; accepted here so it is not
            // reported as unknown.
        } else if (a == "--settings-file" && next(&v)) {
            opt->settingsFile = v;
        } else if (a == "--default-settings") {
            opt->defaultSettings = true;
        } else if (a == "--ui-monitor" && next(&v)) {
            opt->uiMonitor = atoi(v);
        } else if (a == "--ui-snapshot" && next(&v)) {
            opt->uiSnapshot = v;
        } else if (a == "--ui-lang" && next(&v)) {
            if (strcmp(v, "en") == 0) {
                opt->uiLang = static_cast<int>(UiLanguage::kEnglish);
            } else if (strcmp(v, "ru") == 0) {
                opt->uiLang = static_cast<int>(UiLanguage::kRussian);
            } else {
                LogErr("bad --ui-lang '%s' (en|ru)", v);
                return false;
            }
        } else if (a == "--offline" && next(&v)) {
            opt->offlineDir = v;
        } else if (a == "--offline-out" && next(&v)) {
            opt->offlineOut = v;
        } else if (a == "--offline-t" && next(&v)) {
            opt->offlineTs.clear();
            for (const char* p = v; *p;) {
                char* end = nullptr;
                const double t = strtod(p, &end);
                if (end == p) break;
                opt->offlineTs.push_back(t);
                p = (*end == ',') ? end + 1 : end;
            }
        } else if (a == "--offline-repeat" && next(&v)) {
            opt->offlineRepeat = atoi(v);
        } else if (a == "--offline-check") {
            opt->offlineCheck = true;
        } else if (a == "--offline-no-warmup") {
            opt->offlineWarmup = false;
        } else if (a == "--offline-inject" && next(&v)) {
            opt->offlineInject = v;
        } else if (a == "--ofa-hints") {
            opt->ofaHints = true;
        } else if (a == "--warp-lab" && next(&v)) {
            opt->warpLab = atoi(v);
        } else if (a == "--ofa-seed-hints") {
            opt->ofaSeedHints = true;
        } else if (a == "--no-field-cohere") {
            opt->fieldCohere = false;
        } else if (a == "--offline-gpu-time") {
            opt->offlineGpuTime = true;
        } else if (a == "--field-lab" && next(&v)) {
            opt->fieldLab = atoi(v);
        } else if (a == "--field-lab-p" && next(&v)) {
            opt->fieldLabParam = static_cast<float>(atof(v));
        } else if (a == "--warp-lab-p" && next(&v)) {
            opt->warpLabParam = static_cast<float>(atof(v));
        } else if (a == "--no-ofa-hints") {
            opt->ofaHints = false;
        } else if (a == "--dump" && next(&v)) {
            opt->dumpPrefix = v;
        } else if (a == "--dump-after" && next(&v)) {
            opt->dumpAfterSec = atof(v);
        } else {
            LogErr("unknown argument '%s' (try --help)", a.c_str());
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ cadence

// Source frame rate and capture pipeline delay, both estimated from arrivals.
// The product will read the rate from the page instead (I8) — capture cadence is
// exactly what makes Lossless Scaling misread 24 fps video as 165 fps.
class Cadence {
public:
    void OnFrame(double t100ns, double arrival100ns) {
        // MEASURED, and counter-intuitive: Direct3D11CaptureFrame::SystemRelativeTime
        // is a PRESENTATION timestamp about one refresh in the FUTURE relative to
        // the moment the frame is dequeued (median -6.01 ms at 165 Hz over 1925
        // frames of the S-M2 data). So this delta is normally NEGATIVE, and a
        // filter that keeps only positive samples throws the whole population away
        // and leaves a phase that is a third of a source period off.
        const double delay = arrival100ns - t100ns;
        if (delay > -1000000.0 && delay < 2000000.0) {  // -100 ms .. +200 ms
            delays_[delayN_++ % kN] = delay;
            if (delayCount_ < kN) ++delayCount_;
        }
        if (haveLast_) {
            const double dt = t100ns - lastT_;
            // 2 ms .. 250 ms: below is a duplicate arrival, above is a stall.
            if (dt > 20000.0 && dt < 2500000.0) {
                intervals_[n_++ % kN] = dt;
                if (count_ < kN) ++count_;
            }
        }
        lastT_ = t100ns;
        haveLast_ = true;
    }

    bool Ready() const { return count_ >= 6; }

    // Source period in 100 ns units, as a TRIMMED MEAN — not a median.
    //
    // Measured why: a 60 fps source on a 165 Hz display composites every 2.75
    // refreshes, so its arrival intervals alternate 3,3,3,2 ticks. The median of
    // that is 18.2 ms (55 fps) while the truth is 16.67 ms (60 fps), and a 9 %
    // error in the period puts a quarter of the generated frames on a clamped
    // phase. The mean of the same sequence is exactly right. The trim window
    // keeps the short-interval members of a beat pattern while still throwing
    // out a doubled interval (a repeated source frame) and a stall.
    double Period100ns() const {
        return EstimateSourcePeriod100ns(intervals_, count_);
    }

    // Median capture delay. The error either way costs a clamped phase at one end
    // of the interval, so the median (not a tail percentile) is what minimises it.
    double Delay100ns() const {
        if (delayCount_ == 0) return 0.0;
        double tmp[kN];
        std::copy(delays_, delays_ + delayCount_, tmp);
        std::nth_element(tmp, tmp + delayCount_ / 2, tmp + delayCount_);
        return tmp[delayCount_ / 2];
    }

    double Fps() const {
        const double p = Period100ns();
        return p > 0.0 ? 10000000.0 / p : 0.0;
    }

    // min / median / max of the accepted arrival intervals, in ms. The shape of
    // this distribution IS the cadence: one value means a clean periodic source,
    // two values means a beat against the refresh, a long tail means repeats.
    void IntervalSpreadMs(double* lo, double* mid, double* hi) const {
        *lo = *mid = *hi = 0.0;
        if (count_ == 0) return;
        double tmp[kN];
        std::copy(intervals_, intervals_ + count_, tmp);
        std::sort(tmp, tmp + count_);
        *lo = tmp[0] / 10000.0;
        *mid = tmp[count_ / 2] / 10000.0;
        *hi = tmp[count_ - 1] / 10000.0;
    }

    void Reset() {
        count_ = n_ = delayCount_ = delayN_ = 0;
        haveLast_ = false;
    }

private:
    static constexpr size_t kN = 32;
    double intervals_[kN] = {};
    double delays_[kN] = {};
    size_t count_ = 0, n_ = 0;
    size_t delayCount_ = 0, delayN_ = 0;
    double lastT_ = 0.0;
    bool haveLast_ = false;
};

// ------------------------------------------------------------------ pacing
//
// PACING (P17). A source frame's presentation stamp is not its content time.
// Chrome shows 24 fps on a 60 Hz output 33 and 50 ms apart, and a phase measured
// from those stamps with one pair in hand has to clamp: right after an early
// frame the phase is below 0, right before a late one it is above 1, and no
// choice of display lag avoids both. Measured with tools/ls-compare: about half
// of the engine's displayed frames were held source frames, against a quarter
// for Lossless Scaling.
//
// So the content timeline is REGULARISED — a least-squares line through the
// last 32 (index, stamp) points, indices counted in whole source periods so a
// dropped frame or a still shot keeps its place — and the picture is shown one
// period plus the capture delay plus a JITTER MARGIN late, the 95th percentile of
// how far frames land after that line. With three frames and two fields in hand,
// the moment on screen is then always inside a pair the engine already has.
class Timeline {
public:
    void Reset() {
        n_ = next_ = 0;
        lastK_ = -1;
        bad_ = 0;
        ready_ = false;
    }

    // A new video frame with presentation stamp `raw`; returns its index.
    int64_t Add(double raw, double period) {
        int64_t k = 0;
        if (lastK_ >= 0 && period > 0.0)
            k = lastK_ + (std::max)(1LL, static_cast<long long>(std::llround((raw - lastRaw_) / period)));
        if (ready_ && period > 0.0) {
            if (fabs(raw - TimeOf(k)) > 0.75 * period) {
                // Three misses in a row: the source changed rate or restarted.
                if (++bad_ >= 3) {
                    Reset();
                    k = 0;
                }
            } else {
                bad_ = 0;
            }
        }
        k_[next_] = k;
        raw_[next_] = raw;
        next_ = (next_ + 1) % kN;
        if (n_ < kN) ++n_;
        lastK_ = k;
        lastRaw_ = raw;
        Fit(period);
        return k;
    }

    bool Ready() const { return ready_; }
    double TimeOf(int64_t k) const { return a_ + b_ * static_cast<double>(k - k0_); }
    double Period100ns() const { return b_; }
    double Margin100ns() const { return margin_; }

private:
    static constexpr size_t kN = 32;

    void Fit(double period) {
        ready_ = false;
        if (n_ < 8 || period <= 0.0) return;
        const size_t oldest = (next_ + kN - n_) % kN;
        k0_ = k_[oldest];
        const double base = raw_[oldest];
        bool use[kN];
        for (size_t i = 0; i < kN; ++i) use[i] = true;
        double a = 0.0, b = period;
        for (int pass = 0; pass < 2; ++pass) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            size_t m = 0;
            for (size_t j = 0; j < n_; ++j) {
                const size_t i = (oldest + j) % kN;
                if (!use[i]) continue;
                const double x = static_cast<double>(k_[i] - k0_), y = raw_[i] - base;
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
                ++m;
            }
            const double den = static_cast<double>(m) * sxx - sx * sx;
            if (m < 6 || den <= 0.0) return;
            b = (static_cast<double>(m) * sxy - sx * sy) / den;
            a = (sy - b * sx) / static_cast<double>(m);
            // Second pass without the points that sit half a period off the
            // line: a frame stamped after a stall must not tilt it.
            for (size_t j = 0; j < n_; ++j) {
                const size_t i = (oldest + j) % kN;
                use[i] = fabs(raw_[i] - base - (a + b * static_cast<double>(k_[i] - k0_))) <= 0.5 * period;
            }
        }
        if (b < 0.5 * period || b > 1.5 * period) return;
        a_ = a + base;
        b_ = b;
        double late[kN];
        size_t m = 0;
        for (size_t j = 0; j < n_; ++j) {
            const size_t i = (oldest + j) % kN;
            if (use[i]) late[m++] = raw_[i] - TimeOf(k_[i]);
        }
        std::sort(late, late + m);
        margin_ = m ? (std::clamp)(late[(m * 95) / 100 < m ? (m * 95) / 100 : m - 1], 0.0, 0.9 * b) : 0.0;
        ready_ = true;
    }

    int64_t k_[kN] = {};
    double raw_[kN] = {};
    size_t n_ = 0, next_ = 0;
    int64_t lastK_ = -1;
    double lastRaw_ = 0.0;
    int bad_ = 0;
    bool ready_ = false;
    int64_t k0_ = 0;
    double a_ = 0.0, b_ = 0.0, margin_ = 0.0;
};

// ------------------------------------------------------- compositor clock

using PFN_WaitForCompositorClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

PFN_WaitForCompositorClock LoadCompositorClock() {
    HMODULE dll = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return nullptr;
    return reinterpret_cast<PFN_WaitForCompositorClock>(
        GetProcAddress(dll, "DCompositionWaitForCompositorClock"));
}

// ------------------------------------------------------------------ hotkeys

enum HotkeyId {
    kHkToggle = 1,
    kHkToggle2,
    kHkMode,
    kHkLeft,
    kHkRight,
    kHkUp,
    kHkDown,
    kHkQuit,
};

// Our hotkeys arrive as this thread message, whether they came from the
// low-level hook or from RegisterHotKey.
constexpr UINT WM_NSP_HOTKEY = WM_APP + 7;

void RegisterHotkeysFallback();

HHOOK g_kbHook = nullptr;
DWORD g_hookThread = 0;   // the dedicated hook thread
DWORD g_mainThread = 0;   // where hotkeys are delivered
HANDLE g_hookThreadHandle = nullptr;

int HotkeyIdForVk(DWORD vk) {
    switch (vk) {
        case 'Q':
        case 'O': return kHkToggle;
        case 'M': return kHkMode;
        case 'X': return kHkQuit;
        case VK_LEFT: return g_nudgeHotkeys ? kHkLeft : 0;
        case VK_RIGHT: return g_nudgeHotkeys ? kHkRight : 0;
        case VK_UP: return g_nudgeHotkeys ? kHkUp : 0;
        case VK_DOWN: return g_nudgeHotkeys ? kHkDown : 0;
        default: return 0;
    }
}

// RegisterHotKey is not enough here, and the reason is worth keeping: another
// program can hold the same combination through its own low-level hook and act
// on it as well. MEASURED on this machine — Lossless Scaling is resident and
// answers Ctrl+Alt+Q by scaling the target window to fullscreen (and shrinking
// the cursor, which is the I12 artifact), while our handler also ran. A
// WH_KEYBOARD_LL hook installed after theirs is called first and can swallow
// the key, so only one program reacts.
//
// This hook looks at nothing else: any key without both Ctrl and Alt down, or
// any key that is not one of ours, is passed straight through untouched.
//
// TWO RULES that are not optional, both found by the beta audit (2026-09-20):
//
// 1. RIGHT ALT IS NOT OUR ALT. On every European layout AltGr is delivered as
//    Ctrl+RightAlt, and AltGr+<key> is how those users type @ { } [ ] \ and
//    their own letters. A hook that consumes Ctrl+Alt+Q would make Q
//    untypeable on such a layout for as long as we run — machine-wide, in every
//    other program. So the chord counts only with the LEFT Alt down and the
//    right one up. The cost is that Ctrl+RightAlt+Q does not toggle generation;
//    that is the correct trade.
//
// 2. AUTOREPEAT IS NOT A SECOND PRESS. Held down, Windows repeats a key ~30
//    times a second; each repeat used to post another toggle, so holding
//    Ctrl+Alt+Q strobed the overlay on and off. Only the transition counts, so
//    the down state of each chord is tracked here and cleared on key-up. The
//    key-up is swallowed too: having eaten the down, letting the up through
//    would leave other programs believing the key is still held.
bool g_chordDown[kHkQuit + 1] = {};

// INPUT THAT CAN MOVE A WINDOWED VIDEO (P11). The geometry guard notices a scroll from the
// capture, but a capture arrives after DWM has already shown the scrolled page - one frame
// of our picture over the old place. The input that causes a scroll arrives BEFORE it, so
// a wheel notch over the browser, or a scrolling key while it has the focus, hides the
// overlay first; it comes back once the video's own rect has been seen again. Observation
// only: nothing is consumed, nothing is delayed.
std::atomic<HWND> g_inputTarget{nullptr};       // the windowed browser, or null
std::atomic<int64_t> g_inputPreemptQpc{0};      // QpcNow() of the last such input

bool IsScrollKey(DWORD vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
        case VK_SPACE:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK LowLevelKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && IsScrollKey(k->vkCode)) {
            const HWND t = g_inputTarget.load(std::memory_order_relaxed);
            if (t && GetForegroundWindow() == t) g_inputPreemptQpc.store(QpcNow(), std::memory_order_release);
        }
        const int id = HotkeyIdForVk(k->vkCode);
        if (id != 0 && id <= kHkQuit) {
            if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                if (g_chordDown[id]) {
                    g_chordDown[id] = false;
                    return 1;  // we ate the down; the up belongs to us too
                }
            } else if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool lalt = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
                const bool ralt = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
                if (ctrl && lalt && !ralt) {
                    if (!g_chordDown[id]) {
                        g_chordDown[id] = true;
                        PostThreadMessageW(g_mainThread, WM_NSP_HOTKEY,
                                           static_cast<WPARAM>(id), 0);
                    }
                    return 1;  // swallowed: nobody else gets this combination
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// THE HOOK GETS ITS OWN THREAD, and that is not tidiness.
//
// A WH_KEYBOARD_LL callback is delivered to the message queue of the thread that
// installed it, and Windows BLOCKS the keystroke system-wide until that thread
// returns or LowLevelHooksTimeout expires (300 ms by default) — after which the
// hook is silently removed and never fires again. The thread that used to own
// this hook is the one pacing 165 frames a second: it submits GPU work, waits on
// the compositor clock and only reaches PeekMessage between ticks. Every
// keystroke on the whole machine was therefore waiting behind a frame, and a
// single slow frame would unhook us for good — which is exactly the shape of
// "Ctrl+Alt+Q stopped doing anything" and of the system feeling like it is
// intercepting input.
//
// A dedicated thread does nothing but GetMessage, so the callback returns in
// microseconds no matter what the renderer is doing.
// Raw input, not a WH_MOUSE_LL hook: a low-level mouse hook would put every mouse event on
// the machine behind this thread; raw input with RIDEV_INPUTSINK is a copy delivered after
// the fact, so nothing anyone does with the mouse can wait for us.
LRESULT CALLBACK InputSinkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size,
                            sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE &&
            (ri.data.mouse.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL))) {
            // The pointer inside the browser's window rect, not WindowFromPoint: that one
            // can SendMessage(WM_NCHITTEST) to a hung window, and this thread also carries the
            // keyboard hook, which must never wait (review, 2026-10-06). A wheel over another
            // window that overlaps the browser only costs a brief, harmless hide.
            const HWND t = g_inputTarget.load(std::memory_order_relaxed);
            POINT pt{};
            RECT wr{};
            if (t && GetCursorPos(&pt) && GetWindowRect(t, &wr) && PtInRect(&wr, pt))
                g_inputPreemptQpc.store(QpcNow(), std::memory_order_release);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateInputSink() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = InputSinkProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NovaSilkPlay.InputSink";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             wc.hInstance, nullptr);
    if (!h) return nullptr;
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, h};  // generic desktop / mouse
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        LogErr("raw mouse input unavailable (GetLastError=%lu) - a wheel scroll of a windowed "
               "video is noticed one frame late, from the capture", GetLastError());
        DestroyWindow(h);
        return nullptr;
    }
    return h;
}

DWORD WINAPI HookThreadProc(LPVOID param) {
    g_hookThread = GetCurrentThreadId();
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyProc, GetModuleHandleW(nullptr), 0);
    const HWND sink = CreateInputSink();
    SetEvent(static_cast<HANDLE>(param));  // Install() may now read g_kbHook
    if (!g_kbHook && !sink) return 0;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // WM_NSP_HOTKEY is posted to g_mainThread by the callback; the only window here
        // is the raw-input sink, and WM_QUIT ends the loop at shutdown.
        DispatchMessageW(&msg);
    }
    if (sink) DestroyWindow(sink);
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    g_kbHook = nullptr;
    return 0;
}

void InstallHotkeys() {
    g_mainThread = GetCurrentThreadId();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hookThreadHandle = CreateThread(nullptr, 0, &HookThreadProc, ready, 0, nullptr);
    if (g_hookThreadHandle && ready) WaitForSingleObject(ready, 2000);
    if (ready) CloseHandle(ready);
    if (g_kbHook) {
        Log("hotkeys: low-level keyboard hook installed - Ctrl+Alt combinations are "
            "consumed by us and not passed on (Lossless Scaling holds Ctrl+Alt+Q too)");
        return;
    }
    LogErr("hotkeys: SetWindowsHookEx(WH_KEYBOARD_LL) failed, GetLastError=%lu - falling back to "
           "RegisterHotKey, which cannot stop another program acting on the same key",
           GetLastError());
    RegisterHotkeysFallback();
}

void RegisterHotkeysFallback() {
    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    struct {
        int id;
        UINT vk;
        const char* name;
    } keys[] = {
        // Ctrl+Alt+Q is the ON/OFF switch (owner's choice); quitting moved to X.
        {kHkToggle, 'Q', "Ctrl+Alt+Q on/off"},   {kHkToggle2, 'O', "Ctrl+Alt+O on/off (alias)"},
        {kHkMode, 'M', "Ctrl+Alt+M mode"},       {kHkLeft, VK_LEFT, "Ctrl+Alt+Left"},
        {kHkRight, VK_RIGHT, "Ctrl+Alt+Right"},  {kHkUp, VK_UP, "Ctrl+Alt+Up"},
        {kHkDown, VK_DOWN, "Ctrl+Alt+Down"},     {kHkQuit, 'X', "Ctrl+Alt+X quit"},
    };
    for (const auto& k : keys) {
        // Same rule as the hook: the arrows are a lab tool and are not claimed
        // machine-wide unless asked for.
        if (!g_nudgeHotkeys &&
            (k.id == kHkLeft || k.id == kHkRight || k.id == kHkUp || k.id == kHkDown))
            continue;
        if (!RegisterHotKey(nullptr, k.id, mods, k.vk))
            LogErr("RegisterHotKey(%s) failed, GetLastError=%lu (key unavailable)", k.name,
                   GetLastError());
    }
}

void UninstallHotkeys() {
    if (g_hookThreadHandle) {
        // The hook must be removed on the thread that installed it, so ask that
        // thread to leave its loop rather than unhooking from here.
        if (g_hookThread) PostThreadMessageW(g_hookThread, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThreadHandle, 2000);
        CloseHandle(g_hookThreadHandle);
        g_hookThreadHandle = nullptr;
        g_hookThread = 0;
    }
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
    for (int id = kHkToggle; id <= kHkQuit; ++id) UnregisterHotKey(nullptr, id);
}

// ------------------------------------------------------------------ console

// Ctrl+C, Ctrl+Break and closing the console window all mean "quit". Without a handler the
// process is killed where it stands: the overlay and capture are not released cleanly and the
// tray icon stays behind as a dead entry until the pointer passes over it.
std::atomic<bool> g_consoleQuit{false};
HANDLE g_runDone = nullptr;  // set once the main loop has cleaned up

// ASK US TO QUIT FROM ANOTHER PROCESS. The build is /SUBSYSTEM:WINDOWS, so it
// has no console and GenerateConsoleCtrlEvent can no longer reach it - which is
// how every harness used to stop it, and how a script would otherwise be left
// with TerminateProcess and a tray icon nobody removed. Any tool can now do
//   SetEvent(OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\NovaSilkPlay.Quit"))
// and the loop leaves by the same path as Ctrl+Alt+X.
constexpr wchar_t kQuitEventName[] = L"Local\\NovaSilkPlay.Quit";
HANDLE g_quitEvent = nullptr;

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    g_consoleQuit.store(true);
    if (type == CTRL_CLOSE_EVENT && g_runDone) {
        // Windows ends the process as soon as this returns: hold it until the loop is done. The
        // loop notices within one iteration (at most ~100 ms of compositor-clock wait).
        WaitForSingleObject(g_runDone, 4000);
    }
    return TRUE;
}

// ---------------------------------------------------------------------- app

class App {
public:
    App(const AppOptions& opt, const Settings& settings, const std::wstring& settingsPath,
        bool settingsUnreadable)
        : opt_(opt), settings_(settings), settingsPath_(settingsPath),
          settingsUnreadable_(settingsUnreadable) {
        if (!opt_.badgeFromCli) opt_.badge = settings_.showFpsCounter;
        char* tracePath = nullptr;
        traceInitFailed_ = _dupenv_s(&tracePath, nullptr, "NSP_PRESENT_TRACE") != 0;
        if (tracePath) presentTracePath_ = tracePath;
        std::free(tracePath);
        if (!presentTracePath_.empty()) presentTrace_.reserve(60000);
    }

    int Run();

private:
    bool ResolveTarget();
    bool NeedFullscreen() const { return !opt_.allowWindowed && !opt_.haveRect; }
    // P11: a video in a WINDOWED browser is found from DWM's damage and generated in place.
    bool WindowedAuto() const { return NeedFullscreen() && !opt_.fullscreenOnly; }
    // Whether a moving picture in the browser may be taken for a video at all. Without a page
    // channel (D30) a playing media session is the only evidence: a WebGL game, a looping ad and
    // a player stuck loading all pass the capture's video-frame test (I1, I10, G91).
    bool MediaGateOpen(int64_t now) const {
        return opt_.scoutAlways || now < manualScoutUntil_ || media_.State() != MediaState::kIdle;
    }
    void CheckMediaGate(int64_t now);
    void Scout(int64_t now);
    void StopScout();
    void GuardCapture();
    bool Engage();
    void Disengage();
    void PollCapture();
    void Tick();
    void LogStats();
    bool DoDump();
    bool WritePresentTrace();
    // "<source fps>/<output fps>" for the readout.
    std::wstring FpsText() const;
    // A change made in the Settings window, applied between ticks.
    void ApplySettings(const Settings& s);
    void ToggleGeneration(const char* source);
    // The tray icon's hover text: what the engine is doing right now, in the user's language.
    std::wstring TrayStatus() const;

    AppOptions opt_;
    Settings settings_;
    std::wstring settingsPath_;       // empty: changes apply to this run only
    bool settingsUnreadable_ = false; // the file existed but could not be read: nothing is captured
    TargetWindow target_{};
    RECT videoClient_{};  // video rect relative to the client origin (source of truth)

    std::unique_ptr<Overlay> overlay_;
    std::unique_ptr<Capture> capture_;
    std::shared_ptr<CaptureRuntime> captureRuntime_ = std::make_shared<CaptureRuntime>();
    std::unique_ptr<Synth> synth_;
    Cadence cadence_;
    Timeline timeline_;
    int64_t kOlder_ = -1, kPrev_ = -1, kNewest_ = -1;  // timeline indices of the three frames
    uint64_t olderPairTicks_ = 0, pairTicks_ = 0;

    bool engaged_ = false;
    bool quit_ = false;
    bool wantShown_ = true;    // what the user asked for (Ctrl+Alt+O)
    bool stalled_ = true;      // no source frame recently -> overlay hidden
    bool obscured_ = false;    // another window is over the video -> paused (I10)
    // P6, the game gate. gamePaused_ mirrors the sampler's verdict on the engine
    // thread so every consumer sees one value for a whole iteration.
    GameGate gameGate_;
    bool gamePaused_ = false;
    int64_t gamePausedAt_ = 0;
    bool gameYielded_ = false;  // we disengaged to hand the device back to the game
    GameSignal gameSignal_ = GameSignal::kNone;
    std::wstring gameExe_;
    // P11. Unlike the game gate this one fails CLOSED: anything but a clear
    // verdict means we do not put a picture on the screen.
    DrmWatch drm_;
    bool drmBlocked_ = false;
    DrmSignal drmSignal_ = DrmSignal::kNone;
    bool cursorOver_ = false;  // only with the cursor gate on: pointer on the video -> paused
    bool cursorGateTripped_ = false;  // the overlay took hit tests: input was being swallowed
    int64_t lastObscuredCheck_ = 0;
    int64_t cursorLeftAt_ = 0;
    WindowWatch watch_;
    // P11, the windowed video. Scouting runs only while nothing is engaged, the browser is
    // windowed and Windows says it is playing media; the guard runs while engaged on the
    // rect the scout found, and hides the overlay the moment the page may have moved.
    MediaWatch media_;
    std::unique_ptr<DamageScout> scout_;
    TargetWindow scoutTarget_{};
    RectFinder finder_;
    int64_t scoutLookQpc_ = 0;       // last target lookup while scouting (4 Hz)
    int64_t scoutStartQpc_ = 0;
    int64_t scoutLogQpc_ = 0;
    bool haveAutoRect_ = false;      // the scout found a video: engage on it next
    int64_t autoRectQpc_ = 0;
    TargetWindow autoTarget_{};      // the window as it was when the rect was found
    RECT autoSurface_{};             // the video rect, WGC surface coordinates
    LONG autoSurfW_ = 0, autoSurfH_ = 0;
    std::vector<DamageSig> autoCompanions_;
    bool engageWindowed_ = false;    // ResolveTarget chose the found rect
    bool guardActive_ = false;       // engaged on a found rect: the guard decides showing
    GeometryGuard guard_;
    bool guardHidLogged_ = false;
    int64_t lastPreemptSeen_ = 0;
    uint64_t guardHides_ = 0, guardGiveUps_ = 0;
    std::vector<TimedDamage> damageBuf_;
    bool engageNow_ = false;         // skip the 1 s re-engage wait once
    bool guardWantsRescout_ = false; // the guard gave up: disengage from the main loop
    int64_t scoutSearchQpc_ = 0;     // when the current search began (the cost duty cycle)
    int64_t manualScoutUntil_ = 0;   // explicit ON searches even without a media session
    bool mediaProven_ = false;       // a browser reported Playing during this engagement
    int64_t mediaIdleSinceQpc_ = 0;  // when this engagement last stopped being backed by one
    int64_t scoutDecideQpc_ = 0;     // last RectFinder::Found (10 Hz)
    RECT scoutViewport_{};           // the page area the finder was reset with
    RECT autoViewport_{};            // ... and the one the found rect came with
    bool scoutNoViewportLogged_ = false;
    int64_t rescoutHoldUntil_ = 0;   // repeated short-lived windowed engagements back off
    int shortGiveUps_ = 0;
    bool shownThisEngage_ = false;   // the overlay showed at least once since Engage
    int64_t scoutFailQpc_ = 0;       // a scout that cannot start is retried after 10 s
    uint64_t occlChecks_ = 0;
    double occlUsSum_ = 0.0, occlUsMax_ = 0.0;
    uint64_t zFixesAtStats_ = 0;
    HWND lastWidgetLogged_ = nullptr;
    double lastIngest100ns_ = 0.0;
    LONG nudgeX_ = 0, nudgeY_ = 0;

    // stats
    int64_t statsQpc_ = 0;
    uint64_t presents_ = 0;
    uint64_t framesAtStats_ = 0;
    double uMin_ = 2.0, uMax_ = -1.0;
    // Output pacing. One desktop can mix refresh rates; the rate that matters is
    // the one of the monitor the VIDEO RECT is on, re-read whenever the window
    // moves. Generating 165 fps for a 60 Hz output is work nobody sees.
    double targetHz_ = 0.0;
    double desktopMaxHz_ = 0.0;   // fastest output on the desktop
    bool rateGate_ = false;       // only true when this output is the slower one
    int64_t nextPresentQpc_ = 0;

    // watermark
    double outFpsSmooth_ = 0.0;   // presents per second, refreshed twice a second
    int shownSrcFps_ = 0, shownOutFps_ = 0;  // what the readout says, with hysteresis
    int64_t badgeFpsQpc_ = 0;
    uint64_t badgeFrames_ = 0;

    uint64_t held_ = 0;  // ticks where the phase was clamped (source frame late)
    struct PresentTraceRow {
        int64_t qpc;
        double a, b, phase, pollMs, motionMs;
        uint64_t motionSerial;
        int64_t motionDone;
        int64_t previous, newest;
        bool older, pair;
        OverlayPresentTrace media;
    };
    std::string presentTracePath_;
    std::vector<PresentTraceRow> presentTrace_;
    struct MotionGpuTrace {
        uint64_t serial = 0;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
    };
    std::vector<MotionGpuTrace> motionGpuTrace_;
    bool presentTraceFull_ = false;
    bool traceInitFailed_ = false;
    uint64_t traceMotionSerial_ = 0;
    double traceMotionMs_ = 0.0;
    int64_t traceMotionDone_ = 0;
    // Pacing quality, the number a viewer actually feels: between two presents the
    // displayed CONTENT time should advance by exactly the wall time that passed.
    // Any difference is judder, whatever caused it (a clamped phase, a wrong
    // period, a pair that was not really two video frames).
    double judderLastContent100ns_ = 0.0;
    double judderLastWall100ns_ = 0.0;
    double judderAbsMs_ = 0.0;
    uint64_t judderN_ = 0, judderBig_ = 0;
    uint64_t judderHolding_ = 0, judderJumps_ = 0;  // presents excluded from the number, and why
    uint64_t subTickPolls_ = 0;
    uint64_t ticks_ = 0;        // main-loop iterations
    double msClock_ = 0, msWait_ = 0, msTick_ = 0;  // where a loop iteration goes
    uint64_t gateSkips_ = 0;    // ticks the rate gate rejected
    uint64_t gateResyncs_ = 0, gateIntervals_ = 0, gatePhaseSamples_ = 0;
    int64_t gateLastTickQpc_ = 0;
    double gateIntervalSumMs_ = 0.0, gateIntervalMinMs_ = 0.0, gateIntervalMaxMs_ = 0.0;
    double gatePhaseAbsSumMs_ = 0.0, gatePhaseMinMs_ = 0.0, gatePhaseMaxMs_ = 0.0;
    struct CpuStage {
        double totalMs = 0.0, maxMs = 0.0;
        uint64_t runs = 0;
        void Add(int64_t start) {
            const double ms = QpcToMs(QpcNow() - start);
            totalMs += ms;
            maxMs = (std::max)(maxMs, ms);
            ++runs;
        }
    };
    CpuStage captureCpu_, motionCpu_, acquireCpu_, drawCpu_, presentCpu_;
    uint64_t reengageFails_ = 0;
    // True when the last failed Engage() got past ResolveTarget and had already
    // started building the overlay, the synth and the capture. Waiting for a
    // browser is cheap and must stay responsive at one attempt a second; a
    // target that exists but cannot be engaged is a real fault, usually a
    // persistent one, and retrying the whole GPU build once a second for ever
    // is how a background tool ends up pinned at the top of Task Manager (beta
    // audit, 2026-09-20).
    bool engageCostly_ = false;
    int64_t engagedAtQpc_ = 0;  // when the last successful Engage() finished
    uint64_t costlyFails_ = 0;  // only the expensive kind, for the backoff
    bool faultDisengage_ = false;  // this Disengage is a fault, not a decision
    Tray tray_;
    int64_t lastTrayStatus_ = 0;
};

bool App::ResolveTarget() {
    TargetWindow t{};
    // Explain a miss on the first attempt and every 30th: browsing without a
    // fullscreen video is the normal state now, and a line a second would bury
    // everything else in the log.
    const bool verbose = reengageFails_ == 0 || ((reengageFails_ + 1) % 30) == 0;
    // Settings decide the target unless a --target-* flag does. Google Chrome is the one browser
    // the beta captures (D30); its window class is shared by every Chromium and Electron program,
    // so the process image is what actually selects it.
    if (!opt_.targetFromCli && !settings_.captureChrome) {
        if (verbose) Log("no browser is selected in Settings - nothing to capture");
        return false;
    }
    engageWindowed_ = false;
    // A FULLSCREEN browser first, always: with a second Chrome window in front of it (or just
    // bigger) a single search would pick that one and the fullscreen video would never engage
    // - the pre-P11 rule, kept exactly (review, 2026-10-06). Only when there is none does the
    // windowed path get to look.
    const bool haveFullscreen =
        NeedFullscreen() &&
        FindTarget(opt_.targetClass, opt_.targetTitle, opt_.targetPid, &t, opt_.targetExe, true, false);
    if (!haveFullscreen &&
        !FindTarget(opt_.targetClass, opt_.targetTitle, opt_.targetPid, &t, opt_.targetExe,
                    NeedFullscreen() && !WindowedAuto(), verbose)) {
        if (!verbose) return false;
        if (NeedFullscreen()) {
            Log("waiting for a fullscreen browser window (press F on the video, or F11); "
                "--allow-windowed attaches to a windowed one");
        } else {
            LogErr("no window matched class='%s' exe='%s' title='%s' pid=%lu",
                   Narrow(opt_.targetClass).c_str(), Narrow(opt_.targetExe).c_str(),
                   Narrow(opt_.targetTitle).c_str(), opt_.targetPid);
        }
        return false;
    }
    if (t.fullscreen && !MediaGateOpen(QpcNow())) {
        if (verbose) Log("browser is fullscreen but no media session is playing - not treated as video");
        return false;
    }
    if (WindowedAuto() && !t.fullscreen) {
        // A windowed browser: only on the rect the scout found in THIS window, as it stood
        // then. Anything else - moved, resized, another window, a find gone stale - and the
        // scout looks again; the whole client is never taken for the video (D27, I10).
        const bool fresh = haveAutoRect_ && autoTarget_.hwnd == t.hwnd &&
                           memcmp(&autoTarget_.clientScreen, &t.clientScreen, sizeof(RECT)) == 0 &&
                           memcmp(&autoTarget_.frameBounds, &t.frameBounds, sizeof(RECT)) == 0 &&
                           QpcToSec(QpcNow() - autoRectQpc_) < 3.0;
        if (!fresh) {
            haveAutoRect_ = false;
            if (verbose)
                Log("browser is windowed - waiting for a video playing in it (or fullscreen)");
            return false;
        }
        target_ = t;
        const LONG sx = t.frameBounds.left + autoSurface_.left - t.clientScreen.left;
        const LONG sy = t.frameBounds.top + autoSurface_.top - t.clientScreen.top;
        videoClient_ = RECT{sx, sy, sx + RectW(autoSurface_), sy + RectH(autoSurface_)};
        engageWindowed_ = true;
    } else if (!opt_.haveRect) {
        target_ = t;
        // Whole client area. For fullscreen video that IS the video rect.
        videoClient_ = RECT{0, 0, RectW(t.clientScreen), RectH(t.clientScreen)};
    } else if (opt_.rectIsScreen) {
        target_ = t;
        videoClient_ = RECT{opt_.rectClient.left - t.clientScreen.left,
                            opt_.rectClient.top - t.clientScreen.top,
                            opt_.rectClient.right - t.clientScreen.left,
                            opt_.rectClient.bottom - t.clientScreen.top};
    } else {
        target_ = t;
        videoClient_ = opt_.rectClient;
    }

    // Clamp to the client area: a visual hanging off the window would be clipped
    // by DComp anyway, and the capture crop would be short.
    videoClient_.left = (std::max)(0L, videoClient_.left);
    videoClient_.top = (std::max)(0L, videoClient_.top);
    videoClient_.right = (std::min)(RectW(t.clientScreen), videoClient_.right);
    videoClient_.bottom = (std::min)(RectH(t.clientScreen), videoClient_.bottom);
    if (RectEmpty(videoClient_)) {
        LogErr("video rect is empty after clamping to the client area");
        return false;
    }
    return true;
}

bool App::Engage() {
    Disengage();
    if (!ResolveTarget()) return false;

    const RECT videoScreen = RECT{target_.clientScreen.left + videoClient_.left,
                                  target_.clientScreen.top + videoClient_.top,
                                  target_.clientScreen.left + videoClient_.right,
                                  target_.clientScreen.top + videoClient_.bottom};

    // Capture space: the origin of a WGC window item is the DWM extended frame
    // bounds, not GetWindowRect.
    const RECT srcRect = RECT{videoScreen.left - target_.frameBounds.left + nudgeX_,
                              videoScreen.top - target_.frameBounds.top + nudgeY_,
                              videoScreen.right - target_.frameBounds.left + nudgeX_,
                              videoScreen.bottom - target_.frameBounds.top + nudgeY_};

    bool hzExact = false;
    targetHz_ = MonitorRefreshHz(videoScreen, &hzExact);
    desktopMaxHz_ = DesktopMaxRefreshHz();
    // The compositor clock is desktop-wide and ticks at the FASTEST output, so
    // gating is only needed when this window sits on a slower one. Gating on the
    // fastest output would fight the clock and beat against it — measured: it
    // cost ~20 fps for nothing.
    rateGate_ = targetHz_ > 1.0 && targetHz_ < desktopMaxHz_ - 1.0;
    nextPresentQpc_ = 0;

    Log("target: hwnd=0x%p pid=%lu \"%s\" (%s)", static_cast<void*>(target_.hwnd), target_.pid,
        Narrow(target_.exe).c_str(), Narrow(target_.title).c_str());
    Log("  output: %.3f Hz%s (desktop max %.3f Hz) - generation %s", targetHz_,
        hzExact ? "" : " (rounded)", desktopMaxHz_,
        rateGate_ ? "is paced down to this monitor" : "follows the compositor clock");
    Log("  client %s  frame bounds %s", RectStr(target_.clientScreen).c_str(),
        RectStr(target_.frameBounds).c_str());
    Log("  video rect: client %s -> screen %s -> capture %s", RectStr(videoClient_).c_str(),
        RectStr(videoScreen).c_str(), RectStr(srcRect).c_str());

    overlay_ = std::make_unique<Overlay>();
    std::string err;
    if (!overlay_->Create(target_.hwnd, target_.clientScreen, videoScreen, &err,
                          opt_.overlayTopmost, opt_.overlayLayered)) {
        LogErr("overlay creation failed: %s", err.c_str());
        printf("%s", overlay_->CreationReport().c_str());
        return false;
    }
    printf("%s", overlay_->CreationReport().c_str());
    fflush(stdout);

    // The compositor already paces the fastest output. With only one queued
    // frame, acquiring its buffer waits again and misses refreshes (G88).
    if (!overlay_->SetFrameLatency(rateGate_ ? 1U : 2U)) {
        LogErr("output frame-latency setup failed");
        return false;
    }

    synth_ = std::make_unique<Synth>();
    if (!synth_->Create(overlay_->Device(), overlay_->Context(), &err)) {
        LogErr("synth creation failed: %s", err.c_str());
        return false;
    }
    // The gate must know our adapter (this machine exposes a second render device
    // whose engines would otherwise be counted) and the captured tree, which is
    // exempt by construction so our own overlay can never gate us off (N14).
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(overlay_->Device()->QueryInterface(IID_PPV_ARGS(&dxgiDev))) &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
            gameGate_.SetAdapterLuid(ad.AdapterLuid);
        gameGate_.SetTargetPid(target_.pid);
    }
    drm_.SetTargetPid(target_.pid);
    synth_->SetOcclusionMode(opt_.occMode);
    synth_->SetFieldCohere(opt_.fieldCohere);
    synth_->SetOfaSeedHints(opt_.ofaSeedHints);
    if (!opt_.fieldCohere) Log("field coherence OFF (--no-field-cohere)");
    if (opt_.fieldLab != 0) {
        synth_->SetFieldLab(opt_.fieldLab, opt_.fieldLabParam);
        Log("FIELD LAB mode %d (p %.4f) - not the shipping field", opt_.fieldLab, opt_.fieldLabParam);
    }
    if (opt_.warpLab != 0) {
        if (!synth_->SetWarpLab(opt_.warpLab, &err)) {
            LogErr("warp lab: %s", err.c_str());
            return false;
        }
        synth_->SetWarpLabParam(opt_.warpLabParam);
        Log("WARP LAB mode %d (p %.4f) - PSWarpLab, not the shipping warp", opt_.warpLab,
            opt_.warpLabParam);
    }
    // With the hardware engine the field lands on ITS grid, so the cell size
    // has to follow the flow grid rather than the block-matcher default.
    const UINT cellWanted =
        opt_.useOfa ? static_cast<UINT>(opt_.ofaGrid) : static_cast<UINT>(opt_.cellPx);
    if (!synth_->Resize(static_cast<UINT>(RectW(videoScreen)), static_cast<UINT>(RectH(videoScreen)),
                        &err, cellWanted)) {
        LogErr("synth resize failed: %s", err.c_str());
        return false;
    }
    if (opt_.useOfa) {
        std::string ofaErr;
        if (synth_->EnableOfa(cellWanted, &ofaErr)) {
            printf("--- NVOFA ---\n%s-------------\n", synth_->OfaReport().c_str());
            fflush(stdout);
        } else {
            LogErr("hardware optical flow unavailable (%s) - falling back to the block matcher",
                   ofaErr.c_str());
            printf("--- NVOFA (failed) ---\n%s----------------------\n",
                   synth_->OfaReport().c_str());
            fflush(stdout);
        }
    }
    {
        UINT gw = 0, gh = 0, cell = 0;
        synth_->MotionGrid(&gw, &gh, &cell);
        Log("  motion field: %ux%u cells of %u px, source: %s", gw, gh, cell,
            synth_->OfaActive() ? "NVOFA hardware optical flow" : "block matcher");
    }

    CaptureOptions co;
    co.minUpdateIntervalMs = opt_.minUpdateMs;
    co.tryBorderless = opt_.borderless;
    co.captureCursor = false;  // I12
    co.logDirty = opt_.logDirty;
    co.videoFrameTest = opt_.videoFrameTest;
    capture_ = std::make_unique<Capture>(captureRuntime_);
    if (!capture_->Start(overlay_->Device(), overlay_->Context(), target_.hwnd, srcRect, co, &err)) {
        LogErr("capture start failed: %s", err.c_str());
        printf("%s", capture_->StartupReport().c_str());
        return false;
    }
    printf("--- WGC startup ---\n%s-------------------\n", capture_->StartupReport().c_str());
    fflush(stdout);

    cadence_.Reset();
    timeline_.Reset();
    kOlder_ = kPrev_ = kNewest_ = -1;
    stalled_ = true;
    obscured_ = false;
    lastObscuredCheck_ = 0;  // run the occlusion test before the first show
    zFixesAtStats_ = 0;
    lastIngest100ns_ = 0.0;
    engaged_ = true;
    statsQpc_ = QpcNow();
    presents_ = 0;
    framesAtStats_ = 0;
    guardActive_ = engageWindowed_;
    shownThisEngage_ = false;
    capture_->EnableDamage(guardActive_);
    if (guardActive_) {
        guard_.Begin(Qpc100nsNow() / 1e7, autoSurface_, autoCompanions_, autoSurfW_, autoSurfH_,
                     autoViewport_);
        guardHidLogged_ = false;
        lastPreemptSeen_ = g_inputPreemptQpc.load(std::memory_order_acquire);
        g_inputTarget.store(target_.hwnd, std::memory_order_release);
        Log("windowed video: the overlay follows the geometry guard (surface rect %s, %zu "
            "companion shapes)", RectStr(autoSurface_).c_str(), autoCompanions_.size());
    }
    Log("engaged, mode=%s, overlay hidden until the first source pair arrives", ModeName(opt_.mode));
    return true;
}

void App::Disengage() {
    const bool resetCaptureRuntime = faultDisengage_;
    // A FAULT THAT KILLS A FRESH ENGAGEMENT FEEDS THE BACKOFF. Rebuilding the
    // device, the overlay, the shaders and the capture session once a second for
    // ever is what a persistently invalid DComp device or a swapchain that will
    // not give up a back buffer used to cause: Engage succeeded, Tick killed it
    // half a second later, and the retry clock had just been reset (review,
    // 2026-09-20). An engagement that HELD is not evidence of anything, so it
    // clears the count instead.
    if (engaged_ && faultDisengage_) {
        const bool shortLived =
            engagedAtQpc_ != 0 && QpcToSec(QpcNow() - engagedAtQpc_) < 5.0;
        if (shortLived) {
            engageCostly_ = true;
            ++costlyFails_;
        } else {
            engageCostly_ = false;
            costlyFails_ = 0;
        }
    }
    faultDisengage_ = false;
    engagedAtQpc_ = 0;
    // THE EXEMPTION IS NEVER DROPPED HERE, and the reason is a circular deadlock
    // found in review (2026-09-20). Disengage runs on every geometry change -
    // including the one the user makes constantly, pressing F11 on a video. For
    // the second between that Disengage and the next Engage, Chrome is a foreign
    // FULLSCREEN FOREGROUND window with no exemption, and a fullscreen re-layout
    // is exactly when its GPU load peaks: the gate could arm it, cross 35 % for
    // two samples, and publish "paused". gamePaused_ then blocks the re-engage
    // that is the only thing which would restore the exemption, and the resume
    // rule never fires because fullscreen video keeps the load above 12 %. The
    // result was frame generation dead until restart, with the tray saying a
    // game was running and naming chrome.exe. Keeping the last target exempt
    // while disengaged costs nothing and fails OPEN, which is the gate's first
    // contract. The pid is replaced by the next Engage.
    if (overlay_) overlay_->Hide();
    // The page was not watched while disengaged: whatever rect the scout found is no
    // longer known to be where the video is.
    if (engaged_ && guardActive_) {
        haveAutoRect_ = false;
        scoutSearchQpc_ = QpcNow();  // look again at once, not on the cost duty cycle
    }
    guardActive_ = false;
    g_inputTarget.store(nullptr, std::memory_order_release);
    zFixesAtStats_ = 0;
    framesAtStats_ = 0;
    capture_.reset();
    synth_.reset();
    overlay_.reset();
    if (engaged_) Log("pipeline disengaged");
    engaged_ = false;
    // A device/DComp fault also needs fresh broker bindings, not a cached item.
    if (resetCaptureRuntime) captureRuntime_ = std::make_shared<CaptureRuntime>();
}

// Called several times per compositor tick. A source frame is only noticed when
// it is polled, so polling once per present would quantise the interpolation
// phase to a whole 6.06 ms refresh — which showed up directly as ~11 % of output
// frames sitting on a clamped phase.
void App::PollCapture() {
    if (!engaged_) return;
    const int64_t pollStart = QpcNow();
    const int ingested = capture_->Poll();
    captureCpu_.Add(pollStart);
    if (guardActive_) GuardCapture();
    if (ingested > 0) {
        const double now100 = Qpc100nsNow();
        cadence_.OnFrame(capture_->NewestTime100ns(), now100);
        lastIngest100ns_ = now100;

        const double period = cadence_.Period100ns();
        const int64_t k = timeline_.Add(capture_->NewestTime100ns(), period > 0.0 ? period : 416667.0);
        if (ingested == 1 && k > kNewest_ && kNewest_ >= 0) {
            kOlder_ = kPrev_;
            kPrev_ = kNewest_;
        } else {
            // Two frames in one poll, or the timeline restarted: the history no
            // longer lines up with the ring, so there is no older pair for now.
            kOlder_ = -1;
            kPrev_ = kNewest_ >= 0 && k > kNewest_ ? k - 1 : -1;
        }
        kNewest_ = k;

        // Motion is estimated ONCE per source pair (I6). Everything the output
        // rate multiplies — the warp — is downstream of this call.
        if (opt_.mode == Mode::Mc && capture_->PreviousSrv() && synth_) {
            MotionGpuTrace gpu;
            if (!presentTracePath_.empty() && motionGpuTrace_.size() < 10000) {
                gpu.serial = traceMotionSerial_ + 1;
                gpu.context = overlay_->Context();
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                HRESULT hr = overlay_->Device()->CreateQuery(&desc, &gpu.disjoint);
                desc.Query = D3D11_QUERY_TIMESTAMP;
                if (SUCCEEDED(hr)) hr = overlay_->Device()->CreateQuery(&desc, &gpu.begin);
                if (SUCCEEDED(hr)) hr = overlay_->Device()->CreateQuery(&desc, &gpu.end);
                if (FAILED(hr)) {
                    LogErr("motion GPU trace query creation failed: %s", HrString(hr).c_str());
                    traceInitFailed_ = true;
                    gpu = {};
                } else {
                    gpu.context->Begin(gpu.disjoint.Get());
                    gpu.context->End(gpu.begin.Get());
                }
            }
            const int64_t t0 = QpcNow();
            // The pair being replaced is the pacing's older pair: keep its field.
            if (ingested == 1 && capture_->OlderSrv()) {
                synth_->HoldFields();
            } else {
                synth_->DropHeldFields();
            }
            synth_->PrepareMotion(capture_->PreviousSrv(), capture_->NewestSrv());
            if (gpu.context) {
                gpu.context->End(gpu.end.Get());
                gpu.context->End(gpu.disjoint.Get());
                motionGpuTrace_.push_back(std::move(gpu));
            }
            motionCpu_.Add(t0);
            if (!presentTracePath_.empty()) {
                ++traceMotionSerial_;
                traceMotionDone_ = QpcNow();
                traceMotionMs_ = QpcToMs(traceMotionDone_ - t0);
            }
        }
    }
}

void App::StopScout() { scout_.reset(); }

// An engagement that started on an explicit ON or before the first media poll must still be
// backed by a playing session once that grace ends. A proven one survives a pause, whose
// return must stay instant, but not a page that stopped being a video long ago.
void App::CheckMediaGate(int64_t now) {
    if (!engaged_) {
        mediaProven_ = false;
        mediaIdleSinceQpc_ = 0;
        return;
    }
    if (media_.State() == MediaState::kPlaying) mediaProven_ = true;
    if (MediaGateOpen(now)) {
        mediaIdleSinceQpc_ = 0;
        return;
    }
    if (mediaIdleSinceQpc_ == 0) mediaIdleSinceQpc_ = now;
    if (QpcToSec(now - mediaIdleSinceQpc_) > (mediaProven_ ? 30.0 : 0.0)) {
        Log("no media session is playing (%s) - disengaging", mediaProven_ ? "after a long pause" : "never was");
        Disengage();
    }
}

void App::ToggleGeneration(const char* source) {
    wantShown_ = !wantShown_;
    if (!wantShown_ && overlay_) overlay_->Hide();
    if (wantShown_) {
        // An explicit request must not sit behind an old failed search's cooldown
        // or depend on Chrome publishing a system media session for this video.
        const int64_t now = QpcNow();
        manualScoutUntil_ = now + 20 * QpcPerSecond();
        scoutSearchQpc_ = now;
        rescoutHoldUntil_ = 0;
        shortGiveUps_ = 0;
        scoutFailQpc_ = 0;
        engageNow_ = true;
    }
    tray_.SetEngineOn(wantShown_);
    lastTrayStatus_ = 0;
    Log("frame generation %s (%s)", wantShown_ ? "ON" : "OFF", source);
}

// Chrome's page area: the largest visible Chrome_RenderWidgetHostHWND under the window.
HWND FindRenderWidget(HWND top) {
    struct Ctx {
        HWND best = nullptr;
        long long area = 0;
    } ctx;
    EnumChildWindows(
        top,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64]{};
            GetClassNameW(h, cls, 64);
            RECT r{};
            if (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0 && IsWindowVisible(h) &&
                GetWindowRect(h, &r)) {
                const long long a = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
                if (a > c->area) {
                    c->area = a;
                    c->best = h;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// P11. Runs every loop iteration; costs nothing unless a windowed Chrome is playing media.
void App::Scout(int64_t now) {
    // NOT gated on the DRM verdict: the DRM scanner reaches a clear verdict only for an
    // engaged target, so gating the search on it would deadlock exactly like the game gate's
    // exemption once did. The DRM gate still decides whether a picture is SHOWN.
    const bool want = WindowedAuto() && !engaged_ && wantShown_ && !gamePaused_ &&
                      !watch_.MoveSizeActive() && !haveAutoRect_ &&
                      (opt_.targetFromCli || settings_.captureChrome) &&
                      MediaGateOpen(now) &&
                      (scoutFailQpc_ == 0 || QpcToSec(now - scoutFailQpc_) > 10.0) &&
                      now >= rescoutHoldUntil_;
    if (!want) {
        if (scout_) StopScout();
        return;
    }
    if (QpcToSec(now - scoutLookQpc_) > 0.25) {
        scoutLookQpc_ = now;
        // A fullscreen browser anywhere is the ordinary path's: never scout beside it.
        TargetWindow t{};
        if (FindTarget(opt_.targetClass, opt_.targetTitle, opt_.targetPid, &t, opt_.targetExe,
                       true, false) ||
            !FindTarget(opt_.targetClass, opt_.targetTitle, opt_.targetPid, &t, opt_.targetExe,
                        false, false) ||
            IsIconic(t.hwnd)) {
            if (scout_) StopScout();
            return;
        }
        // The page area, re-read every lookup: bookmarks, an infobar or devtools change it
        // without moving the window. Without it the finder cannot tell the whole page from a
        // video, so no page area means no search (fail closed, I10).
        RECT vp{};
        if (const HWND rw = FindRenderWidget(t.hwnd)) {
            RECT r{};
            if (GetWindowRect(rw, &r))
                vp = RECT{r.left - t.frameBounds.left, r.top - t.frameBounds.top,
                          r.right - t.frameBounds.left, r.bottom - t.frameBounds.top};
        }
        if (RectEmpty(vp)) {
            if (!scoutNoViewportLogged_)
                LogErr("windowed video: no page area (Chrome_RenderWidgetHostHWND) in 0x%p - not "
                       "looking for a video in it", static_cast<void*>(t.hwnd));
            scoutNoViewportLogged_ = true;
            if (scout_) StopScout();
            return;
        }
        scoutNoViewportLogged_ = false;
        // COST. Scouting is a capture of the whole window for as long as Chrome says it plays,
        // and background music says that for hours. After 20 s without a find it looks for 5 s
        // in every 20; a new page in the window (its title changes) starts it over.
        if (t.hwnd != scoutTarget_.hwnd || t.title != scoutTarget_.title) scoutSearchQpc_ = now;
        const double searching = QpcToSec(now - scoutSearchQpc_);
        if (searching > 20.0 && fmod(searching - 20.0, 20.0) > 5.0) {
            if (scout_) StopScout();
            scoutTarget_ = t;  // keep the title, so the duty cycle is not reset
            return;
        }
        const bool same = scout_ && scout_->Target() == t.hwnd &&
                          memcmp(&scoutTarget_.clientScreen, &t.clientScreen, sizeof(RECT)) == 0 &&
                          memcmp(&scoutTarget_.frameBounds, &t.frameBounds, sizeof(RECT)) == 0;
        if (!same) {
            StopScout();
            scout_ = std::make_unique<DamageScout>(captureRuntime_);
            std::string err;
            if (!scout_->Start(t.hwnd, &err)) {
                LogErr("windowed video: cannot watch the browser window (%s) - retrying in 10 s",
                       err.c_str());
                scout_.reset();
                scoutFailQpc_ = now;
                return;
            }
            scoutFailQpc_ = 0;
            scoutStartQpc_ = now;
            const double aspect = autoTarget_.hwnd == t.hwnd && autoTarget_.pid == t.pid && autoTarget_.title == t.title &&
                                  RectH(autoSurface_) > 0 ?
                static_cast<double>(RectW(autoSurface_)) / RectH(autoSurface_) : 0.0;
            const bool sameFrame = memcmp(&autoTarget_.frameBounds, &t.frameBounds, sizeof(RECT)) == 0;
            finder_.Reset(scout_->SurfaceW(), scout_->SurfaceH(), vp, aspect, aspect > 0.0 && sameFrame ? autoSurface_ : RECT{});
            scoutViewport_ = vp;
            Log("windowed video: looking for the video in 0x%p \"%s\" (media %s, page area %s)",
                static_cast<void*>(t.hwnd), Narrow(t.title).c_str(), MediaStateName(media_.State()),
                RectStr(vp).c_str());
        } else if (memcmp(&vp, &scoutViewport_, sizeof(RECT)) != 0 || t.title != scoutTarget_.title) {
            const double aspect = autoTarget_.hwnd == t.hwnd && autoTarget_.pid == t.pid && autoTarget_.title == t.title &&
                                  RectH(autoSurface_) > 0 ?
                static_cast<double>(RectW(autoSurface_)) / RectH(autoSurface_) : 0.0;
            const bool sameFrame = memcmp(&autoTarget_.frameBounds, &t.frameBounds, sizeof(RECT)) == 0;
            finder_.Reset(scout_->SurfaceW(), scout_->SurfaceH(), vp, aspect, aspect > 0.0 && sameFrame ? autoSurface_ : RECT{});
            scoutViewport_ = vp;
        }
        scoutTarget_ = t;
    }
    if (!scout_) return;
    damageBuf_.clear();
    if (!scout_->Poll(&damageBuf_)) {
        StopScout();  // closed or resized: the next lookup starts over
        return;
    }
    for (const auto& d : damageBuf_) finder_.Add(d.tSec, d.d);
    damageBuf_.clear();
    if (QpcToSec(now - scoutLogQpc_) > 5.0 && QpcToSec(now - scoutStartQpc_) > 2.0) {
        scoutLogQpc_ = now;
        Log("windowed video: still looking - last second: %s",
            finder_.Summary(Qpc100nsNow() / 1e7).c_str());
    }
    // Ten decisions a second are plenty for a rule that needs half a second of evidence.
    if (QpcToSec(now - scoutDecideQpc_) < 0.1) return;
    scoutDecideQpc_ = now;
    RECT r{};
    std::vector<DamageSig> comps;
    if (finder_.Found(Qpc100nsNow() / 1e7, &r, &comps)) {
        haveAutoRect_ = true;
        autoRectQpc_ = now;
        autoTarget_ = scoutTarget_;
        autoSurface_ = r;
        autoCompanions_ = comps;
        autoSurfW_ = scout_->SurfaceW();
        autoSurfH_ = scout_->SurfaceH();
        autoViewport_ = scoutViewport_;
        Log("windowed video found after %.1f s: surface rect %s (%ldx%ld), %zu companion shapes",
            QpcToSec(now - scoutStartQpc_), RectStr(r).c_str(), RectW(r), RectH(r), comps.size());
        Log("  last second: %s", finder_.Summary(Qpc100nsNow() / 1e7).c_str());
        StopScout();
        // Straight to the engage, unless engaging has been failing: the backoff stands.
        if (!engageCostly_) engageNow_ = true;
    }
}

// P11. Every capture's damage, and any input that can scroll, through the guard.
void App::GuardCapture() {
    capture_->TakeDamage(&damageBuf_);
    for (const auto& d : damageBuf_) guard_.Add(d.tSec, d.d);
    damageBuf_.clear();
    const int64_t p = g_inputPreemptQpc.load(std::memory_order_acquire);
    if (p != lastPreemptSeen_) {
        lastPreemptSeen_ = p;
        guard_.OnInput(QpcToSec(p));
    }
    const double nowSec = Qpc100nsNow() / 1e7;
    guard_.Tick(nowSec);
    if (guard_.Suspect()) {
        if (overlay_ && overlay_->IsShown()) {
            overlay_->Hide();
            ++guardHides_;
        }
        if (!guardHidLogged_) {
            guardHidLogged_ = true;
            Log("windowed video: %s (last foreign damage %s) - overlay hidden until the video "
                "rect is seen again", guard_.LastReason(), guard_.LastForeign().Str().c_str());
        }
    } else if (guardHidLogged_) {
        guardHidLogged_ = false;
        Log("windowed video: the video rect is confirmed again");
    }
    if (guard_.GiveUp(nowSec) && !guardWantsRescout_) {
        ++guardGiveUps_;
        guardWantsRescout_ = true;
        Log("windowed video: the video rect did not come back - looking for the video again");
    }
}

void App::Tick() {
    if (!engaged_) return;

    const int64_t traceStart = presentTracePath_.empty() ? 0 : QpcNow();
    PollCapture();
    const double tracePollMs = traceStart ? QpcToMs(QpcNow() - traceStart) : 0.0;
    const double now100 = Qpc100nsNow();

    if (capture_->NeedsReengage()) {
        Log("capture asked for a re-engage");
        faultDisengage_ = true;
        Disengage();
        return;
    }

    const bool haveNew = capture_->NewestSrv() != nullptr;
    const bool havePair = capture_->PreviousSrv() != nullptr;
    if (!haveNew) return;

    // Stall = the source stopped producing frames (paused video, or the page is
    // simply static). Fail closed: hide rather than hold a stale picture over a
    // window that may have scrolled underneath (I10).
    //
    // "Producing frames" counts a still shot. Since UI-only recompositions stopped
    // advancing the pair, a still shot — or animation held on one drawing — also
    // stops advancing it, and measured on a <video> alternating 2 s of motion with
    // 1.5 s of stillness the overlay hid three times in 14 s, each hide and re-show
    // a visible jump between our picture (a period late) and the browser's. The
    // browser still repaints the picture for every frame of a still shot, and that
    // is what keeps the source alive.
    const double lastAlive = (std::max)(lastIngest100ns_, capture_->LastPictureActivity100ns());
    const double sinceIngestMs = (now100 - lastAlive) / 10000.0;
    const bool stalledNow = lastAlive == 0.0 || sinceIngestMs > opt_.stallMs;
    if (stalledNow != stalled_) {
        stalled_ = stalledNow;
        if (stalled_ && overlay_->IsShown()) {
            overlay_->Hide();
            Log("source stalled (%.0f ms without a frame) - overlay hidden", sinceIngestMs);
        }
    }

    // A rate-gated output is scheduled on this monitor's refresh, even though
    // Tick() wakes on the desktop compositor clock. Evaluate its phase at the
    // scheduled output instant so source motion advances evenly between the
    // presents that will actually be shown (P25). The ungated path keeps using
    // the current QPC time as before.
    double display100 = now100;
    if (rateGate_) {
        const int64_t nowQpc = QpcNow();
        const int64_t gatePeriod = static_cast<int64_t>(QpcPerSecond() / targetHz_);
        if (nextPresentQpc_ == 0) {
            nextPresentQpc_ = nowQpc;
            gateLastTickQpc_ = 0;
            gateSkips_ = gateResyncs_ = gateIntervals_ = gatePhaseSamples_ = 0;
            gateIntervalSumMs_ = gateIntervalMinMs_ = gateIntervalMaxMs_ = 0.0;
            gatePhaseAbsSumMs_ = gatePhaseMinMs_ = gatePhaseMaxMs_ = 0.0;
        } else if (nowQpc < nextPresentQpc_ - gatePeriod / 4) {
            ++gateSkips_;
            return;  // too early for this output: capture was polled, present is not due
        }

        int64_t displayQpc = nextPresentQpc_;
        nextPresentQpc_ += gatePeriod;
        // Never accumulate a backlog: after a stall, restart from now and use
        // that same instant for the frame being rendered now.
        if (nextPresentQpc_ < nowQpc - gatePeriod) {
            ++gateResyncs_;
            displayQpc = nowQpc;
            nextPresentQpc_ = nowQpc + gatePeriod;
        }
        if (gateLastTickQpc_ != 0) {
            const double intervalMs = QpcToMs(nowQpc - gateLastTickQpc_);
            if (intervalMs > 0.0 && intervalMs < 100.0) {
                if (gateIntervals_ == 0) gateIntervalMinMs_ = gateIntervalMaxMs_ = intervalMs;
                gateIntervalSumMs_ += intervalMs;
                gateIntervalMinMs_ = (std::min)(gateIntervalMinMs_, intervalMs);
                gateIntervalMaxMs_ = (std::max)(gateIntervalMaxMs_, intervalMs);
                ++gateIntervals_;
            }
        }
        gateLastTickQpc_ = nowQpc;
        const double scheduleOffsetMs = QpcToMs(nowQpc - displayQpc);
        if (gatePhaseSamples_ == 0) gatePhaseMinMs_ = gatePhaseMaxMs_ = scheduleOffsetMs;
        gatePhaseAbsSumMs_ += fabs(scheduleOffsetMs);
        gatePhaseMinMs_ = (std::min)(gatePhaseMinMs_, scheduleOffsetMs);
        gatePhaseMaxMs_ = (std::max)(gatePhaseMaxMs_, scheduleOffsetMs);
        ++gatePhaseSamples_;
        display100 = static_cast<double>(displayQpc) * 10000000.0 /
                     static_cast<double>(QpcPerSecond());
    }

    // ---- phase
    // Display lag L = one source period + the capture pipeline delay, so that the
    // instant a frame arrives the phase is 0 (showing the PREVIOUS frame) and it
    // reaches 1 exactly as the next frame lands. Getting L wrong does not add
    // ghosting, it adds judder: the content time would jump at every arrival.
    const double period = cadence_.Period100ns();
    const double delay = cadence_.Delay100ns();

    // Nothing to interpolate when the source already arrives about as often as
    // we present. Chrome does exactly this for a while after a window is
    // un-occluded or interacted with: every refresh carries a new composited
    // frame, the estimator reads "165 fps", and interpolating between two
    // near-identical frames is work with no output. Observed live, and it is
    // the same class of error I8 exists for. Threshold: a source period under
    // 1.5 output ticks.
    // THE OUTPUT PERIOD IS KNOWN, NOT MEASURED. This used to divide by the
    // measured present rate, which is 0 for the first half second of every
    // engage and then feeds back on itself: with the fallback standing in for
    // 165 Hz, a 60 fps source on the 60 Hz secondary was interpolated until the
    // measurement caught up, then stopped, then started again (beta audit,
    // 2026-09-20). The refresh of the monitor the video is on is read at Engage
    // and re-read whenever the window moves or the mode changes (I14), so it is
    // right from the first tick and cannot oscillate.
    const double outHz = rateGate_ ? targetHz_ : desktopMaxHz_;
    const double outTick = outHz > 1.0 ? 10000000.0 / outHz : 60600.0;
    const bool sourceIsFast = period > 0.0 && period < outTick * 1.5;
    const bool usePair =
        havePair && period > 0.0 && !sourceIsFast && opt_.mode != Mode::Passthrough;
    float t = 1.0f;
    // GAP REPAIR. When the pair spans much more than one period — the first video
    // frame after a still shot, or a frame the source dropped — the phase measured
    // from the older frame is already past 1 when the newer one lands, so the
    // picture holds for a period and then jumps. Measuring from one period before
    // the newer frame instead starts the pair at the picture already on screen and
    // moves through it at the source rate.
    double tA = capture_->PreviousTime100ns();
    double tB = capture_->NewestTime100ns();
    ID3D11ShaderResourceView* srcA = capture_->PreviousSrv();
    ID3D11ShaderResourceView* srcB = capture_->NewestSrv();
    bool olderPair = false;
    double lag = period + delay;
    if (usePair && timeline_.Ready() && kPrev_ >= 0 && kNewest_ > kPrev_) {
        // PACING: see Timeline. The display lag carries the jitter margin, and the
        // pair is whichever of the two in hand brackets the moment on screen.
        const double P = timeline_.Period100ns();
        lag = P + delay + timeline_.Margin100ns();
        const double shown = display100 - lag;
        tA = timeline_.TimeOf(kPrev_);
        tB = timeline_.TimeOf(kNewest_);
        if (shown < tA && kOlder_ >= 0 && kOlder_ < kPrev_ && capture_->OlderSrv() &&
            (opt_.mode != Mode::Mc || synth_->HasHeldFields())) {
            olderPair = true;
            tB = tA;
            tA = timeline_.TimeOf(kOlder_);
            srcA = capture_->OlderSrv();
            srcB = capture_->PreviousSrv();
        }
        if (tB - tA > 1.6 * P) tA = tB - P;
    } else if (havePair && period > 0.0 && tB - tA > 1.6 * period) {
        tA = tB - period;
    }
    double uRaw = 0.0;
    if (usePair) {
        const double span = tB - tA > 0.0 ? tB - tA : period;
        const double u = (display100 - lag - tA) / span;
        uRaw = u;
        ++pairTicks_;
        if (olderPair) ++olderPairTicks_;
        if (u <= 0.0) {
            t = 0.0f;
            ++held_;
        } else if (u >= 1.0) {
            t = 1.0f;
            ++held_;
        } else {
            t = static_cast<float>(u);
        }
        // Only sane values enter the reported range: during a stall u grows
        // without bound (it is clamped for rendering, not for reporting) and a
        // single stall used to leave "phase 0.00..15564.59" in the log.
        if (u > -4.0 && u < 5.0) {
            uMin_ = (std::min)(uMin_, u);
            uMax_ = (std::max)(uMax_, u);
        }
    }

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    const int64_t acquireStart = QpcNow();
    const bool acquired = overlay_->AcquireBackBuffer(&back, &rtv);
    acquireCpu_.Add(acquireStart);
    if (!acquired) {
        // A back buffer we cannot get is a lost device, not a skipped frame.
        // This used to `return`, which left the engine "engaged" for ever after a
        // TDR: every tick failed here, nothing re-engaged, and the last picture
        // stayed frozen over live video until the user quit (beta audit,
        // 2026-09-20). Present() already treats its own failure this way.
        LogErr("back buffer unavailable - the device is lost, re-engaging");
        faultDisengage_ = true;
        Disengage();
        return;
    }

    const int64_t drawStart = QpcNow();
    if (usePair) {
        const UINT vw = static_cast<UINT>(RectW(overlay_->VideoRect()));
        const UINT vh = static_cast<UINT>(RectH(overlay_->VideoRect()));
        if (opt_.mode == Mode::Mc) {
            synth_->Warp(rtv, vw, vh, srcA, srcB, t, olderPair);
        } else {
            synth_->Blend(rtv, vw, vh, srcA, srcB, t);
        }
    } else {
        // Passthrough: a bit-exact copy, so any visible difference from the window
        // underneath is a geometry or colour bug, not a synthesis artifact.
        overlay_->Context()->CopyResource(back, capture_->NewestTex());
    }

    if (opt_.badge && overlay_->IsShown()) {
        const UINT vw = static_cast<UINT>(RectW(overlay_->VideoRect()));
        const UINT vh = static_cast<UINT>(RectH(overlay_->VideoRect()));
        // Fixed: the top-right corner of the video rect, 12 px in. The size
        // follows the picture so it reads the same on a 1080p secondary and a
        // 1440p primary, and it is deliberately small — the readout is a check,
        // not a feature of the picture (owner, 2026-09-20). MEASURED with Segoe
        // UI Semibold: 1080p -> 13 px font -> 9 px digits, 1440p -> 16 -> 11,
        // 2160p -> the 20 px cap -> 14. The pixel font it replaced was 21 px
        // tall at 1440p, so this is about half.
        const int fontPx = (std::min)(20, (std::max)(13, static_cast<int>(vh) / 90));
        synth_->DrawFpsText(rtv, vw, vh, static_cast<float>(vw) - 12.0f, 12.0f, fontPx, FpsText());
    }

    drawCpu_.Add(drawStart);
    const int64_t presentStart = QpcNow();
    const auto present = overlay_->Present();
    presentCpu_.Add(presentStart);
    if (present == Overlay::PresentResult::DeviceLost) {
        LogErr("device lost on present - re-engaging");
        faultDisengage_ = true;
        Disengage();
        return;
    }
    if (present == Overlay::PresentResult::Skipped) {
        // Do not count a rejected submission or show an unpresented first frame.
        // Reset the judder baseline: this output instant never reached DXGI.
        judderLastWall100ns_ = 0.0;
        return;
    }
    ++presents_;
    ++badgeFrames_;
    if (!presentTracePath_.empty() && !presentTraceFull_) {
        if (presentTrace_.size() < 60000) {
            presentTrace_.push_back({QpcNow(), tA, tB, t, tracePollMs, traceMotionMs_, traceMotionSerial_,
                                     traceMotionDone_, kPrev_, kNewest_, olderPair, usePair,
                                     overlay_->TracePresentation()});
        } else {
            presentTraceFull_ = true;
            LogErr("presentation trace reached 60000 rows; further samples omitted");
        }
    }

    if (overlay_->IsShown()) {
        const double content = usePair ? tA + t * (tB - tA) : capture_->NewestTime100ns();
        // Not judder: a next video frame more than 60 % of a period overdue means
        // the source is holding still (or stopped), and the picture correctly holds
        // too; and a content jump of several periods is the restart after such a
        // hold, a discontinuity of the TIMESTAMPS, not of what is on screen. Both
        // are counted apart so they cannot drown the number that matters.
        const bool sourceHolding = usePair && uRaw > 1.6;
        if (judderLastWall100ns_ > 0.0 && !sourceHolding) {
            const double dWallMs = (display100 - judderLastWall100ns_) / 10000.0;
            const double err = fabs((content - judderLastContent100ns_) / 10000.0 - dWallMs);
            if (dWallMs > 0.0 && dWallMs < 100.0) {
                if (period > 0.0 && err > 3.0 * period / 10000.0) {
                    ++judderJumps_;
                } else {
                    judderAbsMs_ += err;
                    ++judderN_;
                    if (err > 4.0) ++judderBig_;
                }
            }
        } else if (sourceHolding) {
            ++judderHolding_;
        }
        judderLastContent100ns_ = content;
        judderLastWall100ns_ = display100;
    } else {
        judderLastWall100ns_ = 0.0;
    }

    // The readout has to settle, not flicker: a half-second window, and each
    // number changes only when the measurement is 0.75 away from what is shown.
    // A 23.976 fps source estimated at 23.4-23.8 otherwise alternated "23" and
    // "24" every half second (seen on the first capture of the readout).
    {
        const int64_t now = QpcNow();
        if (badgeFpsQpc_ == 0) badgeFpsQpc_ = now;
        const double sec = QpcToSec(now - badgeFpsQpc_);
        if (sec >= 0.5) {
            outFpsSmooth_ = static_cast<double>(badgeFrames_) / sec;
            badgeFrames_ = 0;
            badgeFpsQpc_ = now;
            const auto settle = [](int* shown, double measured) {
                if (fabs(measured - *shown) > 0.75)
                    *shown = (std::min)(999, (std::max)(0, static_cast<int>(measured + 0.5)));
            };
            settle(&shownSrcFps_, cadence_.Fps());
            settle(&shownOutFps_, outFpsSmooth_);
        }
    }

    // Show only AFTER a real frame has been presented (D13: engage after
    // validate), so the first visible pixel is never an uninitialised buffer.
    // cadence_.Ready() is also the "is anything actually animating" test: a static
    // page delivers the odd repaint a second apart, which is not a source to
    // interpolate and must not make the overlay blink in and out.
    if (opt_.autoShow && wantShown_ && !stalled_ && !obscured_ && !cursorOver_ && !gamePaused_ &&
        !drmBlocked_ && !(guardActive_ && guard_.Suspect()) &&
        !overlay_->IsShown() && havePair &&
        cadence_.Ready()) {
        overlay_->Show();
        shownThisEngage_ = true;
        Log("overlay shown (source %.2f fps, capture delay %.1f ms)", cadence_.Fps(),
            delay / 10000.0);
    }
}

// "24 → 165", the same wording as the tray tooltip. An arrow, not a slash:
// the two numbers are a source and what it becomes, not a fraction.
std::wstring App::FpsText() const {
    return std::to_wstring(shownSrcFps_) + L" → " + std::to_wstring(shownOutFps_);
}

void App::ApplySettings(const Settings& s) {
    const Settings old = settings_;
    settings_ = s;
    settingsUnreadable_ = false;  // whatever the file said, the user has chosen now
    if (old.captureChrome != s.captureChrome)
        Log("settings: Google Chrome %s%s", s.captureChrome ? "selected" : "not selected",
            opt_.targetFromCli ? " (no effect: a --target-* flag decides the target)" : "");
    if (old.showFpsCounter != s.showFpsCounter) {
        if (!opt_.badgeFromCli) opt_.badge = s.showFpsCounter;
        Log("settings: frame rate counter %s%s", s.showFpsCounter ? "on" : "off",
            opt_.badgeFromCli ? " (no effect: --no-badge)" : "");
    }
    if (old.language != s.language)
        Log("settings: language %s", s.language == UiLanguage::kRussian   ? "ru"
                                     : s.language == UiLanguage::kEnglish ? "en"
                                                                          : "auto");
    // A browser that is no longer selected is let go at once, not at the next re-engage.
    if (engaged_ && !opt_.targetFromCli && !settings_.captureChrome) {
        Log("the captured browser is no longer selected - disengaging");
        Disengage();
    }
    lastTrayStatus_ = 0;
}

std::wstring App::TrayStatus() const {
    std::wstring app = L"Chrome";
    if (engaged_ && !target_.exe.empty() && _wcsicmp(target_.exe.c_str(), L"chrome.exe") != 0) {
        app = target_.exe;
    } else if (!engaged_ && opt_.targetFromCli && !opt_.targetExe.empty() &&
               _wcsicmp(opt_.targetExe.c_str(), L"chrome.exe") != 0) {
        app = opt_.targetExe;
    }
    // Truncated, never checked: a process image name can be MAX_PATH long, and swprintf_s ends
    // the process on overflow. The shell cuts the tooltip at 127 characters anyway.
    wchar_t text[128];
    if (!wantShown_) {
        _snwprintf_s(text, _TRUNCATE, L"%ls", Tr(Txt::kStatusOff));
    } else if (!opt_.targetFromCli && settingsUnreadable_) {
        _snwprintf_s(text, _TRUNCATE, L"%ls", Tr(Txt::kStatusSettingsUnreadable));
    } else if (!opt_.targetFromCli && !settings_.captureChrome) {
        _snwprintf_s(text, _TRUNCATE, L"%ls", Tr(Txt::kStatusNothingSelected));
    } else if (drmBlocked_) {
        _snwprintf_s(text, _TRUNCATE, L"%ls", Tr(Txt::kStatusProtected));
    } else if (gamePaused_) {
        // Above the !engaged_ branch on purpose: a gate YIELD disengages us, and
        // "waiting for a video" would be a lie while a game is running.
        _snwprintf_s(text, _TRUNCATE, Tr(Txt::kStatusGamePaused),
                     gameExe_.empty() ? L"?" : gameExe_.c_str());
    } else if (!engaged_) {
        _snwprintf_s(text, _TRUNCATE, Tr(Txt::kStatusWaiting), app.c_str());
    } else if (obscured_) {
        _snwprintf_s(text, _TRUNCATE, L"%ls", Tr(Txt::kStatusCovered));
    } else if (!overlay_ || !overlay_->IsShown()) {
        _snwprintf_s(text, _TRUNCATE, Tr(Txt::kStatusNoVideo), app.c_str());
    } else {
        _snwprintf_s(text, _TRUNCATE, Tr(Txt::kStatusGenerating), shownSrcFps_, shownOutFps_, app.c_str());
    }
    return std::wstring(L"Nova SilkPlay \u2014 ") + text;
}

// Writes the two source frames and what the synthesizer puts between them, so
// the result can be judged off-screen: at t=0.5 a moving edge must be ONE edge
// at the midpoint (motion compensation worked), not two faint ones (a
// cross-fade). Blocking, allocating and slow — it ends the run.
bool App::DoDump() {
    if (!engaged_ || !capture_ || !synth_ || !overlay_) return false;
    ID3D11Texture2D* texA = capture_->PreviousTex();
    ID3D11Texture2D* texB = capture_->NewestTex();
    if (!texA || !texB) return false;

    ID3D11Device* dev = overlay_->Device();
    ID3D11DeviceContext* ctx = overlay_->Context();
    const UINT w = static_cast<UINT>(RectW(overlay_->VideoRect()));
    const UINT h = static_cast<UINT>(RectH(overlay_->VideoRect()));

    // Offscreen target in the same format family as the overlay's back buffer,
    // so what is dumped is what would have been presented.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &rt);
    if (FAILED(hr)) {
        LogErr("dump: offscreen CreateTexture2D failed %s", HrString(hr).c_str());
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    hr = dev->CreateRenderTargetView(rt.Get(), &rd, &rtv);
    if (FAILED(hr)) {
        LogErr("dump: offscreen RTV failed %s", HrString(hr).c_str());
        return false;
    }

    synth_->PrepareMotion(capture_->PreviousSrv(), capture_->NewestSrv());
    {
        float p50 = 0, p95 = 0, mx = 0, zf = 0;
        if (synth_->DebugFieldStats(&p50, &p95, &mx, &zf))
            Log("motion field |v|: p50 %.2f px, p95 %.2f px, max %.2f px, %.0f%% near zero  (%s)",
                p50, p95, mx, zf * 100.0f,
                synth_->OfaActive() ? "NVOFA" : "block matcher");
    }

    const std::string p = opt_.dumpPrefix;
    DumpTextureToPpm(dev, ctx, texA, (p + "_a.ppm").c_str());
    DumpTextureToPpm(dev, ctx, texB, (p + "_b.ppm").c_str());
    // NO readout here: the frames everything is compared against must be
    // unbiased. It has its own dump below.
    synth_->Warp(rtv.Get(), w, h, capture_->PreviousSrv(), capture_->NewestSrv(), 0.5f);
    DumpTextureToPpm(dev, ctx, rt.Get(), (p + "_mc50.ppm").c_str());
    if (opt_.badge) {
        const int dumpFontPx = (std::min)(20, (std::max)(13, static_cast<int>(h) / 90));
        synth_->DrawFpsText(rtv.Get(), w, h, static_cast<float>(w) - 12.0f, 12.0f, dumpFontPx,
                            FpsText());
        DumpTextureToPpm(dev, ctx, rt.Get(), (p + "_fps.ppm").c_str());
    }
    synth_->Blend(rtv.Get(), w, h, capture_->PreviousSrv(), capture_->NewestSrv(), 0.5f);
    DumpTextureToPpm(dev, ctx, rt.Get(), (p + "_blend50.ppm").c_str());
    synth_->Warp(rtv.Get(), w, h, capture_->PreviousSrv(), capture_->NewestSrv(), 0.25f);
    DumpTextureToPpm(dev, ctx, rt.Get(), (p + "_mc25.ppm").c_str());
    return true;
}

void App::LogStats() {
    const int64_t now = QpcNow();
    const double sec = QpcToSec(now - statsQpc_);
    if (sec < opt_.statsEverySec) return;

    const uint64_t frames = capture_ ? capture_->FramesIngested() : 0;
    const double inFps = (frames - framesAtStats_) / sec;
    const double outFps = presents_ / sec;
    const char* mode = "n/a";
    if (overlay_) {
        switch (overlay_->LastCompositionMode()) {
            case 0: mode = "COMPOSED"; break;
            case 1: mode = "OVERLAY"; break;
            case 2: mode = "NONE"; break;
            case 3: mode = "FAILURE"; break;
            default: break;
        }
        const OverlayMediaStats media = overlay_->TakeMediaStats();
        if (media.available && media.samples) {
            Log("  scan-out: DXGI reports %llu shown images across %llu samples | same v-blank %llu, skipped v-blanks %llu, discontinuities %llu | present refresh %u, sync refresh %u",
                static_cast<unsigned long long>(media.framesShown),
                static_cast<unsigned long long>(media.samples),
                static_cast<unsigned long long>(media.sameRefresh),
                static_cast<unsigned long long>(media.skippedRefreshes),
                static_cast<unsigned long long>(media.discontinuities),
                media.lastPresentRefreshCount, media.lastSyncRefreshCount);
        }
    }
    double ilo = 0.0, imid = 0.0, ihi = 0.0;
    cadence_.IntervalSpreadMs(&ilo, &imid, &ihi);
    Log("  arrival intervals: min %.1f med %.1f max %.1f ms | judder: mean %.2f ms, >4 ms on %llu of %llu presents (source holding %llu, restarts %llu)",
        ilo, imid, ihi, judderN_ ? judderAbsMs_ / static_cast<double>(judderN_) : 0.0,
        static_cast<unsigned long long>(judderBig_), static_cast<unsigned long long>(judderN_),
        static_cast<unsigned long long>(judderHolding_), static_cast<unsigned long long>(judderJumps_));
    judderAbsMs_ = 0.0;
    judderN_ = judderBig_ = judderHolding_ = judderJumps_ = 0;
    Log("  pacing: timeline %s, period %.2f ms, jitter margin %.1f ms, older pair on %llu of %llu ticks",
        timeline_.Ready() ? "locked" : "not locked", timeline_.Period100ns() / 10000.0,
        timeline_.Margin100ns() / 10000.0, static_cast<unsigned long long>(olderPairTicks_),
        static_cast<unsigned long long>(pairTicks_));
    if (rateGate_ && gatePhaseSamples_) {
        Log("  rate gate: early loops %llu, resyncs %llu | actual tick interval mean/min/max %.2f/%.2f/%.2f ms | QPC schedule offset mean-abs/min/max %.2f/%.2f/%.2f ms",
            static_cast<unsigned long long>(gateSkips_),
            static_cast<unsigned long long>(gateResyncs_),
            gateIntervals_ ? gateIntervalSumMs_ / gateIntervals_ : 0.0,
            gateIntervals_ ? gateIntervalMinMs_ : 0.0, gateIntervals_ ? gateIntervalMaxMs_ : 0.0,
            gatePhaseAbsSumMs_ / gatePhaseSamples_, gatePhaseMinMs_, gatePhaseMaxMs_);
    }
    gateResyncs_ = gateIntervals_ = gatePhaseSamples_ = 0;
    gateIntervalSumMs_ = gateIntervalMinMs_ = gateIntervalMaxMs_ = 0.0;
    gatePhaseAbsSumMs_ = gatePhaseMinMs_ = gatePhaseMaxMs_ = 0.0;
    olderPairTicks_ = pairTicks_ = 0;
    Log("in %.1f fps (est. source %.2f fps, delay %.1f ms) | out %.1f fps | %s | plane %s | "
        "phase %.2f..%.2f held %llu | loop %.0f/s (clock %.1f + wait %.1f + tick %.1f ms) | %s",
        inFps, cadence_.Fps(), cadence_.Delay100ns() / 10000.0, outFps, ModeName(opt_.mode), mode,
        uMin_ > 1.5 ? 0.0 : uMin_, uMax_ < 0.0 ? 0.0 : uMax_,
        static_cast<unsigned long long>(held_),
        ticks_ / sec, msClock_ / (double)(ticks_ ? ticks_ : 1),
        msWait_ / (double)(ticks_ ? ticks_ : 1), msTick_ / (double)(ticks_ ? ticks_ : 1),
        overlay_ && overlay_->IsShown() ? "SHOWN" : "hidden");
    if (capture_) {
        const CaptureStats cs = capture_->TakeStats();
        Log("  capture: %llu video frames, %llu UI-only refreshes | content tests %llu (answer after avg %.2f ms, max %.2f ms)",
            static_cast<unsigned long long>(frames - framesAtStats_),
            static_cast<unsigned long long>(cs.uiRefreshes),
            static_cast<unsigned long long>(cs.contentTests),
            cs.contentTests ? cs.testWaitMsSum / static_cast<double>(cs.contentTests) : 0.0,
            cs.testWaitMsMax);
    }
    {
        // Disengaged: the counters restarted with the overlay; never subtract
        // across that (it printed 18446744073709551xxx, found in review).
        const uint64_t zNow = overlay_ ? overlay_->ZOrderFixes() : 0;
        const uint64_t zDelta = zNow >= zFixesAtStats_ ? zNow - zFixesAtStats_ : zNow;
        Log("  windows: z-order fixes %llu | occlusion tests %llu (avg %.0f us, max %.0f us) | "
            "window events %llu | hit tests on overlay %u%s%s%s",
            static_cast<unsigned long long>(zDelta),
            static_cast<unsigned long long>(occlChecks_),
            occlChecks_ ? occlUsSum_ / static_cast<double>(occlChecks_) : 0.0, occlUsMax_,
            static_cast<unsigned long long>(watch_.EventsSeen()),
            overlay_ ? overlay_->HitTestsSeen() : 0u, obscured_ ? " | OBSCURED" : "",
            cursorGateTripped_ ? " | CURSOR GATE (tripwire)" : "",
            overlay_ && overlay_->ZOrderDegraded() ? " | Z-ORDER DEGRADED TO TOPMOST" : "");
        zFixesAtStats_ = zNow;
        occlChecks_ = 0;
        occlUsSum_ = occlUsMax_ = 0.0;
    }

    const auto logCpu = [](const char* name, const CpuStage& stage) {
        Log("  CPU stage %s: mean %.3f ms, max %.3f ms, %llu calls (submission, not GPU time)",
            name, stage.runs ? stage.totalMs / static_cast<double>(stage.runs) : 0.0,
            stage.maxMs, static_cast<unsigned long long>(stage.runs));
    };
    logCpu("capture", captureCpu_);
    logCpu("motion", motionCpu_);
    logCpu("acquire", acquireCpu_);
    logCpu("draw", drawCpu_);
    logCpu("present", presentCpu_);
    captureCpu_ = motionCpu_ = acquireCpu_ = drawCpu_ = presentCpu_ = {};
    statsQpc_ = now;
    subTickPolls_ = 0;
    ticks_ = 0;
    gateSkips_ = 0;
    msClock_ = msWait_ = msTick_ = 0;
    presents_ = 0;
    framesAtStats_ = frames;
    uMin_ = 2.0;
    uMax_ = -1.0;
    held_ = 0;
}

int App::Run() {
    if (traceInitFailed_) {
        LogErr("reading NSP_PRESENT_TRACE failed");
        return 1;
    }
    auto waitClock = LoadCompositorClock();
    if (!waitClock)
        LogErr("DCompositionWaitForCompositorClock unavailable - falling back to Sleep(1); "
               "pacing is NOT vblank-locked");

    InstallHotkeys();
    // On THIS thread: out-of-context WinEvents are dispatched by the thread that
    // installed them, from its own message pump, which here is the main loop.
    for (const auto& exe : opt_.occlusionIgnore)
        Log("occlusion: windows of %s never pause generation", Narrow(exe).c_str());
    if (!watch_.Start())
        LogErr("window events unavailable (SetWinEventHook failed) - covering windows are "
               "found by the 10 Hz poll only");
    // P6. FAIL OPEN: a gate that cannot measure must never stop a video (I2), so
    // a refusal here is loud and the run continues generating.
    if (opt_.gameGate) {
        std::string gerr;
        if (!gameGate_.Start(&gerr))
            LogErr("game gate unavailable (%s) - generation will NOT pause for games this run",
                   gerr.c_str());
    } else {
        Log("game gate OFF (--no-game-gate) - generation will not pause for a game");
    }
    if (opt_.drmGate) {
        std::string derr;
        if (!drm_.Start(&derr))
            LogErr("DRM watch unavailable (%s) - generation is BLOCKED, because a watch that "
                   "cannot tell must not let us draw over protected video (I7)",
                   derr.c_str());
    } else {
        Log("DRM gate OFF (--no-drm-gate) - protected video would NOT be skipped");
    }
    {
        std::string trayErr;
        if (!tray_.Create(settings_, settingsPath_, opt_.uiMonitor, &trayErr))
            LogErr("%s - continuing without a tray icon", trayErr.c_str());
        tray_.SetStatus(TrayStatus());
        Log("settings: %s - Google Chrome %s, frame rate counter %s",
            settingsPath_.empty() ? "(not saved this run)" : Narrow(settingsPath_).c_str(),
            settings_.captureChrome ? "on" : "off", opt_.badge ? "on" : "off");
    }
    // A failed FIRST engage is not fatal any more. The loop below already
    // retries once a second, so starting before Chrome exists — or before any
    // video is on screen — just means waiting, which is what a background tool
    // is supposed to do. Exiting here instead was the single thing that made
    // this a "launch it at the right moment" utility rather than something that
    // can sit in the tray.
    if (WindowedAuto()) {
        media_.Start(L"chrome");
        Log("windowed video: on - a video playing in a windowed Chrome is found from DWM's "
            "damage (--fullscreen-only turns this off)");
    }
    if (!Engage())
        Log("no target yet - waiting for a window matching class='%s'; Ctrl+Alt+X quits",
            Narrow(opt_.targetClass).c_str());

    int64_t lastGeomCheck = QpcNow();
    int64_t lastReengageAttempt = 0;
    const int64_t runStartQpc = QpcNow();

    while (!quit_) {
        {
            const int64_t t0 = QpcNow();
            if (waitClock) {
                waitClock(0, nullptr, 100);
            } else {
                Sleep(1);
            }
            msClock_ += QpcToMs(QpcNow() - t0);
        }

        // Capture is polled here as well as inside Tick(): when the rate gate
        // holds a present back, Tick() returns early and would otherwise stop
        // draining WGC.
        //
        // What used to be here — waiting on the swapchain's frame-latency object
        // in 2 ms slices — was REMOVED after it was measured doing the opposite
        // of its purpose. On a monitor slower than the compositor clock the gate
        // skips presents, so nothing retires, so the waitable never signals, so
        // all eight slices time out: 57 ms per iteration, a 16 Hz main loop and
        // 8 presented frames a second on a 60 Hz output. The wait belongs where
        // a present is actually about to happen, not in the polling path.
        PollCapture();

        switch (tray_.Poll()) {
            case Tray::Command::kToggle:
                ToggleGeneration("tray");
                break;
            case Tray::Command::kSettingsChanged:
                ApplySettings(tray_.CurrentSettings());
                break;
            case Tray::Command::kCycleMode:
                opt_.mode = NextMode(opt_.mode);
                Log("mode -> %s (tray)", ModeName(opt_.mode));
                break;
            case Tray::Command::kQuit:
                Log("quit (tray)");
                quit_ = true;
                break;
            default: break;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit_ = true;
                break;
            }
            if (msg.message == WM_HOTKEY || msg.message == WM_NSP_HOTKEY) {
                switch (static_cast<int>(msg.wParam)) {
                    case kHkQuit:
                        Log("Ctrl+Alt+X - quitting");
                        quit_ = true;
                        break;
                    case kHkToggle:
                    case kHkToggle2:
                        ToggleGeneration("Ctrl+Alt+Q/O");
                        break;
                    case kHkMode:
                        opt_.mode = NextMode(opt_.mode);
                        Log("mode -> %s", ModeName(opt_.mode));
                        break;
                    case kHkLeft:
                    case kHkRight:
                    case kHkUp:
                    case kHkDown: {
                        const LONG dx = (msg.wParam == kHkLeft) ? -1 : (msg.wParam == kHkRight ? 1 : 0);
                        const LONG dy = (msg.wParam == kHkUp) ? -1 : (msg.wParam == kHkDown ? 1 : 0);
                        nudgeX_ += dx;
                        nudgeY_ += dy;
                        if (capture_) {
                            const RECT& r = capture_->SourceRect();
                            capture_->SetSourceOffset(r.left + dx, r.top + dy);
                            Log("capture rect nudged to %s (total nudge %+ld,%+ld)",
                                RectStr(capture_->SourceRect()).c_str(), nudgeX_, nudgeY_);
                        }
                        break;
                    }
                    default: break;
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!quit_ && g_quitEvent && WaitForSingleObject(g_quitEvent, 0) == WAIT_OBJECT_0) {
            Log("quit requested through %ls - shutting down", kQuitEventName);
            quit_ = true;
        }
        if (!quit_ && g_consoleQuit.load()) {
            Log("console closed or Ctrl+C - quitting");
            quit_ = true;
        }
        if (quit_) break;

        // Before the present, so the frame lands in the right place in z-order:
        // clicking the browser raises it above us, and this puts us back.
        if (engaged_ && overlay_) overlay_->KeepAbove();

        ++ticks_;
        { const int64_t t0 = QpcNow(); Tick(); msTick_ += QpcToMs(QpcNow() - t0); }

        if (!opt_.dumpPrefix.empty() && engaged_ && overlay_->IsShown() &&
            QpcToSec(QpcNow() - runStartQpc) > opt_.dumpAfterSec) {
            if (DoDump()) {
                Log("dump complete - exiting");
                quit_ = true;
                continue;
            }
        }

        // Geometry watch. The overlay HWND is frozen (I11), so a target that moved
        // or resized invalidates everything: tear down and engage again.
        const int64_t now = QpcNow();

        // THE CLICK-THROUGH TRIPWIRE. The pointer on the video no longer pauses
        // anything: the overlay is WS_EX_LAYERED | WS_EX_TRANSPARENT, which the
        // clickthrough probe measured passing every move, click and wheel notch to
        // the page. What was here before — hiding whenever the pointer was on the
        // video — meant that fullscreen video, where the pointer is ALWAYS on the
        // video, was almost never generated at all.
        //
        // A click-through window never receives WM_NCHITTEST. If ours does while
        // it is shown, input is being swallowed on this machine for a reason the
        // probe did not see, and a player the user cannot operate is worse than
        // an unsmoothed one: fall back to the old gate for the rest of the run
        // and say so loudly. Three hits, not one, so a single stray query from an
        // accessibility tool cannot trip it.
        if (engaged_ && overlay_ && !opt_.cursorGate && !cursorGateTripped_ &&
            overlay_->HitTestsSeen() >= 3) {
            cursorGateTripped_ = true;
            overlay_->Hide();
            LogErr("the overlay received %u mouse hit tests - it is NOT click-through here, so "
                   "clicks were being swallowed. Falling back to pausing while the pointer is "
                   "on the video (as --cursor-gate).",
                   overlay_->HitTestsSeen());
        }
        if (engaged_ && overlay_ && (opt_.cursorGate || cursorGateTripped_)) {
            POINT pt{};
            const RECT vr = overlay_->VideoRect();
            const bool inside = GetCursorPos(&pt) && PtInRect(&vr, pt);
            if (inside) {
                cursorLeftAt_ = now;
                if (!cursorOver_) {
                    cursorOver_ = true;
                    overlay_->Hide();
                    Log("pointer is on the video - generation paused (cursor gate)");
                }
            } else if (cursorOver_ && QpcToSec(now - cursorLeftAt_) > 0.5) {
                cursorOver_ = false;
                Log("pointer left the video - generation resumes");
            }
        }

        // A foreground change is what the gate most wants to know about, and the
        // watcher already sees it. Its own flag, never the occlusion test's: a
        // shared one would let whichever consumer polled first swallow the
        // other's notification.
        if (opt_.gameGate && watch_.TakeForegroundChanged()) gameGate_.NudgeForeground();

        // THE DRM GATE (P11, I7). It decides whether a PICTURE may be shown, never
        // whether we may engage - gating the engage would deadlock exactly the way
        // the game gate's exemption did, because the scanner needs a target pid to
        // reach a clear verdict and only Engage gives it one.
        //
        // FAILS CLOSED, the opposite of the game gate: kUnknown counts as
        // protected. Generating over content somebody encrypted is the failure
        // this exists to prevent; refusing to smooth one page is not.
        //
        // WHAT IS WIRED, and what is not: only the process scan. A content
        // decryption module running anywhere in the browser's tree blocks
        // generation for that browser. That is deliberately coarse - a Netflix tab
        // left open in the BACKGROUND blocks generation on the YouTube tab in
        // front - and it is the conservative direction I7 asks for. The dead-
        // picture half of the design (DrmWatch::OnPictureSummary) is NOT called:
        // its thresholds cannot be honest without measuring real protected
        // playback, and the owner has no subscription to measure against
        // (2026-09-20). Both halves stay `unverified` until that run happens.
        if (opt_.drmGate) {
            const DrmStatus ds = drm_.Status();
            const bool blocked = !ds.ok || ds.verdict != DrmVerdict::kClear;
            if (blocked != drmBlocked_) {
                drmBlocked_ = blocked;
                drmSignal_ = ds.signal;
                lastTrayStatus_ = 0;
                if (drmBlocked_) {
                    if (overlay_) overlay_->Hide();
                    Log("DRM gate: %s (pid %lu, %d children unreadable) - not generating over "
                        "protected video",
                        DrmSignalName(ds.signal), ds.cdmPid, ds.unreadableChildren);
                } else {
                    Log("DRM gate: clear - generation resumes");
                }
            }
        }

        // THE GAME GATE (P6, I1, I2, D3). Read once per iteration so every
        // consumer below sees one verdict, and placed BEFORE the occlusion test
        // so a game wins both the log line and the tray tooltip over a window
        // that merely covers the video.
        if (opt_.gameGate) {
            const GameGateStatus gs = gameGate_.Status();
            // !Ok() is CLEAR, never a held pause: when the instrument gives up
            // mid-run the video must keep playing (contract 1 of nsp_gamegate.h).
            const bool nowGame = gameGate_.Ok() && gs.verdict == GateVerdict::kPaused;
            if (nowGame != gamePaused_) {
                gamePaused_ = nowGame;
                gameSignal_ = gs.signal;
                gameExe_ = gs.exe;
                lastTrayStatus_ = 0;  // push the new tooltip at once
                if (gamePaused_) {
                    gamePausedAt_ = now;
                    if (overlay_) overlay_->Hide();
                    Log("game gate: %s pid %lu %s at %.0f%% of the 3D engine - generation PAUSED",
                        GameSignalName(gs.signal), gs.pid, Narrow(gs.exe).c_str(), gs.utilPct);
                } else {
                    Log("game gate: clear - generation resumes");
                    gameYielded_ = false;
                }
            }
            // YIELD. Ten seconds in, the game wants the few hundred megabytes and
            // the capture session we are still holding for a video nobody can see.
            if (gamePaused_ && engaged_ && !gameYielded_ &&
                QpcToSec(now - gamePausedAt_) > 10.0) {
                Log("game gate: 10 s paused - releasing the capture session and the device");
                gameYielded_ = true;
                Disengage();
            }
        }

        // OFF MEANS OFF. Turning generation off used to hide the overlay and
        // leave the whole pipeline running: WGC still delivered frames, motion
        // was still estimated once a pair and a frame was still synthesized and
        // presented on every compositor tick, all of it invisible (beta audit,
        // 2026-09-20). A tray utility the user has switched off must not keep a
        // capture session and a few hundred megabytes of GPU memory, and must
        // not heat the machine for a picture nobody sees.
        if (guardWantsRescout_) {
            guardWantsRescout_ = false;
            if (engaged_) {
                // Each give-up costs a device, an overlay and a capture session to rebuild. One
                // after a scroll is the design, and so is one after the user changed the layout
                // twice in ten seconds: an engagement that SHOWED a picture did its job. What
                // backs off (1, 2, 4 ... 30 s) is a string of engagements that never showed one
                // - a page whose damage keeps breaking the rect before it is ever confirmed.
                if (shownThisEngage_) {
                    shortGiveUps_ = 0;
                } else if (++shortGiveUps_ >= 2) {
                    const double hold = (std::min)(30.0, std::ldexp(1.0, shortGiveUps_ - 2));
                    rescoutHoldUntil_ = now + static_cast<int64_t>(hold * QpcPerSecond());
                    Log("windowed video: %d short-lived engagements in a row - looking again in %.0f s",
                        shortGiveUps_, hold);
                }
                Disengage();
            }
        }
        if (!wantShown_ && engaged_) {
            Log("frame generation is off - releasing the capture session and the device");
            Disengage();
        }

        // THE OCCLUSION TEST — owner's rule: while ANY window covers even part of
        // the video, generation pauses.
        //
        // Run the moment the window layout changes (WindowWatch: foreground,
        // show/hide, cloak, minimize, move/size), every iteration while a window
        // is being dragged, and ten times a second regardless as the safety net
        // for what no event reports (a program moving its window by itself). The
        // screen is already correct in the meantime: the overlay sits one slot
        // above the browser, so a covering window is drawn over it by DWM on its
        // first frame; this test only decides whether generating is worthwhile.
        {
            const bool dirty = watch_.TakeDirty();
            const double sinceCheck = QpcToSec(now - lastObscuredCheck_);
            const bool due = dirty || sinceCheck > 0.1 ||
                             (watch_.MoveSizeActive() && sinceCheck > 0.012);
            if (engaged_ && overlay_ && due) {
                lastObscuredCheck_ = now;
                const int64_t c0 = QpcNow();
                OccluderInfo who, widget;
                bool nowObscured =
                    VideoRectObscured(target_.hwnd, overlay_->VideoRect(), overlay_->Hwnd(), &who,
                                      &opt_.occlusionIgnore, !opt_.pauseForWidgets, &widget);
                if (widget.hwnd && widget.hwnd != lastWidgetLogged_) {
                    lastWidgetLogged_ = widget.hwnd;
                    Log("ignoring always-on-top widget 0x%p %s [%s] %ldx%ld over the video "
                        "(--pause-for-widgets pauses for it)",
                        static_cast<void*>(widget.hwnd), Narrow(widget.exe).c_str(),
                        Narrow(widget.cls).c_str(), RectW(widget.bounds), RectH(widget.bounds));
                }
                // The target itself going away counts too: a minimised browser or
                // one cloaked by a virtual-desktop switch stops delivering frames,
                // but the stall timer would leave our last picture on the new
                // desktop for 400 ms first.
                DWORD cloaked = 0;
                const bool targetHidden =
                    IsIconic(target_.hwnd) ||
                    (SUCCEEDED(DwmGetWindowAttribute(target_.hwnd, DWMWA_CLOAKED, &cloaked,
                                                     sizeof(cloaked))) &&
                     cloaked != 0);
                nowObscured = nowObscured || targetHidden;
                const double us = QpcToMs(QpcNow() - c0) * 1000.0;
                occlUsSum_ += us;
                occlUsMax_ = (std::max)(occlUsMax_, us);
                ++occlChecks_;

                if (nowObscured != obscured_) {
                    obscured_ = nowObscured;
                    if (obscured_) {
                        overlay_->Hide();
                        if (targetHidden) {
                            Log("the browser window is minimised or cloaked - generation PAUSED");
                        } else {
                            Log("window 0x%p %s [%s] covers %s of the video (its bounds %s) - "
                                "generation PAUSED",
                                static_cast<void*>(who.hwnd), Narrow(who.exe).c_str(),
                                Narrow(who.cls).c_str(), RectStr(who.overlap).c_str(),
                                RectStr(who.bounds).c_str());
                        }
                    } else {
                        Log("the video is clear again - generation resumes");
                    }
                    // Absolute QPC, so an external test can time the reaction.
                    Log("  (qpc %.3f ms)", QpcToMs(c0));
                }
            }
        }

        // The target MOVING is checked every iteration, not every 250 ms: the
        // overlay is frozen where the video was (I11), and one slot above the
        // browser it covers whatever the browser uncovers as it is dragged,
        // snapped or maximised. GetWindowRect reads user-mode memory, so this is
        // cheap; the full check below runs in the same iteration when it fires.
        if (engaged_ && overlay_) {
            RECT wr{};
            if (!GetWindowRect(target_.hwnd, &wr) ||
                memcmp(&wr, &target_.windowRect, sizeof(RECT)) != 0) {
                overlay_->Hide();
                lastGeomCheck = 0;
            }
        }

        if (QpcToSec(now - lastGeomCheck) > 0.25) {
            lastGeomCheck = now;
            TargetWindow t{};
            if (engaged_ && DescribeWindow(target_.hwnd, &t)) {
                if (memcmp(&t.clientScreen, &target_.clientScreen, sizeof(RECT)) != 0) {
                    Log("target moved/resized: %s -> %s - re-engaging",
                        RectStr(target_.clientScreen).c_str(), RectStr(t.clientScreen).c_str());
                    Disengage();
                } else if (NeedFullscreen() && !guardActive_ && !t.fullscreen) {
                    Log("target left fullscreen - disengaging");
                    Disengage();
                } else {
                    // The frame changed without the client area moving: take the
                    // new rect as the baseline, or the check above would hide the
                    // overlay again on every iteration.
                    target_.windowRect = t.windowRect;
                }
            } else if (engaged_) {
                Log("target window is gone - disengaging");
                Disengage();
            }
            if (engaged_ && overlay_) {
                // BOTH rates, every time. The desktop maximum used to be re-read
                // only when the video's own monitor changed rate, so plugging in
                // a faster display left rateGate_ off and the engine presented at
                // the compositor's new 165 Hz onto a 60 Hz output - generating
                // above the refresh, which I14 forbids (beta audit, 2026-09-20).
                bool exact = false;
                const double hz = MonitorRefreshHz(overlay_->VideoRect(), &exact);
                const double maxHz = DesktopMaxRefreshHz();
                if (hz > 1.0 && (fabs(hz - targetHz_) > 0.5 || fabs(maxHz - desktopMaxHz_) > 0.5)) {
                    const double oldMax = desktopMaxHz_;
                    desktopMaxHz_ = maxHz;
                    rateGate_ = hz < desktopMaxHz_ - 1.0;
                    if (!overlay_->SetFrameLatency(rateGate_ ? 1U : 2U)) {
                        LogErr("output frame-latency update failed - re-engaging");
                        faultDisengage_ = true;
                        Disengage();
                        continue;
                    }
                    Log("output refresh %.3f -> %.3f Hz (desktop max %.3f -> %.3f) - generation %s",
                        targetHz_, hz, oldMax, maxHz,
                        rateGate_ ? "paced down to this monitor" : "follows the compositor clock");
                    targetHz_ = hz;
                    nextPresentQpc_ = 0;
                }
            }
        }

        // Not while a window is being dragged or sized: every attempt would build a
        // device, an overlay and a capture session for a rect that is about to move.
        // The backoff counts only EXPENSIVE failures. reengageFails_ also counts
        // "Chrome is not running yet", which a background tool does hundreds of
        // times an hour; using it here meant the first real failure jumped
        // straight to the 10 s ceiling (review, 2026-09-20).
        const double reengageAfter =
            engageCostly_ ? (std::min)(10.0, 1.0 + 0.5 * static_cast<double>(costlyFails_)) : 1.0;
        CheckMediaGate(now);
        Scout(now);
        if (engageNow_) {
            engageNow_ = false;
            if (!engageCostly_) lastReengageAttempt = 0;
        }
        if (!engaged_ && wantShown_ && !gamePaused_ && !watch_.MoveSizeActive() &&
            QpcToSec(now - lastReengageAttempt) > reengageAfter) {
            lastReengageAttempt = now;
            if (!Engage()) {
                // overlay_ survives a failed Engage, so its presence says how far
                // the attempt got.
                engageCostly_ = overlay_ != nullptr;
                if (engageCostly_) {
                    ++costlyFails_;
                    faultDisengage_ = true;
                }
                // Once, then every 30 s. Waiting for Chrome is the normal state
                // of a background tool, and a line a second would bury the log
                // that matters when it finally attaches.
                ++reengageFails_;
                if (reengageFails_ == 1 || (reengageFails_ % 30) == 0)
                    Log("still waiting for a target (%llu attempts)",
                        static_cast<unsigned long long>(reengageFails_));
            } else {
                reengageFails_ = 0;
                // NOT costlyFails_: an engagement that succeeds and then dies
                // half a second later is the case the backoff exists for, and
                // clearing the count here is what let it rebuild the whole
                // pipeline once a second for ever (review, 2026-09-20). It is
                // cleared below, once the engagement has held.
                engageCostly_ = false;
                engagedAtQpc_ = now;
            }
        }

        LogStats();
        if (QpcToSec(QpcNow() - lastTrayStatus_) >= 1.0) {
            lastTrayStatus_ = QpcNow();
            tray_.SetStatus(TrayStatus());
        }
    }

    // The tray first: it is what the user sees, and closing the console leaves only a few seconds
    // (OnConsoleCtrl) — an icon removed last could be killed with the process and linger in the
    // notification area until the pointer passes over it.
    tray_.Destroy();
    StopScout();
    Disengage();
    captureRuntime_.reset();
    media_.Stop();
    watch_.Stop();
    UninstallHotkeys();
    return WritePresentTrace() && !traceInitFailed_ ? 0 : 1;
}

bool App::WritePresentTrace() {
    if (presentTracePath_.empty()) return true;
    FILE* file = nullptr;
    if (fopen_s(&file, presentTracePath_.c_str(), "wb") != 0 || !file) {
        LogErr("cannot create presentation trace: %s", presentTracePath_.c_str());
        return false;
    }
    bool ok = fprintf(file, "qpc_ms,a_ms,b_ms,phase,content_ms,poll_ms,motion_ms,motion_serial,motion_done_ms,previous,newest,older,pair,submitted,displayed,refresh,sync_qpc_ms,submitted_hr,media_valid\n") >= 0;
    for (const auto& row : presentTrace_) {
        if (!ok) break;
        const double content = row.a + row.phase * (row.b - row.a);
        ok = fprintf(file, "%.6f,%.6f,%.6f,%.9f,%.6f,%.6f,%.6f,%llu,%.6f,%lld,%lld,%d,%d,%u,%u,%u,%.6f,%ld,%d\n",
                     QpcToMs(row.qpc), row.a / 10000.0, row.b / 10000.0, row.phase, content / 10000.0,
                     row.pollMs, row.motionMs, static_cast<unsigned long long>(row.motionSerial), QpcToMs(row.motionDone),
                     static_cast<long long>(row.previous), static_cast<long long>(row.newest),
                     row.older ? 1 : 0, row.pair ? 1 : 0, row.media.submitted, row.media.displayed,
                     row.media.refresh, QpcToMs(row.media.syncQpc), static_cast<long>(row.media.submittedHr),
                     row.media.mediaValid ? 1 : 0) >= 0;
    }
    if (fclose(file) != 0) ok = false;
    const std::string gpuPath = presentTracePath_ + ".gpu.csv";
    file = nullptr;
    if (fopen_s(&file, gpuPath.c_str(), "wb") != 0 || !file) {
        LogErr("cannot create motion GPU trace: %s", gpuPath.c_str());
        return false;
    }
    ok = fprintf(file, "motion_serial,gpu_ms,disjoint,frequency,status\n") >= 0 && ok;
    for (const auto& gpu : motionGpuTrace_) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0, end = 0;
        HRESULT hr = gpu.context->GetData(gpu.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) hr = gpu.context->GetData(gpu.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) hr = gpu.context->GetData(gpu.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const double ms = hr == S_OK && !disjoint.Disjoint && disjoint.Frequency && end >= begin ?
            1000.0 * static_cast<double>(end - begin) / static_cast<double>(disjoint.Frequency) : -1.0;
        if (fprintf(file, "%llu,%.6f,%d,%llu,%ld\n", static_cast<unsigned long long>(gpu.serial), ms,
                    disjoint.Disjoint ? 1 : 0, static_cast<unsigned long long>(disjoint.Frequency),
                    static_cast<long>(hr)) < 0) ok = false;
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) LogErr("writing presentation trace failed: %s", presentTracePath_.c_str());
    return ok;
}

// The value after `flag` on the WIDE command line (the last one, as ParseArgs keeps). argv is in
// the ANSI code page, which cannot hold every path — a Cyrillic folder on an English-language
// Windows — and the settings path is one this program writes to.
std::wstring WideArgValue(const wchar_t* flag) {
    int n = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    std::wstring out;
    if (wargv) {
        for (int i = 1; i + 1 < n; ++i)
            if (wcscmp(wargv[i], flag) == 0) out = wargv[i + 1];
        LocalFree(wargv);
    }
    return out;
}

// --ui-snapshot: opens the Settings window without taking the focus, lets it paint, and writes
// what DWM shows for it to a PNG. The window gets checked without touching anyone's work:
// by default it opens on the second monitor.
int RunUiSnapshot(const AppOptions& opt, const Settings& settings) {
    const ULONG_PTR gdiplus = StartGdiplus();
    SettingsWindow window;
    std::string err;
    if (!window.Show(settings, nullptr, opt.uiMonitor > 0 ? opt.uiMonitor : 2, false, &err)) {
        LogErr("--ui-snapshot: %s", err.c_str());
        StopGdiplus(gdiplus);
        return 2;
    }
    const int64_t until = QpcNow() + QpcPerSecond() / 2;
    MSG msg;
    while (QpcNow() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }

    RECT wr{};
    GetWindowRect(window.Hwnd(), &wr);
    const int w = RectW(wr), h = RectH(wr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HDC screen = GetDC(nullptr);
    const HDC mem = CreateCompatibleDC(screen);
    const HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    int rc = 0;
    if (!dib || !bits) {
        LogErr("--ui-snapshot: CreateDIBSection failed");
        rc = 2;
    } else {
        const HGDIOBJ old = SelectObject(mem, dib);
        // PW_RENDERFULLCONTENT: the picture DWM composes, title bar included.
        const bool printed = PrintWindow(window.Hwnd(), mem, PW_RENDERFULLCONTENT) != FALSE;
        SelectObject(mem, old);
        if (!printed) {
            LogErr("--ui-snapshot: PrintWindow failed, GetLastError=%lu", GetLastError());
            rc = 2;
        } else {
            auto* px = static_cast<uint8_t*>(bits);
            for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;  // GDI leaves alpha undefined
            if (!SaveImageBgraPng(opt.uiSnapshot, px, static_cast<UINT>(w), static_cast<UINT>(h),
                                  static_cast<UINT>(w * 4), &err)) {
                LogErr("--ui-snapshot: %s", err.c_str());
                rc = 2;
            } else {
                Log("--ui-snapshot: %dx%d window written to %s", w, h, opt.uiSnapshot.c_str());
            }
        }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    window.Close();
    StopGdiplus(gdiplus);
    return rc;
}

// The program is linked /SUBSYSTEM:WINDOWS so that autostart never flashes a
// console (P7). That costs the lab nothing: a GUI-subsystem process inherits
// its standard handles exactly like a console one, so `silkplay.exe > run.log`
// and every harness that passes a pipe to subprocess.Popen keep working
// unchanged. Only an interactive run from a terminal has nowhere to write, and
// that is what this restores - on demand, never by default.
bool g_allocatedConsole = false;

void EnsureConsole() {
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    // Already redirected or inherited: that handle is the developer's choice and
    // must not be clobbered.
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return;
        g_allocatedConsole = true;
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    // stdin too: without it the "[press Enter]" that holds an ALLOCATED console
    // open reads EOF immediately, the window closes with the process, and the
    // output it was allocated for is never seen (review, 2026-09-20).
    freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
}

// Every switch below prints and is developer-only, so being asked for one is
// being asked for output. --console is the explicit form.
bool WantsConsole(int argc, char** argv) {
    static const char* kSwitches[] = {
        "--console", "--help", "-h", "/?", "--offline", "--offline-out", "--list-displays",
        "--list-windows", "--dump", "--log-dirty", "--stats-every", "--warp-lab", "--field-lab",
        "--ui-snapshot", "--default-settings", "--settings-file", "--game-gate-probe",
    };
    for (int i = 1; i < argc; ++i) {
        for (const char* sw : kSwitches)
            if (strcmp(argv[i], sw) == 0) return true;
    }
    return false;
}

int RunMain(int argc, char** argv) {
    if (WantsConsole(argc, argv)) EnsureConsole();
    AppOptions opt;
    if (!ParseArgs(argc, argv, &opt)) return 1;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo)) {
        LogErr("CoInitializeEx(MTA) failed %s", HrString(hrCo).c_str());
        return 2;
    }

    if (!opt.offlineDir.empty()) {
        OfflineOptions oo;
        // Sorted by name: the harness names frames with a zero-padded index, so
        // lexicographic order IS temporal order. Anything else would silently
        // interpolate the wrong pair.
        std::vector<std::string> frames;
        {
            const std::wstring pattern = Widen((opt.offlineDir + "/*.png").c_str());
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        frames.push_back(opt.offlineDir + "/" + Narrow(fd.cFileName));
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            std::sort(frames.begin(), frames.end());
        }
        if (frames.empty()) {
            LogErr("--offline: no .png files in %s", opt.offlineDir.c_str());
            CoUninitialize();
            return 2;
        }
        oo.frames = frames;
        oo.outDir = opt.offlineOut;
        oo.mode = (opt.mode == Mode::Mc) ? "mc"
                  : (opt.mode == Mode::Blend) ? "blend" : "passthrough";
        if (!opt.offlineTs.empty()) oo.ts = opt.offlineTs;
        oo.useOfa = opt.useOfa;
        oo.cellPx = opt.cellPx;
        oo.ofaGrid = opt.ofaGrid;
        oo.occMode = opt.occMode;
        oo.repeat = opt.offlineRepeat;
        oo.checkEndpoints = opt.offlineCheck;
        oo.warmup = opt.offlineWarmup;
        oo.injectDir = opt.offlineInject;
        oo.ofaHints = opt.ofaHints;
        oo.warpLab = opt.warpLab;
        oo.warpLabParam = opt.warpLabParam;
        oo.fieldLab = opt.fieldLab;
        oo.gpuTime = opt.offlineGpuTime;
        oo.fieldCohere = opt.fieldCohere;
        oo.ofaSeedHints = opt.ofaSeedHints;
        oo.fieldLabParam = opt.fieldLabParam;
        const int rc = RunOffline(oo);
        CoUninitialize();
        return rc;
    }

    if (opt.listDisplays) {
        struct Ctx {
            int n = 0;
        } ctx;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                MONITORINFOEXW mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(mon, &mi)) {
                    bool exact = false;
                    const double hz = MonitorRefreshHz(mi.rcMonitor, &exact);
                    printf("%-16s %-22s %8.3f Hz%s%s\n", Narrow(mi.szDevice).c_str(),
                           RectStr(mi.rcMonitor).c_str(), hz, exact ? "" : " (rounded)",
                           (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  PRIMARY" : "");
                }
                ++c->n;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        CoUninitialize();
        return 0;
    }

    // Settings are read before anything that shows UI.
    std::wstring settingsPath;
    if (!opt.defaultSettings)
        settingsPath = opt.settingsFile.empty() ? SettingsPath() : WideArgValue(L"--settings-file");
    Settings settings;
    bool settingsUnreadable = false;
    if (opt.defaultSettings) {
        Log("settings: built-in defaults (--default-settings) - nothing is read or saved this run");
    } else if (settingsPath.empty()) {
        LogErr("settings: no location for settings.json (LOCALAPPDATA unresolved) - built-in "
               "defaults, nothing saved this run");
    } else {
        std::string err;
        bool loaded = LoadSettingsFrom(settingsPath, &settings, &err);
        if (!loaded) {
            // Once more after a moment: a scanner holding the file briefly is the likely cause.
            Sleep(250);
            err.clear();
            loaded = LoadSettingsFrom(settingsPath, &settings, &err);
        }
        if (!loaded) {
            // Unreadable is not "first run": the user made choices this run cannot see. Fail closed
            // — capture nothing until a browser is chosen again — and keep what they wrote: the
            // file is moved aside so the next save cannot overwrite it, and if even that fails,
            // nothing is saved this run.
            settingsUnreadable = true;
            settings = Settings{};
            settings.captureChrome = false;
            const std::wstring aside = settingsPath + L".unreadable";
            if (MoveFileExW(settingsPath.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                LogErr("%s - kept as %s; nothing is captured until a browser is chosen in Settings",
                       err.c_str(), Narrow(aside).c_str());
            } else {
                const DWORD e = GetLastError();
                LogErr("%s - could not move it aside (GetLastError=%lu); nothing is captured and "
                       "nothing is saved this run",
                       err.c_str(), e);
                settingsPath.clear();
            }
        }
    }
    SetUiLanguage(opt.uiLang >= 0 ? static_cast<UiLanguage>(opt.uiLang) : settings.language);

    if (!opt.uiSnapshot.empty()) {
        const int rc = RunUiSnapshot(opt, settings);
        CoUninitialize();
        return rc;
    }

    // THE GATE'S OWN ACCEPTANCE TEST. It opens no window, creates no overlay and
    // captures nothing, so it can be left running beside the owner's real work
    // for hours - which is the only way to prove the thing that matters most
    // about a game gate: that it never pauses a video by mistake (N14).
    if (opt.gateProbeSec > 0.0) {
        GameGate gate;
        std::string gerr;
        if (!gate.Start(&gerr)) {
            LogErr("game gate probe: %s", gerr.c_str());
            CoUninitialize();
            return 4;
        }
        Log("game gate probe: %.0f s, sampling. A line per verdict change, a summary at the end.",
            opt.gateProbeSec);
        const int64_t start = QpcNow();
        GateVerdict last = GateVerdict::kClear;
        int changes = 0;
        double utilMax = 0.0;
        uint64_t samplesSeen = 0;
        GameGateStatus gs{};
        while (QpcToSec(QpcNow() - start) < opt.gateProbeSec) {
            Sleep(100);
            gs = gate.Status();
            samplesSeen = gs.samples;
            utilMax = (std::max)(utilMax, gs.utilPct);
            if (gs.verdict != last) {
                last = gs.verdict;
                ++changes;
                Log("t=%7.1f s  verdict=%s  signal=%s  pid=%lu %s  util=%.0f%%",
                    QpcToSec(QpcNow() - start),
                    gs.verdict == GateVerdict::kPaused ? "PAUSED" : "clear",
                    GameSignalName(gs.signal), gs.pid, Narrow(gs.exe).c_str(), gs.utilPct);
            }
            if (!gate.Ok()) {
                LogErr("game gate probe: the gate gave up mid-run - see the lines above");
                break;
            }
        }
        Log("game gate probe done: %llu samples, %d verdict changes, peak tree load %.0f%%, "
            "PDH collect p50 %.2f ms p99 %.2f ms",
            static_cast<unsigned long long>(samplesSeen), changes, utilMax, gs.collectMsP50,
            gs.collectMsP99);
        Log("PASS means changes=0 while nothing but ordinary work ran.");
        gate.Stop();
        CoUninitialize();
        return changes == 0 ? 0 : 1;
    }

    if (opt.listWindows) {
        const auto all = EnumerateWindows();
        printf("%-18s %-8s %-24s %-28s %-22s %-6s %s\n", "HWND", "PID", "PROCESS", "CLASS",
               "CLIENT", "CLOAK", "TITLE");
        for (const auto& w : all) {
            printf("0x%-16p %-8lu %-24s %-28s %-22s %-6s %s\n", static_cast<void*>(w.hwnd), w.pid,
                   Narrow(w.exe).c_str(), Narrow(w.cls).c_str(), RectStr(w.clientScreen).c_str(),
                   w.cloaked ? "YES" : "no", Narrow(w.title).c_str());
        }
        CoUninitialize();
        return 0;
    }

    // One engine per session. Two would each keep inserting their overlay into
    // the one slot above the browser, swapping places on every iteration (found
    // in review), and both would capture and generate the same video.
    // ONE ENGINE PER SESSION. Two would take turns stealing the z-order slot
    // above the browser and fight over the same hotkeys.
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\NovaSilkPlay.Engine");
    bool alreadyRunning = instance && GetLastError() == ERROR_ALREADY_EXISTS;
    if (!instance) {
        // CreateMutexW can fail outright - most plausibly ACCESS_DENIED against a
        // mutex another instance owns. Failing to CREATE it used to fall straight
        // through into a second engine; ask whether it EXISTS instead (beta
        // audit, 2026-09-20).
        const DWORD createErr = GetLastError();
        HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\NovaSilkPlay.Engine");
        if (existing) {
            CloseHandle(existing);
            alreadyRunning = true;
        } else {
            LogErr("single-instance mutex could not be created (error %lu) and does not exist - "
                   "continuing without the guard", createErr);
        }
    }
    if (alreadyRunning) {
        LogErr("another silkplay.exe is already running in this session - exiting");
        // Started from a shortcut there is nowhere for that line to go, and the
        // program appeared to do nothing at all. Say it on screen, but only then:
        // a harness run must never stop on a dialog.
        if (!GetConsoleWindow()) {
            SetUiLanguage(settings.language);
            MessageBoxW(nullptr,
                        UiIsRussian()
                            ? L"Nova SilkPlay \u0443\u0436\u0435 \u0437\u0430\u043f\u0443\u0449"
                              L"\u0435\u043d\u0430.\n\n\u041e\u0442\u043a\u0440\u043e\u0439\u0442"
                              L"\u0435 \u0435\u0451 \u0437\u043d\u0430\u0447\u043e\u043a \u0432 "
                              L"\u043e\u0431\u043b\u0430\u0441\u0442\u0438 \u0443\u0432\u0435"
                              L"\u0434\u043e\u043c\u043b\u0435\u043d\u0438\u0439."
                            : L"Nova SilkPlay is already running.\n\nUse its icon in the "
                              L"notification area.",
                        L"Nova SilkPlay", MB_OK | MB_ICONINFORMATION);
        }
        if (instance) CloseHandle(instance);
        CoUninitialize();
        return 3;
    }

    // A SECOND, PLAIN mutex, for Inno Setup's AppMutex: an unprefixed name lands
    // in the session namespace, which is what the installer opens to notice that
    // the program is running. A Global\\ name is impossible here - an unelevated
    // process has no SeCreateGlobalPrivilege. This one is never waited on; its
    // existence is the whole signal.
    HANDLE appMutex = CreateMutexW(nullptr, FALSE, L"NovaSilkPlayAppMutex");

    // Migrate enabled legacy Run entries; repair missing shortcut targets only.
    // Running a development build never redirects a working installed shortcut.
    AutostartRepair();

    // Manual-reset and created here, not opened: we own it, and a stale set
    // state from a previous run cannot exist because the object dies with us.
    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);

    g_runDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(&OnConsoleCtrl, TRUE);
    App app(opt, settings, settingsPath, settingsUnreadable);
    const int rc = app.Run();
    if (g_runDone) SetEvent(g_runDone);
    if (instance) CloseHandle(instance);
    if (appMutex) CloseHandle(appMutex);
    if (g_quitEvent) {
        CloseHandle(g_quitEvent);
        g_quitEvent = nullptr;
    }
    // A console WE allocated vanishes with the process, taking the output with
    // it. One that we attached to, or that was redirected, belongs to someone
    // else and is left alone.
    if (g_allocatedConsole) {
        fputs("\n[press Enter]", stdout);
        fflush(stdout);
        (void)getchar();
    }
    CoUninitialize();
    return rc;
}

}  // namespace
}  // namespace nsp

// /SUBSYSTEM:WINDOWS, so autostart never flashes a console (P7).
//
// WinMain, NOT wWinMain, and the difference is not cosmetic: the entry point
// chooses which CRT startup runs, and wWinMainCRTStartup builds only __wargv -
// __argv stays NULL and the first argv[i] crashes the process. MEASURED here
// (2026-09-20): the wWinMain build segfaulted on `silkplay.exe --help` before
// printing a byte. WinMainCRTStartup fills __argc/__argv exactly as
// mainCRTStartup did, so ParseArgs, the wide-argument helper and the ACP
// encoding of argv are all byte-identical to what main() used to see.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return nsp::RunMain(__argc, __argv); }
