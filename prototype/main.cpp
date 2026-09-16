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
#include "nsp_common.h"
#include "nsp_dump.h"
#include "nsp_offline.h"
#include "nsp_overlay.h"
#include "nsp_tray.h"
#include "nsp_synth.h"
#include "nsp_winwatch.h"

#include <dwmapi.h>

#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
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
    bool   pauseForWidgets = false; // --pause-for-widgets: the strict occlusion rule
    bool   overlayLayered = true;   // --overlay-unlayered: measurement of LAYERED's cost only
    bool   logDirty = false;        // --log-dirty: per-capture video-frame test verdicts
    bool   videoFrameTest = true;   // --no-frame-test: every capture is a video frame (A/B)
    std::vector<std::wstring> occlusionIgnore;  // --occlusion-ignore a.exe,b.exe (lower-case)
    bool   listWindows = false;
    bool   listDisplays = false;
    bool   badge = true;         // the "24/165" readout in the top-right corner of the video
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

    // Development aid: after `dumpAfterSec` of steady playback, write the two
    // source frames and the synthesized in-between frame, then exit.
    std::string dumpPrefix;
    double dumpAfterSec = 4.0;
};

void PrintUsage() {
    printf(
        "Nova SilkPlay prototype — frame generation over a browser window.\n"
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
        "  --allow-windowed         also attach to a browser that is not fullscreen\n"
        "                           (default: fullscreen only; --rect implies it)\n"
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
        "  --warp-lab N             warp laboratory: branch maps / ablations (P21)\n"
        "  --field-lab N            per-pair field coherence lab (P21)\n"
        "  --no-field-cohere        turn off the per-pair field coherence pass (P21)\n"
        "  --stats-every F          stats line interval in seconds (default 3)\n"
        "  --dump PREFIX            write PREFIX_a/_b/_mc/_blend .ppm after a few\n"
        "                           seconds of playback, then exit (dev aid)\n"
        "  --dump-after F           when to dump, in seconds (default 4)\n"
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
        } else if (a == "--list-windows") {
            opt->listWindows = true;
        } else if (a == "--target-class" && next(&v)) {
            opt->targetClass = Widen(v);
        } else if (a == "--target-exe" && next(&v)) {
            opt->targetExe = Widen(v);
        } else if (a == "--target-title" && next(&v)) {
            opt->targetTitle = Widen(v);
        } else if (a == "--target-pid" && next(&v)) {
            opt->targetPid = static_cast<DWORD>(strtoul(v, nullptr, 10));
            opt->targetExe.clear();  // an explicit pid is the whole answer
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
        if (count_ < 6) return 0.0;
        double tmp[kN];
        std::copy(intervals_, intervals_ + count_, tmp);
        std::nth_element(tmp, tmp + count_ / 2, tmp + count_);
        const double med = tmp[count_ / 2];
        if (med <= 0.0) return 0.0;
        double sum = 0.0;
        size_t n = 0;
        for (size_t i = 0; i < count_; ++i) {
            if (intervals_[i] >= med * 0.4 && intervals_[i] <= med * 1.6) {
                sum += intervals_[i];
                ++n;
            }
        }
        return n ? sum / static_cast<double>(n) : med;
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
        case VK_LEFT: return kHkLeft;
        case VK_RIGHT: return kHkRight;
        case VK_UP: return kHkUp;
        case VK_DOWN: return kHkDown;
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
LRESULT CALLBACK LowLevelKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrl && alt) {
            const int id = HotkeyIdForVk(k->vkCode);
            if (id != 0) {
                PostThreadMessageW(g_mainThread, WM_NSP_HOTKEY, static_cast<WPARAM>(id), 0);
                return 1;  // swallowed: nobody else gets this combination
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
DWORD WINAPI HookThreadProc(LPVOID param) {
    g_hookThread = GetCurrentThreadId();
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyProc, GetModuleHandleW(nullptr), 0);
    SetEvent(static_cast<HANDLE>(param));  // Install() may now read g_kbHook
    if (!g_kbHook) return 0;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // WM_NSP_HOTKEY is posted to g_mainThread by the callback; nothing is
        // expected here except WM_QUIT at shutdown.
    }
    UnhookWindowsHookEx(g_kbHook);
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

// ---------------------------------------------------------------------- app

class App {
public:
    explicit App(const AppOptions& opt) : opt_(opt) {}

    int Run();

private:
    bool ResolveTarget();
    bool NeedFullscreen() const { return !opt_.allowWindowed && !opt_.haveRect; }
    bool Engage();
    void Disengage();
    void PollCapture();
    void Tick();
    void LogStats();
    bool DoDump();
    // "<source fps>/<output fps>" for the readout.
    std::string FpsText() const;

    AppOptions opt_;
    TargetWindow target_{};
    RECT videoClient_{};  // video rect relative to the client origin (source of truth)

    std::unique_ptr<Overlay> overlay_;
    std::unique_ptr<Capture> capture_;
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
    bool cursorOver_ = false;  // only with the cursor gate on: pointer on the video -> paused
    bool cursorGateTripped_ = false;  // the overlay took hit tests: input was being swallowed
    int64_t lastObscuredCheck_ = 0;
    int64_t cursorLeftAt_ = 0;
    WindowWatch watch_;
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
    double motionCpuMs_ = 0.0;  // CPU time submitting the estimate, not GPU time
    uint64_t motionRuns_ = 0;
    uint64_t reengageFails_ = 0;
    Tray tray_;
    int64_t lastTrayStatus_ = 0;
};

bool App::ResolveTarget() {
    TargetWindow t{};
    // Explain a miss on the first attempt and every 30th: browsing without a
    // fullscreen video is the normal state now, and a line a second would bury
    // everything else in the log.
    const bool verbose = reengageFails_ == 0 || ((reengageFails_ + 1) % 30) == 0;
    if (!FindTarget(opt_.targetClass, opt_.targetTitle, opt_.targetPid, &t, opt_.targetExe,
                    NeedFullscreen(), verbose)) {
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
    target_ = t;

    if (!opt_.haveRect) {
        // Whole client area. For fullscreen video that IS the video rect.
        videoClient_ = RECT{0, 0, RectW(t.clientScreen), RectH(t.clientScreen)};
    } else if (opt_.rectIsScreen) {
        videoClient_ = RECT{opt_.rectClient.left - t.clientScreen.left,
                            opt_.rectClient.top - t.clientScreen.top,
                            opt_.rectClient.right - t.clientScreen.left,
                            opt_.rectClient.bottom - t.clientScreen.top};
    } else {
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

    synth_ = std::make_unique<Synth>();
    if (!synth_->Create(overlay_->Device(), overlay_->Context(), &err)) {
        LogErr("synth creation failed: %s", err.c_str());
        return false;
    }
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
    capture_ = std::make_unique<Capture>();
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
    Log("engaged, mode=%s, overlay hidden until the first source pair arrives", ModeName(opt_.mode));
    return true;
}

void App::Disengage() {
    if (overlay_) overlay_->Hide();
    zFixesAtStats_ = 0;
    framesAtStats_ = 0;
    capture_.reset();
    synth_.reset();
    overlay_.reset();
    engaged_ = false;
}

// Called several times per compositor tick. A source frame is only noticed when
// it is polled, so polling once per present would quantise the interpolation
// phase to a whole 6.06 ms refresh — which showed up directly as ~11 % of output
// frames sitting on a clamped phase.
void App::PollCapture() {
    if (!engaged_) return;
    const int ingested = capture_->Poll();
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
            const int64_t t0 = QpcNow();
            // The pair being replaced is the pacing's older pair: keep its field.
            if (ingested == 1 && capture_->OlderSrv()) {
                synth_->HoldFields();
            } else {
                synth_->DropHeldFields();
            }
            synth_->PrepareMotion(capture_->PreviousSrv(), capture_->NewestSrv());
            motionCpuMs_ += QpcToMs(QpcNow() - t0);
            ++motionRuns_;
        }
    }
}

void App::Tick() {
    if (!engaged_) return;

    PollCapture();
    const double now100 = Qpc100nsNow();

    if (capture_->NeedsReengage()) {
        Log("capture asked for a re-engage");
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
    const double outTick = outFpsSmooth_ > 30.0 ? 10000000.0 / outFpsSmooth_ : 60600.0;
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
        const double shown = now100 - lag;
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
        const double u = (now100 - lag - tA) / span;
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

    // Rate gate. The compositor clock ticks at the fastest output on the
    // desktop, so on a 60 Hz secondary monitor it would still wake us 165 times
    // a second. Presenting only when this monitor's next refresh is due keeps
    // the generated rate at the rate that is actually scanned out.
    const int64_t nowQpc = QpcNow();
    if (rateGate_) {
        const int64_t period = static_cast<int64_t>(QpcPerSecond() / targetHz_);
        if (nextPresentQpc_ == 0) {
            nextPresentQpc_ = nowQpc;
        } else if (nowQpc < nextPresentQpc_ - period / 4) {
            ++gateSkips_;
            return;  // too early for this output: capture was polled, present is not due
        }
        nextPresentQpc_ += period;
        // Never accumulate a backlog: after a stall, restart from now.
        if (nextPresentQpc_ < nowQpc - period) nextPresentQpc_ = nowQpc + period;
    }

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (!overlay_->AcquireBackBuffer(&back, &rtv)) return;

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
        // Fixed: the top-right corner of the video rect, 12 px in, twice the
        // 5x7 font at 1080p and three times from 1440p up.
        const int scale = vh >= 1400 ? 3 : 2;
        synth_->DrawFpsText(rtv, vw, vh, static_cast<float>(vw) - 12.0f, 12.0f, scale, FpsText());
    }

    if (!overlay_->Present()) {
        LogErr("device lost on present - re-engaging");
        Disengage();
        return;
    }
    ++presents_;
    ++badgeFrames_;

    if (overlay_->IsShown()) {
        const double content = usePair ? tA + t * (tB - tA) : capture_->NewestTime100ns();
        // Not judder: a next video frame more than 60 % of a period overdue means
        // the source is holding still (or stopped), and the picture correctly holds
        // too; and a content jump of several periods is the restart after such a
        // hold, a discontinuity of the TIMESTAMPS, not of what is on screen. Both
        // are counted apart so they cannot drown the number that matters.
        const bool sourceHolding = usePair && uRaw > 1.6;
        if (judderLastWall100ns_ > 0.0 && !sourceHolding) {
            const double dWallMs = (now100 - judderLastWall100ns_) / 10000.0;
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
        judderLastWall100ns_ = now100;
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
    if (opt_.autoShow && wantShown_ && !stalled_ && !obscured_ && !cursorOver_ &&
        !overlay_->IsShown() && havePair &&
        cadence_.Ready()) {
        overlay_->Show();
        Log("overlay shown (source %.2f fps, capture delay %.1f ms)", cadence_.Fps(),
            delay / 10000.0);
    }
}

std::string App::FpsText() const {
    return std::to_string(shownSrcFps_) + "/" + std::to_string(shownOutFps_);
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
        synth_->DrawFpsText(rtv.Get(), w, h, static_cast<float>(w) - 12.0f, 12.0f, h >= 1400 ? 3 : 2,
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

    statsQpc_ = now;
    subTickPolls_ = 0;
    ticks_ = 0;
    gateSkips_ = 0;
    msClock_ = msWait_ = msTick_ = 0;
    motionCpuMs_ = 0.0;
    motionRuns_ = 0;
    presents_ = 0;
    framesAtStats_ = frames;
    uMin_ = 2.0;
    uMax_ = -1.0;
    held_ = 0;
}

int App::Run() {
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
    {
        std::string trayErr;
        if (!tray_.Create(L"Nova SilkPlay - starting", &trayErr))
            LogErr("%s - continuing without a tray icon", trayErr.c_str());
    }
    // A failed FIRST engage is not fatal any more. The loop below already
    // retries once a second, so starting before Chrome exists — or before any
    // video is on screen — just means waiting, which is what a background tool
    // is supposed to do. Exiting here instead was the single thing that made
    // this a "launch it at the right moment" utility rather than something that
    // can sit in the tray.
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
                wantShown_ = !wantShown_;
                if (!wantShown_ && overlay_) overlay_->Hide();
                tray_.SetEngineOn(wantShown_);
                Log("frame generation %s (tray)", wantShown_ ? "ON" : "OFF");
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
                        wantShown_ = !wantShown_;
                        if (!wantShown_ && overlay_) overlay_->Hide();
                        tray_.SetEngineOn(wantShown_);
                        Log("frame generation %s (Ctrl+Alt+Q) - the PROGRAM keeps running; "
                            "Ctrl+Alt+X or the tray menu quits",
                            wantShown_ ? "ON" : "OFF");
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
                } else if (NeedFullscreen() && !t.fullscreen) {
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
                bool exact = false;
                const double hz = MonitorRefreshHz(overlay_->VideoRect(), &exact);
                if (hz > 1.0 && fabs(hz - targetHz_) > 0.5) {
                    desktopMaxHz_ = DesktopMaxRefreshHz();
                    rateGate_ = hz < desktopMaxHz_ - 1.0;
                    Log("output refresh changed %.3f -> %.3f Hz (desktop max %.3f) - generation %s",
                        targetHz_, hz, desktopMaxHz_,
                        rateGate_ ? "paced down to this monitor" : "follows the compositor clock");
                    targetHz_ = hz;
                    nextPresentQpc_ = 0;
                }
            }
        }

        // Not while a window is being dragged or sized: every attempt would build a
        // device, an overlay and a capture session for a rect that is about to move.
        if (!engaged_ && !watch_.MoveSizeActive() && QpcToSec(now - lastReengageAttempt) > 1.0) {
            lastReengageAttempt = now;
            if (!Engage()) {
                // Once, then every 30 s. Waiting for Chrome is the normal state
                // of a background tool, and a line a second would bury the log
                // that matters when it finally attaches.
                ++reengageFails_;
                if (reengageFails_ == 1 || (reengageFails_ % 30) == 0)
                    Log("still waiting for a target (%llu attempts)",
                        static_cast<unsigned long long>(reengageFails_));
            } else {
                reengageFails_ = 0;
            }
        }

        LogStats();
    }

    Disengage();
    watch_.Stop();
    UninstallHotkeys();
    tray_.Destroy();
    return 0;
}

int RunMain(int argc, char** argv) {
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
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\NovaSilkPlay.Engine");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        LogErr("another silkplay.exe is already running in this session - exiting");
        CloseHandle(instance);
        CoUninitialize();
        return 3;
    }

    App app(opt);
    const int rc = app.Run();
    if (instance) CloseHandle(instance);
    CoUninitialize();
    return rc;
}

}  // namespace
}  // namespace nsp

int main(int argc, char** argv) { return nsp::RunMain(argc, argv); }
