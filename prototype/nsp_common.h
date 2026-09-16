// nsp_common.h — shared plumbing for the browser-path prototype.
//
// This directory is the FIRST end-to-end prototype (ROADMAP P4), not a
// measurement instrument: tools/overlay-probe answers questions, this binary
// tries to actually make video smoother. Where the two overlap the probe's
// hard-won details are reused verbatim (window styles, DComp creation order,
// the readback readiness gate), because they were paid for once already.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nsp {

// ------------------------------------------------------------------- logging

// Log() -> stdout, line-flushed. LogErr() -> stderr. Both carry a
// milliseconds-since-start prefix so a run can be read as a timeline.
void Log(const char* fmt, ...);
void LogErr(const char* fmt, ...);

std::string HrString(HRESULT hr);          // "0x80070057 (E_INVALIDARG)"
std::string Narrow(const std::wstring& w);

// ---------------------------------------------------------------------- time

int64_t QpcNow();
int64_t QpcPerSecond();
double  QpcToMs(int64_t ticks);
double  QpcToSec(int64_t ticks);
// QPC ticks -> the 100 ns unit Direct3D11CaptureFrame::SystemRelativeTime uses.
// Both are QPC-based, so this is an exact unit conversion, not a re-clocking.
double  Qpc100nsNow();

// ------------------------------------------------------------------ geometry

inline LONG RectW(const RECT& r) { return r.right - r.left; }
inline LONG RectH(const RECT& r) { return r.bottom - r.top; }
inline bool RectEmpty(const RECT& r) { return RectW(r) <= 0 || RectH(r) <= 0; }
std::string RectStr(const RECT& r);

// ------------------------------------------------------------------- device

// A plain hardware D3D11 device with BGRA support and multithread protection —
// the same one the overlay creates, minus the swapchain. The offline quality
// instrument needs a device with no window attached to it.
bool CreateRenderDevice(ID3D11Device** device, ID3D11DeviceContext** ctx, std::string* err);

// ---------------------------------------------------------------- displays

// Exact refresh rate of the monitor `screenRect` mostly sits on, in Hz.
// Per-monitor on purpose: a 165 Hz primary and a 60 Hz secondary are one
// desktop with one compositor clock, and generating 165 frames a second for a
// 60 Hz output is pure waste. Falls back to 60.0 only if every query fails, and
// says so through `exact` (false = the value is a guess, not a measurement).
double MonitorRefreshHz(const RECT& screenRect, bool* exact = nullptr);

// Fastest refresh among all active monitors — what the desktop-wide DirectComposition
// clock ticks at, and therefore the rate a window on a slower monitor must be paced down from.
double DesktopMaxRefreshHz();

// ------------------------------------------------------------- target window

struct TargetWindow {
    HWND        hwnd = nullptr;
    DWORD       pid = 0;
    std::wstring cls;
    std::wstring title;
    std::wstring exe;
    RECT        windowRect{};    // GetWindowRect
    RECT        clientScreen{};  // client area mapped to screen
    RECT        frameBounds{};   // DWMWA_EXTENDED_FRAME_BOUNDS (the WGC origin)
    bool        cloaked = false;
    bool        fullscreen = false;  // IsFullscreenWindow
};

// A browser in fullscreen: the window rect IS its monitor's rect, it has neither a
// caption nor a sizing frame, and it is not maximised. MEASURED on Chrome
// (2026-09-13): fullscreen style 160B0000 with window == monitor; maximised
// 17CF0000 with the window 8 px past every edge and the client on the WORK area;
// windowed 16CF0000. A maximised window with an auto-hidden taskbar can cover the
// whole monitor too, which is why the frame bits are tested as well as the rect.
bool IsFullscreenWindow(HWND hwnd);

// Describes the window that was found to cover the video rect.
struct OccluderInfo {
    HWND         hwnd = nullptr;
    DWORD        pid = 0;
    std::wstring cls;
    std::wstring exe;
    RECT         bounds{};   // the visible bounds that were tested, screen physical px
    RECT         overlap{};  // bounds intersected with the video rect
};

// Every visible, titled, uncloaked top-level window.
std::vector<TargetWindow> EnumerateWindows();
// Fills clientScreen / frameBounds / rects for an already-known HWND.
bool DescribeWindow(HWND hwnd, TargetWindow* out);
// Largest client area among the windows matching class/title/pid (empty filters
// match anything). Returns false when nothing matches.
// `exeSub` additionally requires the owning process image to contain that
// substring. It matters more than it looks: Chrome's window class,
// Chrome_WidgetWin_1, is shared by every Electron and CEF application on the
// machine — the Claude desktop app answers to it here — so class alone picks
// whichever of them happens to be largest.
// `requireFullscreen` skips every window that is not IsFullscreenWindow — the
// owner's rule until the video rect comes from the page (P11). `verbose` controls
// the per-candidate explanation of a miss, which is noise when the miss is the
// normal state of a background tool polling once a second.
bool FindTarget(const std::wstring& cls, const std::wstring& titleSub, DWORD pid,
                TargetWindow* out, const std::wstring& exeSub = std::wstring(),
                bool requireFullscreen = false, bool verbose = true);

// Is any other top-level window above `target` in z-order visibly covering any
// part of `videoScreen`? The owner's rule (2026-09-13) is that generation pauses
// while ANY window covers even part of the video. The overlay sits one slot above
// the target, so the covering window is already drawn correctly over it; this
// decides whether to keep generating underneath. `ignore` is exempted (our own
// overlay). If `who` is non-null it is filled with the first occluder found.
// `ignoreExes` (lower-case image names, e.g. L"quotty.exe") are never occluders.
// `ignoreWidgets` (owner 2026-09-13, the default) exempts WIDGETS: always-on-top
// windows no bigger than a tenth of the picture — a resident quote ticker, a
// clock, a toast, a tooltip, a picture-in-picture player. They sit above the
// browser and so above our overlay, so they are drawn correctly either way; what
// the exemption changes is only that generation keeps running around them. The
// first one skipped is reported through `widget`.
bool VideoRectObscured(HWND target, const RECT& videoScreen, HWND ignore,
                       OccluderInfo* who = nullptr,
                       const std::vector<std::wstring>* ignoreExes = nullptr,
                       bool ignoreWidgets = false, OccluderInfo* widget = nullptr);

}  // namespace nsp
