// probe_common.h — shared types for the S-M2 overlay/composition measurement probe.
//
// Spike S-M2 (see .claude/kb/research/directives-2026-09.md#6) asks two questions
// that can kill the browser path:
//   1. Does a DirectComposition overlay over Chrome cost Chrome its hardware plane
//      (G5, the +1-refresh latency claim, the VRR rejection)?
//   2. Does Windows.Graphics.Capture return real pixels while Chrome's video is on
//      an MPO plane at all?
// Everything here must run UNELEVATED and use only in-box Windows components.
// No PresentMon, no downloads: our own swapchain's composition mode comes from
// IDXGISwapChainMedia::GetFrameStatisticsMedia, Chrome's from its own histograms
// sampled over CDP by chrome_cdp.py.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace np {

// ---------------------------------------------------------------- CLI options

enum class PhaseKind { Hidden, Shown };

struct PhaseSpec {
    std::string name;      // free-form label used in the CSV and on stdout
    PhaseKind   kind = PhaseKind::Hidden;
    double      seconds = 10.0;
};

// How the overlay's *visual* is positioned. The HWND itself is created once and
// never resized (invariant I11) — geometry lives in the DComp visual only.
enum class GeomMode {
    Client,  // visual covers the target's whole client area
    Rect,    // visual covers Options::rectClient, expressed in target-client px
    Video    // reserved: rect supplied by the browser extension (not in this probe)
};

struct Options {
    std::wstring targetClass = L"Chrome_WidgetWin_1";
    std::wstring targetTitle;              // optional substring, case-insensitive
    DWORD        targetPid = 0;            // optional exact match

    GeomMode geom = GeomMode::Client;
    RECT     rectClient{0, 0, 0, 0};       // used when geom == Rect

    // The overlay HWND is always sized to the target's client area (I11); only
    // the DComp visual moves. `insetPx` shrinks the *visual* so the probe can be
    // told apart from the page underneath.
    int insetPx = 0;

    // S-M11's negative case: WS_EX_TRANSPARENT is claimed to be an unconditional
    // early-out in Chromium's gfx::IsWindowVisibleAndFullyOpaque(). Turning it
    // off must make Chrome's rAF counter stop.
    bool styleTransparent = true;
    bool styleTopmost     = true;

    enum class WgcSource { Window, Monitor };
    WgcSource wgcSource = WgcSource::Window;

    bool   wgc = true;
    double wgcMinUpdateIntervalMs = 1.0;    // G4: 0 or <1000 us caps WGC at ~50 fps
    bool   wgcTryBorderless = true;         // G14: expected to fail without pkg identity
    bool   wgcCursor = false;

    std::vector<PhaseSpec> phases;

    std::string csvPath  = "sm2_samples.csv";
    std::string jsonPath = "sm2_report.json";

    // When true, phase transitions wait for a line on stdin instead of a timer,
    // so chrome_cdp.py and this process step through phases together.
    bool stdinSync = false;

    // 0 = present on every compositor tick.
    int presentEveryNTicks = 1;
};

// ------------------------------------------------------------------- samples

// One present of *our* overlay swapchain.
struct PresentSample {
    // TRUE only when Present1() actually ran and returned a success code (which
    // includes DXGI_STATUS_OCCLUDED). FALSE when the frame never reached Present1
    // (back-buffer / RTV acquisition failed) or Present1 returned a failure that
    // was not device loss. Everything below is meaningless on a !valid sample, so
    // ONLY valid samples may enter the aggregates - a zeroed phantom sample drags
    // mean/p50/p99 present-CPU time to zero and inflates the "n/a" mode bucket.
    bool     valid = false;
    int64_t  qpc = 0;
    uint32_t phaseIndex = 0;
    uint32_t presentCount = 0;
    uint32_t presentRefreshCount = 0;
    uint32_t syncRefreshCount = 0;
    int64_t  syncQpcTime = 0;
    // DXGI_FRAME_PRESENTATION_MODE: 0 COMPOSED, 1 OVERLAY, 2 NONE,
    // 3 COMPOSITION_FAILURE. -1 when IDXGISwapChainMedia is unavailable.
    int      compositionMode = -1;
    uint32_t approvedPresentDuration = 0;
    double   presentCpuMs = 0.0;   // wall time inside Present1()
};

// One frame delivered by Windows.Graphics.Capture.
struct CaptureSample {
    int64_t  qpc = 0;
    uint32_t phaseIndex = 0;
    int64_t  systemRelativeTime100ns = 0;  // Direct3D11CaptureFrame::SystemRelativeTime
    uint32_t width = 0;
    uint32_t height = 0;
    double   meanLuma = 0.0;    // 0..255 over the downsampled grid
    double   stdLuma = 0.0;
    uint64_t gridHash = 0;      // FNV-1a over the downsampled grid
    bool     isBlack = false;         // meanLuma < 1.0 && stdLuma < 1.0
    bool     isDuplicate = false;     // gridHash equals the previous frame's
    double   readbackMs = 0.0;
    // false: the frame ARRIVED (qpc / systemRelativeTime100ns / width / height are
    // real) but its pixels could not be mapped without stalling the pace loop, so
    // meanLuma/stdLuma are NaN and gridHash/isBlack/isDuplicate mean nothing.
    // Aggregations over the pixel statistics must skip these samples.
    bool     statsValid = true;
};

// ------------------------------------------------------------------- helpers

int64_t QpcNow();
double  QpcToMs(int64_t ticks);          // ticks -> milliseconds
const char* CompositionModeName(int m);  // "COMPOSED" / "OVERLAY" / ... / "n/a"

// Logging: Log() goes to stdout and is line-buffered+flushed (the Python side
// synchronises on these lines). LogErr() goes to stderr.
void Log(const char* fmt, ...);
void LogErr(const char* fmt, ...);
std::string HrString(HRESULT hr);        // "0x80070057 (E_INVALIDARG)"
std::string Narrow(const std::wstring& w);

}  // namespace np
