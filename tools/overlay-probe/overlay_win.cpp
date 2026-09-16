// overlay_win.cpp — the overlay HWND + DirectComposition visual + composition
// swapchain used by spike S-M2.
//
// Invariants enforced here (see overlay_win.h):
//   I9  WS_POPUP always; WS_EX_TRANSPARENT unless Options::styleTransparent is
//       deliberately cleared for the negative control.
//   I11 the HWND is created once at `hwndRectScreen` and is NEVER moved or
//       resized afterwards. Only ShowWindow(SW_SHOWNA)/SW_HIDE touch it again;
//       all geometry lives in the DComp visual (offset + rectangle clip).
//   D13 the window is created WITHOUT WS_VISIBLE — the probe starts hidden.
//
// Every HRESULT is either fatal (Create fills *err) or recorded: creation-time
// results go into CreationReport(), per-frame ones are logged once per distinct
// HRESULT so a 10-minute run cannot drown in identical lines.

#include "overlay_win.h"

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace np {
namespace {

// ------------------------------------------------------------------ constants

constexpr wchar_t kOverlayClassName[] = L"NovaSilkOverlayProbe";
constexpr wchar_t kOverlayWindowText[] = L"Nova SilkPlay overlay probe";

// Width of the animated marker bar, in swapchain pixels.
constexpr LONG kBarWidthPx = 64;
// Pixels the marker bar advances per presented frame.
constexpr LONG kBarStepPx = 8;
// Upper bound on the wait for the frame-latency waitable object. This is the
// SAME thread that afterwards drains the WGC frame pool, so every millisecond
// spent here is a millisecond capture is not polled: one long wait would starve
// capture and corrupt the capture-side fps/duplicate statistics. The bound is
// therefore ~two 165 Hz refresh intervals (2 x 6.06 ms), not a comfortable
// 100 ms. Waits that are not signalled are counted and reported, never silent.
constexpr DWORD kWaitableTimeoutMs = 12;

// ------------------------------------------------------------------- printing

void AppendFmt(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) {
        out.append(buf, static_cast<size_t>(n));
    } else if (n < 0) {
        // _TRUNCATE: buffer is still NUL-terminated, the text was just cut.
        buf[sizeof(buf) - 1] = '\0';
        out.append(buf);
    }
}

const char* BoolStr(bool b) { return b ? "yes" : "no"; }

const char* FeatureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        default: return "other";
    }
}

const char* SwapEffectName(DXGI_SWAP_EFFECT e) {
    switch (e) {
        case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD(bitblt)";
        case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL(bitblt)";
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
        case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
        default: return "unknown";
    }
}

const char* AlphaModeName(DXGI_ALPHA_MODE m) {
    switch (m) {
        case DXGI_ALPHA_MODE_UNSPECIFIED: return "UNSPECIFIED";
        case DXGI_ALPHA_MODE_PREMULTIPLIED: return "PREMULTIPLIED";
        case DXGI_ALPHA_MODE_STRAIGHT: return "STRAIGHT";
        case DXGI_ALPHA_MODE_IGNORE: return "IGNORE";
        default: return "unknown";
    }
}

const char* ScalingName(DXGI_SCALING s) {
    switch (s) {
        case DXGI_SCALING_STRETCH: return "STRETCH";
        case DXGI_SCALING_NONE: return "NONE";
        case DXGI_SCALING_ASPECT_RATIO_STRETCH: return "ASPECT_RATIO_STRETCH";
        default: return "unknown";
    }
}

const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE c) {
    switch (c) {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709 (sRGB)";
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709 (scRGB)";
        case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709: return "RGB_STUDIO_G22_NONE_P709";
        case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020: return "RGB_STUDIO_G22_NONE_P2020";
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020 (HDR10)";
        default: return "other";
    }
}

const char* RotationName(DXGI_MODE_ROTATION r) {
    switch (r) {
        case DXGI_MODE_ROTATION_IDENTITY: return "IDENTITY";
        case DXGI_MODE_ROTATION_ROTATE90: return "ROTATE90";
        case DXGI_MODE_ROTATION_ROTATE180: return "ROTATE180";
        case DXGI_MODE_ROTATION_ROTATE270: return "ROTATE270";
        default: return "UNSPECIFIED";
    }
}

// ------------------------------------------------------------------- geometry

LONG RectWidth(const RECT& r) { return r.right - r.left; }
LONG RectHeight(const RECT& r) { return r.bottom - r.top; }

// Area of the intersection of a and b, saturating at LONG_MAX-ish values is not
// a concern: desktop rects are far below 2^31 in area on any supported config.
long long IntersectArea(const RECT& a, const RECT& b) {
    const LONG l = (std::max)(a.left, b.left);
    const LONG t = (std::max)(a.top, b.top);
    const LONG r = (std::min)(a.right, b.right);
    const LONG bo = (std::min)(a.bottom, b.bottom);
    if (r <= l || bo <= t) return 0;
    return static_cast<long long>(r - l) * static_cast<long long>(bo - t);
}

// ------------------------------------------------------- display-mode lookup

// Exact refresh rate as the display path reports it (165/1 rather than the
// rounded 165 that EnumDisplaySettings hands back). Returns false when the GDI
// device name has no active path.
bool RationalRefreshHz(const wchar_t* gdiDeviceName, double* outHz) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    if (pathCount == 0) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount ? modeCount : 1);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return false;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(src.viewGdiDeviceName, gdiDeviceName) != 0) continue;

        const DISPLAYCONFIG_RATIONAL& rr = paths[i].targetInfo.refreshRate;
        if (rr.Denominator != 0) {
            *outHz = static_cast<double>(rr.Numerator) / static_cast<double>(rr.Denominator);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------- wndproc

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            // Belt and braces next to WS_EX_TRANSPARENT: never claim the cursor.
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            // No redirection surface — GDI must not paint anything here.
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ------------------------------------------------- dcomp entry-point binding

using PFN_DCompCreateDevice3 = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
using PFN_DCompCreateDevice2 = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
using PFN_DCompCreateDevice1 = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);

}  // namespace

// ============================================================================
//                                    Impl
// ============================================================================

struct Overlay::Impl {
    Options opt;

    HWND      hwnd = nullptr;
    HWND      target = nullptr;
    RECT      hwndRect = {};          // frozen at creation (I11)
    RECT      committedVisual = {};   // what SetVisualRect last put on screen
    HINSTANCE hinst = nullptr;
    bool      ownsClass = false;
    bool      shown = false;

    ComPtr<ID3D11Device>         device;
    ComPtr<ID3D11DeviceContext>  ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    D3D_FEATURE_LEVEL            featureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
    bool                         clearViewSupported = false;

    ComPtr<IDXGISwapChain1>    swap;
    ComPtr<IDXGISwapChain2>    swap2;
    ComPtr<IDXGISwapChainMedia> media;
    HANDLE                     waitable = nullptr;
    UINT                       scWidth = 0;
    UINT                       scHeight = 0;

    HMODULE                            dcompDll = nullptr;
    ComPtr<IDCompositionDevice>        devV1;       // legacy fallback path
    ComPtr<IDCompositionDesktopDevice> devDesktop;  // preferred (device2/3)
    ComPtr<IDCompositionTarget>        dcompTarget;
    ComPtr<IDCompositionVisual>        visual;
    ComPtr<IDCompositionRectangleClip> clip;

    // RTVs cached per back-buffer texture pointer. The cached RTV holds a
    // reference to its texture, so a key pointer can never be recycled by a
    // different resource while it is still in this table.
    struct RtvEntry {
        ID3D11Texture2D*              key = nullptr;
        ComPtr<ID3D11RenderTargetView> rtv;
    };
    std::vector<RtvEntry> rtvs;

    std::string report;
    // Assembled on demand by RunReport(); mutable so a const accessor can
    // refresh it without pretending the counters are const.
    mutable std::string runReport;

    // Last statistics that actually arrived. DISJOINT/WAS_STILL_DRAWING frames
    // reuse these instead of zeroing the sample.
    DXGI_FRAME_STATISTICS lastStats = {};
    int      lastCompositionMode = -1;
    uint32_t lastApprovedPresentDuration = 0;

    // One log line per distinct HRESULT, not per frame.
    HRESULT loggedStatsHr = S_OK;
    HRESULT loggedMediaHr = S_OK;
    HRESULT loggedPresentHr = S_OK;
    HRESULT loggedBufferHr = S_OK;
    HRESULT loggedRtvHr = S_OK;
    bool    loggedClearViewMissing = false;

    // Per-run counters. A silently-wrong number is worse than a crash, so every
    // frame that did NOT produce a real present is counted, never averaged in.
    uint64_t presentsOk = 0;
    uint64_t presentFailures = 0;   // Present1 returned a non-device-loss failure
    uint64_t presentSkips = 0;      // frame never reached Present1 (GetBuffer/RTV)
    uint64_t waitCount = 0;         // frame-latency waits performed
    uint64_t waitNotSignaled = 0;   // ... of which did not return WAIT_OBJECT_0
    double   maxWaitMs = 0.0;       // worst wall time spent in that wait
    // True when the marker bar is unavailable and the alternating-colour
    // full-surface clear stands in for it (recorded at creation).
    bool     clearViewFallback = false;

    HRESULT Commit() {
        if (devDesktop) return devDesktop->Commit();
        if (devV1) return devV1->Commit();
        return E_POINTER;
    }

    const std::string& BuildRunReport() const;
    void Destroy();
};

void Overlay::Impl::Destroy() {
    // Last chance to state what the run actually did: if the caller never asked
    // for RunReport(), the failure/skip counts would otherwise die with the
    // object and the run would look cleaner than it was.
    if (presentsOk || presentFailures || presentSkips) {
        std::string r = BuildRunReport();
        while (!r.empty() && r.back() == '\n') r.pop_back();
        LogErr("%s", r.c_str());
    }

    // DComp first: drop the tree before the swapchain it points at.
    if (visual) {
        visual->SetContent(nullptr);
        visual->SetClip(static_cast<IDCompositionClip*>(nullptr));
    }
    if (dcompTarget) dcompTarget->SetRoot(nullptr);
    if (devDesktop || devV1) {
        const HRESULT hr = Commit();
        if (FAILED(hr)) LogErr("Overlay::Destroy: final Commit failed %s\n", HrString(hr).c_str());
    }
    clip.Reset();
    visual.Reset();
    dcompTarget.Reset();
    devDesktop.Reset();
    devV1.Reset();

    rtvs.clear();
    media.Reset();
    swap2.Reset();
    if (waitable) {
        CloseHandle(waitable);
        waitable = nullptr;
    }
    swap.Reset();

    if (ctx) {
        ctx->ClearState();
        ctx->Flush();
    }
    ctx1.Reset();
    ctx.Reset();
    device.Reset();

    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (ownsClass) {
        if (!UnregisterClassW(kOverlayClassName, hinst))
            LogErr("Overlay::Destroy: UnregisterClass failed, GetLastError=%lu\n", GetLastError());
        ownsClass = false;
    }
    // dcompDll is deliberately left loaded: unloading it after the last DComp
    // object is released buys nothing and risks unloading under a worker.
    dcompDll = nullptr;
}

// ============================================================================
//                                  Overlay
// ============================================================================

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}

Overlay::~Overlay() {
    if (impl_) impl_->Destroy();
}

HWND Overlay::Hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

const std::string& Overlay::CreationReport() const { return impl_->report; }

const RECT& Overlay::CommittedVisualRect() const { return impl_->committedVisual; }

const std::string& Overlay::RunReport() const { return impl_->BuildRunReport(); }

const std::string& Overlay::Impl::BuildRunReport() const {
    const Impl& d = *this;
    std::string& rep = d.runReport;
    rep.clear();
    AppendFmt(rep, "=== Overlay run report ===\n");
    AppendFmt(rep,
              "  presents: %llu ok, %llu FAILED (Present1 error, sample invalidated), "
              "%llu SKIPPED (no back buffer, sample invalidated)\n",
              static_cast<unsigned long long>(d.presentsOk),
              static_cast<unsigned long long>(d.presentFailures),
              static_cast<unsigned long long>(d.presentSkips));
    if (d.waitable) {
        AppendFmt(rep,
                  "  frame-latency wait: %llu waits, %llu not signalled within %lu ms, "
                  "worst observed %.3f ms\n",
                  static_cast<unsigned long long>(d.waitCount),
                  static_cast<unsigned long long>(d.waitNotSignaled),
                  static_cast<unsigned long>(kWaitableTimeoutMs), d.maxWaitMs);
    } else {
        AppendFmt(rep, "  frame-latency wait: no waitable object (presents are unthrottled)\n");
    }
    AppendFmt(rep, "  marker bar: %s\n",
              d.clearViewFallback ? "ClearView UNAVAILABLE - full-surface alternating clear instead "
                                    "(frames still change, but the bar position is gone)"
                                  : "ClearView");
    return rep;
}

bool Overlay::IsShown() const { return impl_ && impl_->shown; }

void Overlay::Show() {
    if (!impl_ || !impl_->hwnd) return;
    // SW_SHOWNA: show without activating. Never SW_SHOW (I11 + no focus steal).
    ShowWindow(impl_->hwnd, SW_SHOWNA);
    impl_->shown = true;
}

void Overlay::Hide() {
    if (!impl_ || !impl_->hwnd) return;
    // Hide only. Nothing is destroyed: the swapchain and DComp tree survive so a
    // later Show() costs nothing and the measurement is not polluted by teardown.
    ShowWindow(impl_->hwnd, SW_HIDE);
    impl_->shown = false;
}

// ----------------------------------------------------------------- Create()

bool Overlay::Create(const Options& opt, HWND target, const RECT& hwndRectScreen, std::string* err) {
    Impl& d = *impl_;
    d.opt = opt;
    d.target = target;
    d.hwndRect = hwndRectScreen;

    // The report is assembled INCREMENTALLY, as each fact is learned: a Create()
    // that fails half-way is exactly when the DPI note, the DComp HRESULTs and
    // the swapchain QI notes are needed, so they must already be in the report
    // by the time an early return happens.
    std::string& rep = d.report;
    rep.clear();
    AppendFmt(rep, "=== Overlay creation report ===\n");

    auto fail = [&](const char* what, HRESULT hr) -> bool {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s: %s", what, HrString(hr).c_str());
        AppendFmt(rep, "  *** CREATE FAILED: %s ***\n", buf);
        if (err) *err = buf;
        return false;
    };
    auto failMsg = [&](const char* what) -> bool {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (GetLastError=%lu)", what, GetLastError());
        AppendFmt(rep, "  *** CREATE FAILED: %s ***\n", buf);
        if (err) *err = buf;
        return false;
    };

    const LONG wL = RectWidth(hwndRectScreen);
    const LONG hL = RectHeight(hwndRectScreen);
    if (wL <= 0 || hL <= 0) {
        AppendFmt(rep, "  requested rect %ld,%ld %ldx%ld\n", hwndRectScreen.left,
                  hwndRectScreen.top, wL, hL);
        AppendFmt(rep, "  *** CREATE FAILED: target rect is empty ***\n");
        if (err) *err = "Create: target rect is empty";
        return false;
    }
    d.scWidth = static_cast<UINT>(wL);
    d.scHeight = static_cast<UINT>(hL);

    // ---- DPI awareness: every rect in this probe is physical pixels.
    // Failure is expected and harmless when a manifest already set the context.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        const DWORD gle = GetLastError();
        AppendFmt(rep, "  DPI: SetProcessDpiAwarenessContext(PMv2) failed, GetLastError=%lu%s\n",
                  gle, gle == ERROR_ACCESS_DENIED ? " (already set - fine if it was set to PMv2)"
                                                  : "");
    } else {
        AppendFmt(rep, "  DPI: SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) ok\n");
    }

    // ---- window class
    d.hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = 0;  // no CS_HREDRAW/CS_VREDRAW: the window never resizes (I11)
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = d.hinst;
    wc.hCursor = nullptr;        // click-through: never set a cursor
    wc.hbrBackground = nullptr;  // no redirection surface, nothing for GDI to fill
    wc.lpszClassName = kOverlayClassName;
    if (RegisterClassExW(&wc) == 0) {
        const DWORD gle = GetLastError();
        if (gle != ERROR_CLASS_ALREADY_EXISTS) return failMsg("RegisterClassExW failed");
        d.ownsClass = false;  // someone else owns it; do not unregister on exit
    } else {
        d.ownsClass = true;
    }

    // ---- the window itself
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (opt.styleTransparent) exStyle |= WS_EX_TRANSPARENT;  // I9, the flag under test
    if (opt.styleTopmost) exStyle |= WS_EX_TOPMOST;
    const DWORD style = WS_POPUP;  // NOT WS_VISIBLE: D13 starts hidden

    d.hwnd = CreateWindowExW(exStyle, kOverlayClassName, kOverlayWindowText, style,
                             hwndRectScreen.left, hwndRectScreen.top, static_cast<int>(wL),
                             static_cast<int>(hL), nullptr, nullptr, d.hinst, nullptr);
    if (!d.hwnd) return failMsg("CreateWindowExW failed");
    // From here on the HWND geometry is frozen: no SetWindowPos, no MoveWindow.

    // ---- report: window styles, read back rather than assumed
    const LONG_PTR gotStyle = GetWindowLongPtrW(d.hwnd, GWL_STYLE);
    const LONG_PTR gotEx = GetWindowLongPtrW(d.hwnd, GWL_EXSTYLE);
    AppendFmt(rep, "  HWND: 0x%p  requested rect %ld,%ld %ldx%ld\n", static_cast<void*>(d.hwnd),
              hwndRectScreen.left, hwndRectScreen.top, wL, hL);
    RECT actual = {};
    if (GetWindowRect(d.hwnd, &actual))
        AppendFmt(rep, "  GetWindowRect: %ld,%ld %ldx%ld\n", actual.left, actual.top,
                  RectWidth(actual), RectHeight(actual));
    else
        AppendFmt(rep, "  GetWindowRect FAILED, GetLastError=%lu\n", GetLastError());
    AppendFmt(rep, "  GWL_STYLE   = 0x%08lX  WS_POPUP=%s WS_VISIBLE=%s\n",
              static_cast<unsigned long>(gotStyle), BoolStr((gotStyle & WS_POPUP) != 0),
              BoolStr((gotStyle & WS_VISIBLE) != 0));
    AppendFmt(rep,
              "  GWL_EXSTYLE = 0x%08lX  NOREDIRECTIONBITMAP=%s TRANSPARENT=%s TOPMOST=%s "
              "TOOLWINDOW=%s NOACTIVATE=%s\n",
              static_cast<unsigned long>(gotEx),
              BoolStr((gotEx & WS_EX_NOREDIRECTIONBITMAP) != 0),
              BoolStr((gotEx & WS_EX_TRANSPARENT) != 0), BoolStr((gotEx & WS_EX_TOPMOST) != 0),
              BoolStr((gotEx & WS_EX_TOOLWINDOW) != 0), BoolStr((gotEx & WS_EX_NOACTIVATE) != 0));
    AppendFmt(rep, "  requested: styleTransparent=%s styleTopmost=%s insetPx=%d\n",
              BoolStr(opt.styleTransparent), BoolStr(opt.styleTopmost), opt.insetPx);
    if (opt.styleTransparent != ((gotEx & WS_EX_TRANSPARENT) != 0))
        AppendFmt(rep, "  *** WS_EX_TRANSPARENT MISMATCH - I9 check failed ***\n");

    // ---- report: target window
    if (d.target) {
        wchar_t cls[128] = {};
        GetClassNameW(d.target, cls, ARRAYSIZE(cls));
        RECT trc = {}, tcl = {};
        GetWindowRect(d.target, &trc);
        GetClientRect(d.target, &tcl);
        DWORD tpid = 0;
        GetWindowThreadProcessId(d.target, &tpid);
        AppendFmt(rep, "  target HWND 0x%p class=%s pid=%lu window=%ld,%ld %ldx%ld client=%ldx%ld\n",
                  static_cast<void*>(d.target), Narrow(cls).c_str(), tpid, trc.left, trc.top,
                  RectWidth(trc), RectHeight(trc), RectWidth(tcl), RectHeight(tcl));
    } else {
        AppendFmt(rep, "  target HWND: none supplied\n");
    }

    // ---- D3D11 device (11_1 preferred, 11_0 accepted)
    const D3D_FEATURE_LEVEL want11_1[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_FEATURE_LEVEL want11_0[] = {D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for DComp/D2D interop
#ifdef NOVA_D3D_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want11_1,
                                   ARRAYSIZE(want11_1), D3D11_SDK_VERSION, &d.device,
                                   &d.featureLevel, &d.ctx);
    if (hr == E_INVALIDARG) {
        // Runtimes that reject the 11_1 enumerator outright.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want11_0,
                               ARRAYSIZE(want11_0), D3D11_SDK_VERSION, &d.device, &d.featureLevel,
                               &d.ctx);
    }
    if (FAILED(hr)) return fail("D3D11CreateDevice failed", hr);

    const HRESULT hrCtx1 = d.ctx.As(&d.ctx1);
    if (FAILED(hrCtx1))
        LogErr("Overlay::Create: ID3D11DeviceContext1 unavailable %s (marker bar disabled)\n",
               HrString(hrCtx1).c_str());

    D3D11_FEATURE_DATA_D3D11_OPTIONS o11 = {};
    const HRESULT hrOpts =
        d.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o11, sizeof(o11));
    d.clearViewSupported = SUCCEEDED(hrOpts) && o11.ClearView != FALSE;
    // The marker bar needs BOTH: the ID3D11DeviceContext1 that owns ClearView and
    // the driver actually supporting it. If either is missing the render path
    // falls back to a full-surface alternating clear, which keeps the frame
    // changing every present (the capture half's staleness detector) but loses
    // the bar's position.
    d.clearViewFallback = !(d.ctx1 && d.clearViewSupported);

    // ---- report: device
    AppendFmt(rep, "  D3D11 feature level: %s\n", FeatureLevelName(d.featureLevel));
    AppendFmt(rep, "  ID3D11DeviceContext1: %s   ClearView support: %s%s\n",
              BoolStr(d.ctx1 != nullptr), BoolStr(d.clearViewSupported),
              SUCCEEDED(hrOpts) ? "" : " (CheckFeatureSupport(D3D11_OPTIONS) failed)");
    if (d.clearViewFallback)
        AppendFmt(rep,
                  "  marker bar: DISABLED (%s) - falling back to a full-surface alternating "
                  "clear; frames still change every present, the bar position does not\n",
                  !d.ctx1 ? "no ID3D11DeviceContext1" : "driver reports ClearView unsupported");

    // ---- the factory that owns this device's adapter
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d.device.As(&dxgiDevice);
    if (FAILED(hr)) return fail("QI IDXGIDevice failed", hr);

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return fail("IDXGIDevice::GetAdapter failed", hr);

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("IDXGIAdapter::GetParent(IDXGIFactory2) failed", hr);

    // ---- report: adapter
    ComPtr<IDXGIAdapter1> adapter1;
    if (SUCCEEDED(adapter.As(&adapter1))) {
        DXGI_ADAPTER_DESC1 ad = {};
        const HRESULT hrAd = adapter1->GetDesc1(&ad);
        if (SUCCEEDED(hrAd)) {
            AppendFmt(rep,
                      "  adapter: %s  vendor=0x%04X device=0x%04X dedicatedVRAM=%llu MiB "
                      "sharedSys=%llu MiB\n",
                      Narrow(ad.Description).c_str(), ad.VendorId, ad.DeviceId,
                      static_cast<unsigned long long>(ad.DedicatedVideoMemory >> 20),
                      static_cast<unsigned long long>(ad.SharedSystemMemory >> 20));
        } else {
            AppendFmt(rep, "  adapter: GetDesc1 FAILED %s\n", HrString(hrAd).c_str());
        }
    }

    // ---- report: the output the window landed on
    {
        const RECT winRect = actual.right > actual.left ? actual : hwndRectScreen;
        long long bestArea = 0;
        ComPtr<IDXGIOutput> bestOutput;
        std::wstring bestAdapterName;
        std::string enumNotes;

        ComPtr<IDXGIFactory1> factory1;
        if (SUCCEEDED(factory.As(&factory1))) {
            ComPtr<IDXGIAdapter1> a;
            for (UINT ai = 0;; ++ai) {
                // Break on ANY failure, not just NOT_FOUND: operator& is
                // ReleaseAndGetAddressOf, so another HRESULT leaves `a` null and
                // the old "!= DXGI_ERROR_NOT_FOUND" loop dereferenced null forever.
                const HRESULT hrEnum = factory1->EnumAdapters1(ai, &a);
                if (hrEnum == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hrEnum) || !a) {
                    AppendFmt(enumNotes,
                              "    EnumAdapters1(%u) FAILED %s - adapter enumeration stopped\n", ai,
                              HrString(hrEnum).c_str());
                    break;
                }
                DXGI_ADAPTER_DESC1 ad = {};
                if (FAILED(a->GetDesc1(&ad))) ad.Description[0] = L'\0';
                ComPtr<IDXGIOutput> o;
                for (UINT oi = 0;; ++oi) {
                    const HRESULT hrOut = a->EnumOutputs(oi, &o);
                    if (hrOut == DXGI_ERROR_NOT_FOUND) break;
                    if (FAILED(hrOut) || !o) {
                        AppendFmt(enumNotes,
                                  "    EnumOutputs(%u) on adapter %u FAILED %s - output "
                                  "enumeration stopped\n",
                                  oi, ai, HrString(hrOut).c_str());
                        break;
                    }
                    DXGI_OUTPUT_DESC od = {};
                    if (SUCCEEDED(o->GetDesc(&od))) {
                        const long long area = IntersectArea(winRect, od.DesktopCoordinates);
                        if (area > bestArea) {
                            bestArea = area;
                            bestOutput = o;
                            bestAdapterName = ad.Description;
                        }
                    }
                    o.Reset();
                }
                a.Reset();
            }
        } else {
            AppendFmt(enumNotes, "    QI IDXGIFactory1 FAILED - outputs not enumerated\n");
        }

        if (!bestOutput) {
            AppendFmt(rep, "  output: window rect intersects NO enumerated output\n");
            rep += enumNotes;
        } else {
            DXGI_OUTPUT_DESC od = {};
            bestOutput->GetDesc(&od);
            AppendFmt(rep, "  output: %s on %s  desktop=%ld,%ld %ldx%ld rotation=%s attached=%s "
                           "(overlap %lld px)\n",
                      Narrow(od.DeviceName).c_str(), Narrow(bestAdapterName).c_str(),
                      od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                      RectWidth(od.DesktopCoordinates), RectHeight(od.DesktopCoordinates),
                      RotationName(od.Rotation), BoolStr(od.AttachedToDesktop != FALSE), bestArea);
            rep += enumNotes;

            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                AppendFmt(rep, "    mode: %lux%lu @ %lu Hz (EnumDisplaySettings), %lu bpp\n",
                          dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel);
            } else {
                AppendFmt(rep, "    mode: EnumDisplaySettings FAILED, GetLastError=%lu\n",
                          GetLastError());
            }
            double hz = 0.0;
            if (RationalRefreshHz(od.DeviceName, &hz))
                AppendFmt(rep, "    refresh (QueryDisplayConfig): %.3f Hz\n", hz);
            else
                AppendFmt(rep, "    refresh (QueryDisplayConfig): unavailable\n");

            ComPtr<IDXGIOutput6> o6;
            const HRESULT hrO6 = bestOutput.As(&o6);
            if (SUCCEEDED(hrO6)) {
                DXGI_OUTPUT_DESC1 od1 = {};
                const HRESULT hrD1 = o6->GetDesc1(&od1);
                if (SUCCEEDED(hrD1)) {
                    AppendFmt(rep,
                              "    DESC1: bitsPerColor=%u colorSpace=%d %s minLum=%.4f maxLum=%.1f "
                              "maxFullFrameLum=%.1f\n",
                              od1.BitsPerColor, static_cast<int>(od1.ColorSpace),
                              ColorSpaceName(od1.ColorSpace), static_cast<double>(od1.MinLuminance),
                              static_cast<double>(od1.MaxLuminance),
                              static_cast<double>(od1.MaxFullFrameLuminance));
                } else {
                    AppendFmt(rep, "    DESC1: GetDesc1 FAILED %s\n", HrString(hrD1).c_str());
                }
                UINT hwFlags = 0;
                const HRESULT hrHw = o6->CheckHardwareCompositionSupport(&hwFlags);
                if (SUCCEEDED(hrHw)) {
                    AppendFmt(rep, "    CheckHardwareCompositionSupport: 0x%X%s%s%s\n", hwFlags,
                              (hwFlags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN)
                                  ? " FULLSCREEN"
                                  : "",
                              (hwFlags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED)
                                  ? " WINDOWED"
                                  : "",
                              (hwFlags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED)
                                  ? " CURSOR_STRETCHED"
                                  : "");
                } else {
                    AppendFmt(rep, "    CheckHardwareCompositionSupport FAILED %s\n",
                              HrString(hrHw).c_str());
                }
            } else {
                AppendFmt(rep, "    IDXGIOutput6 unavailable %s\n", HrString(hrO6).c_str());
            }
        }
    }

    BOOL dwmOn = FALSE;
    const HRESULT hrDwm = DwmIsCompositionEnabled(&dwmOn);
    if (SUCCEEDED(hrDwm))
        AppendFmt(rep, "  DwmIsCompositionEnabled: %s\n", BoolStr(dwmOn != FALSE));
    else
        AppendFmt(rep, "  DwmIsCompositionEnabled FAILED %s\n", HrString(hrDwm).c_str());

    // ---- composition swapchain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = d.scWidth;
    scd.Height = d.scHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.Stereo = FALSE;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    scd.BufferCount = 3;
    scd.Scaling = DXGI_SCALING_STRETCH;  // required by CreateSwapChainForComposition
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = factory->CreateSwapChainForComposition(d.device.Get(), &scd, nullptr, &d.swap);
    if (FAILED(hr)) return fail("CreateSwapChainForComposition failed", hr);

    std::string swapNotes;
    hr = d.swap.As(&d.swap2);
    if (FAILED(hr)) {
        AppendFmt(swapNotes, "    QI IDXGISwapChain2 FAILED %s (frame latency left at default)\n",
                  HrString(hr).c_str());
    } else {
        const HRESULT hrLat = d.swap2->SetMaximumFrameLatency(1);
        if (FAILED(hrLat))
            AppendFmt(swapNotes, "    SetMaximumFrameLatency(1) FAILED %s\n", HrString(hrLat).c_str());
        d.waitable = d.swap2->GetFrameLatencyWaitableObject();
        if (!d.waitable)
            AppendFmt(swapNotes, "    GetFrameLatencyWaitableObject returned NULL\n");
    }

    const HRESULT hrMedia = d.swap.As(&d.media);
    if (FAILED(hrMedia))
        AppendFmt(swapNotes, "    QI IDXGISwapChainMedia FAILED %s (compositionMode stays -1)\n",
                  HrString(hrMedia).c_str());

    // ---- report: swapchain, read back rather than assumed
    DXGI_SWAP_CHAIN_DESC1 got = {};
    const HRESULT hrDesc = d.swap->GetDesc1(&got);
    if (SUCCEEDED(hrDesc)) {
        AppendFmt(rep,
                  "  swapchain: %ux%u fmt=%d(BGRA8=%d) buffers=%u effect=%s alpha=%s scaling=%s "
                  "flags=0x%X waitable=%s\n",
                  got.Width, got.Height, static_cast<int>(got.Format),
                  static_cast<int>(DXGI_FORMAT_B8G8R8A8_UNORM), got.BufferCount,
                  SwapEffectName(got.SwapEffect), AlphaModeName(got.AlphaMode),
                  ScalingName(got.Scaling), got.Flags, BoolStr(d.waitable != nullptr));
    } else {
        AppendFmt(rep, "  swapchain: GetDesc1 FAILED %s\n", HrString(hrDesc).c_str());
    }
    AppendFmt(rep, "  IDXGISwapChainMedia: %s\n", BoolStr(d.media != nullptr));
    AppendFmt(rep, "  frame-latency wait timeout: %lu ms\n",
              static_cast<unsigned long>(kWaitableTimeoutMs));
    rep += swapNotes;

    // No MakeWindowAssociation: a composition swapchain has no HWND of its own,
    // and the probe must not install DXGI's alt-enter handling on the overlay.

    // ---- DirectComposition device: 3 -> 2 -> 1
    std::string dcompNotes;
    d.dcompDll = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d.dcompDll) return failMsg("LoadLibrary(dcomp.dll) failed");

    auto create3 = reinterpret_cast<PFN_DCompCreateDevice3>(
        GetProcAddress(d.dcompDll, "DCompositionCreateDevice3"));
    auto create2 = reinterpret_cast<PFN_DCompCreateDevice2>(
        GetProcAddress(d.dcompDll, "DCompositionCreateDevice2"));
    auto create1 = reinterpret_cast<PFN_DCompCreateDevice1>(
        GetProcAddress(d.dcompDll, "DCompositionCreateDevice"));

    HRESULT hrDcomp = E_NOINTERFACE;
    const char* dcompPath = "none";
    if (create3) {
        hrDcomp = create3(dxgiDevice.Get(), IID_PPV_ARGS(&d.devDesktop));
        AppendFmt(dcompNotes, "    DCompositionCreateDevice3 -> %s\n", HrString(hrDcomp).c_str());
        if (SUCCEEDED(hrDcomp)) dcompPath = "DCompositionCreateDevice3";
    } else {
        AppendFmt(dcompNotes, "    DCompositionCreateDevice3 export MISSING\n");
    }
    if (!d.devDesktop && create2) {
        hrDcomp = create2(dxgiDevice.Get(), IID_PPV_ARGS(&d.devDesktop));
        AppendFmt(dcompNotes, "    DCompositionCreateDevice2 -> %s\n", HrString(hrDcomp).c_str());
        if (SUCCEEDED(hrDcomp)) dcompPath = "DCompositionCreateDevice2";
    }
    if (!d.devDesktop && create1) {
        hrDcomp = create1(dxgiDevice.Get(), IID_PPV_ARGS(&d.devV1));
        AppendFmt(dcompNotes, "    DCompositionCreateDevice  -> %s\n", HrString(hrDcomp).c_str());
        if (SUCCEEDED(hrDcomp)) {
            dcompPath = "DCompositionCreateDevice";
            // The object usually implements the desktop interface too; prefer it.
            const HRESULT hrQi = d.devV1.As(&d.devDesktop);
            AppendFmt(dcompNotes, "    QI IDCompositionDesktopDevice -> %s\n",
                      HrString(hrQi).c_str());
        }
    }
    // Recorded BEFORE the failure check: a run that cannot get a DComp device is
    // exactly the run whose three DCompositionCreateDevice HRESULTs matter.
    AppendFmt(rep, "  DComp device: %s (desktopDevice=%s legacyDevice=%s)\n", dcompPath,
              BoolStr(d.devDesktop != nullptr), BoolStr(d.devV1 != nullptr));
    rep += dcompNotes;

    if (!d.devDesktop && !d.devV1) return fail("no DirectComposition device could be created", hrDcomp);

    // ---- target / visual / clip
    if (d.devDesktop) {
        hr = d.devDesktop->CreateTargetForHwnd(d.hwnd, TRUE, &d.dcompTarget);
    } else {
        hr = d.devV1->CreateTargetForHwnd(d.hwnd, TRUE, &d.dcompTarget);
    }
    if (FAILED(hr)) return fail("CreateTargetForHwnd failed", hr);

    if (d.devDesktop) {
        ComPtr<IDCompositionVisual2> v2;
        hr = d.devDesktop->CreateVisual(&v2);
        if (SUCCEEDED(hr)) d.visual = v2;
    } else {
        hr = d.devV1->CreateVisual(&d.visual);
    }
    if (FAILED(hr) || !d.visual) return fail("CreateVisual failed", hr);

    hr = d.visual->SetContent(d.swap.Get());
    if (FAILED(hr)) return fail("IDCompositionVisual::SetContent failed", hr);

    hr = d.dcompTarget->SetRoot(d.visual.Get());
    if (FAILED(hr)) return fail("IDCompositionTarget::SetRoot failed", hr);

    // Initial geometry: the visual covers the whole window. The INSET IS NOT
    // APPLIED HERE and must not be applied by the caller either - SetVisualRect
    // owns Options::insetPx and applies it exactly once, so this rect is the
    // full, un-inset window rect (see CommittedVisualRect() for what landed).
    if (!SetVisualRect(hwndRectScreen)) {
        AppendFmt(rep, "  *** CREATE FAILED: initial SetVisualRect failed (see log) ***\n");
        if (err) *err = "initial SetVisualRect failed (see log)";
        return false;
    }
    AppendFmt(rep,
              "  visual rect committed: %ld,%ld %ldx%ld (requested %ld,%ld %ldx%ld, inset %d px)\n",
              d.committedVisual.left, d.committedVisual.top, RectWidth(d.committedVisual),
              RectHeight(d.committedVisual), hwndRectScreen.left, hwndRectScreen.top, wL, hL,
              opt.insetPx);

    hr = d.Commit();
    if (FAILED(hr)) return fail("IDCompositionDevice::Commit failed", hr);

    AppendFmt(rep, "  visible after Create: %s (D13 expects no)\n",
              BoolStr(IsWindowVisible(d.hwnd) != FALSE));
    return true;
}

// ------------------------------------------------------------ SetVisualRect()

bool Overlay::SetVisualRect(const RECT& screenRect) {
    Impl& d = *impl_;
    if (!d.visual) {
        LogErr("Overlay::SetVisualRect: no visual\n");
        return false;
    }

    // Screen space -> visual-local space. The HWND itself never moves (I11), so
    // its creation-time rect is the origin of the visual coordinate system.
    LONG offX = screenRect.left - d.hwndRect.left;
    LONG offY = screenRect.top - d.hwndRect.top;
    LONG w = RectWidth(screenRect);
    LONG h = RectHeight(screenRect);
    if (w <= 0 || h <= 0) {
        LogErr("Overlay::SetVisualRect: empty rect %ld,%ld %ldx%ld\n", screenRect.left,
               screenRect.top, w, h);
        return false;
    }

    // THE OVERLAY OWNS THE INSET. `screenRect` is the un-inset rect; the inset is
    // applied here and nowhere else. (It used to be applied by the caller as well,
    // so the visible region was inset by 2N while the report said N.)
    // The clip lives in the visual's own coordinate space (offset already
    // applied), so it starts at 0,0 and the inset shrinks it symmetrically.
    LONG inset = d.opt.insetPx > 0 ? static_cast<LONG>(d.opt.insetPx) : 0;
    if (2 * inset >= w) inset = (w - 1) / 2;
    if (2 * inset >= h) inset = (h - 1) / 2;
    if (inset < 0) inset = 0;

    const HRESULT hrX = d.visual->SetOffsetX(static_cast<float>(offX));
    const HRESULT hrY = d.visual->SetOffsetY(static_cast<float>(offY));
    if (FAILED(hrX) || FAILED(hrY)) {
        LogErr("Overlay::SetVisualRect: SetOffsetX %s / SetOffsetY %s\n", HrString(hrX).c_str(),
               HrString(hrY).c_str());
        return false;
    }

    if (!d.clip) {
        HRESULT hrClip = E_POINTER;
        if (d.devDesktop) {
            hrClip = d.devDesktop->CreateRectangleClip(&d.clip);
        } else if (d.devV1) {
            hrClip = d.devV1->CreateRectangleClip(&d.clip);
        }
        if (FAILED(hrClip) || !d.clip) {
            LogErr("Overlay::SetVisualRect: CreateRectangleClip failed %s\n",
                   HrString(hrClip).c_str());
            return false;
        }
    }

    // Clip edges in visual-local pixels. The swapchain content only exists over
    // [0,scWidth) x [0,scHeight) of that space, so the far edges are clamped to
    // it: a clip reaching past the content would make CommittedVisualRect()
    // claim painted pixels that are not there.
    const LONG clipL = inset;
    const LONG clipT = inset;
    const LONG clipR = (std::min)(w - inset, static_cast<LONG>(d.scWidth));
    const LONG clipB = (std::min)(h - inset, static_cast<LONG>(d.scHeight));
    if (clipR <= clipL || clipB <= clipT) {
        LogErr("Overlay::SetVisualRect: clip degenerated to %ld,%ld,%ld,%ld "
               "(rect %ldx%ld, inset %ld, swapchain %ux%u)\n",
               clipL, clipT, clipR, clipB, w, h, inset, d.scWidth, d.scHeight);
        return false;
    }

    HRESULT hr = d.clip->SetLeft(static_cast<float>(clipL));
    if (SUCCEEDED(hr)) hr = d.clip->SetTop(static_cast<float>(clipT));
    if (SUCCEEDED(hr)) hr = d.clip->SetRight(static_cast<float>(clipR));
    if (SUCCEEDED(hr)) hr = d.clip->SetBottom(static_cast<float>(clipB));
    if (FAILED(hr)) {
        LogErr("Overlay::SetVisualRect: RectangleClip setter failed %s\n", HrString(hr).c_str());
        return false;
    }

    hr = d.visual->SetClip(d.clip.Get());
    if (FAILED(hr)) {
        LogErr("Overlay::SetVisualRect: SetClip failed %s\n", HrString(hr).c_str());
        return false;
    }

    // Exactly one Commit per geometry change (I11).
    hr = d.Commit();
    if (FAILED(hr)) {
        LogErr("Overlay::SetVisualRect: Commit failed %s\n", HrString(hr).c_str());
        return false;
    }

    // Record what was actually committed, in screen space, so the caller reports
    // the rect that is on screen instead of the one it asked for. Visual-local
    // (0,0) is screenRect's top-left, the offset having been applied above.
    d.committedVisual.left = screenRect.left + clipL;
    d.committedVisual.top = screenRect.top + clipT;
    d.committedVisual.right = screenRect.left + clipR;
    d.committedVisual.bottom = screenRect.top + clipB;
    return true;
}

// --------------------------------------------------------- RenderAndPresent()

bool Overlay::RenderAndPresent(uint32_t frameIndex, PresentSample* out) {
    Impl& d = *impl_;
    if (!d.swap || !d.ctx) {
        LogErr("Overlay::RenderAndPresent: not created\n");
        return false;
    }

    // A frame that never reaches Present1 is NOT a sample: `out` keeps valid=false
    // so the caller can drop it instead of averaging in a zeroed phantom. The qpc
    // is stamped because the skip itself happened at a real point in time.
    auto skip = [&]() -> bool {
        ++d.presentSkips;
        if (out) out->qpc = QpcNow();
        return true;  // non-fatal: the run continues, this frame is just void
    };

    // Frame-latency waitable object: with MaximumFrameLatency=1 this returns at
    // the compositor's cadence and keeps the wait OUT of the Present1 timing.
    if (d.waitable) {
        // Non-alertable: an APC must not be mistaken for "the frame is ready".
        const int64_t wt0 = QpcNow();
        const DWORD wr = WaitForSingleObjectEx(d.waitable, kWaitableTimeoutMs, FALSE);
        const double waitMs = QpcToMs(QpcNow() - wt0);
        ++d.waitCount;
        if (waitMs > d.maxWaitMs) d.maxWaitMs = waitMs;
        if (wr != WAIT_OBJECT_0) {
            ++d.waitNotSignaled;
            if (d.waitNotSignaled == 1)
                LogErr("Overlay: frame-latency wait returned %lu after %.3f ms (timeout=%lu ms); "
                       "logged once, the total is in the run report\n",
                       wr, waitMs, static_cast<unsigned long>(kWaitableTimeoutMs));
        }
    }

    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = d.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) {
        if (hr != d.loggedBufferHr) {
            d.loggedBufferHr = hr;
            LogErr("Overlay::RenderAndPresent: GetBuffer(0) failed %s (frame skipped)\n",
                   HrString(hr).c_str());
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
        return skip();
    }

    ID3D11RenderTargetView* rtv = nullptr;
    for (auto& e : d.rtvs) {
        if (e.key == back.Get()) {
            rtv = e.rtv.Get();
            break;
        }
    }
    if (!rtv) {
        Impl::RtvEntry entry;
        hr = d.device->CreateRenderTargetView(back.Get(), nullptr, &entry.rtv);
        if (FAILED(hr)) {
            if (hr != d.loggedRtvHr) {
                d.loggedRtvHr = hr;
                LogErr("Overlay::RenderAndPresent: CreateRenderTargetView failed %s "
                       "(frame skipped)\n",
                       HrString(hr).c_str());
            }
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
            return skip();
        }
        entry.key = back.Get();  // kept alive by entry.rtv
        d.rtvs.push_back(entry);
        rtv = d.rtvs.back().rtv.Get();
    }

    // Premultiplied alpha with A=1: the drawn area is fully opaque, which is the
    // product's real case and the one MPO promotion is decided on.
    const bool even = (frameIndex & 1u) == 0u;
    const FLOAT bg[4] = {even ? 1.0f : 0.0f, even ? 0.0f : 1.0f, 1.0f, 1.0f};  // magenta / cyan
    d.ctx->ClearRenderTargetView(rtv, bg);

    // Moving marker bar, drawn with ClearView so the probe needs no shaders.
    // BOTH conditions matter: without ID3D11DeviceContext1 there is no ClearView
    // to call, and when D3D11_OPTIONS.ClearView is FALSE (or the query itself
    // failed) the call is silently ignored - either way the marker bar, which is
    // the capture half's staleness detector, would vanish with no record.
    if (d.ctx1 && d.clearViewSupported) {
        const LONG w = static_cast<LONG>(d.scWidth);
        const LONG h = static_cast<LONG>(d.scHeight);
        const LONG span = (w > kBarWidthPx) ? (w - kBarWidthPx) : 1;
        const LONG x = static_cast<LONG>((static_cast<long long>(frameIndex) * kBarStepPx) % span);
        D3D11_RECT bar = {x, 0, (std::min)(x + kBarWidthPx, w), h};
        const FLOAT white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        d.ctx1->ClearView(rtv, white, &bar, 1);
    } else {
        // Fallback: a second FULL-SURFACE clear whose colour walks a four-step
        // cycle, so consecutive frames still differ (a two-step alternation
        // cannot be told apart from every second frame being dropped) and the
        // capture side can still detect a stale stream. Premultiplied alpha with
        // A=1, same as the clear above.
        const float step = static_cast<float>(frameIndex & 3u) / 3.0f;
        const FLOAT cycle[4] = {step, 1.0f - step, even ? 1.0f : 0.25f, 1.0f};
        d.ctx->ClearRenderTargetView(rtv, cycle);
        if (!d.loggedClearViewMissing) {
            d.loggedClearViewMissing = true;
            LogErr("Overlay: marker bar unavailable (%s); falling back to a full-surface "
                   "alternating clear - frames still change, the bar position does not. "
                   "Logged once.\n",
                   !d.ctx1 ? "no ID3D11DeviceContext1"
                           : "D3D11_OPTIONS.ClearView is FALSE or unqueryable");
        }
    }

    DXGI_PRESENT_PARAMETERS pp = {};  // zeroed: whole-frame present, no dirty rects
    const int64_t t0 = QpcNow();
    const HRESULT hrPresent = d.swap->Present1(0, 0, &pp);
    const int64_t t1 = QpcNow();

    if (out) {
        out->qpc = t1;
        out->presentCpuMs = QpcToMs(t1 - t0);
    }

    if (hrPresent == DXGI_ERROR_DEVICE_REMOVED || hrPresent == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = d.device ? d.device->GetDeviceRemovedReason() : hrPresent;
        LogErr("Overlay: Present1 %s, device removed reason %s\n", HrString(hrPresent).c_str(),
               HrString(reason).c_str());
        return false;
    }
    if (FAILED(hrPresent)) {
        if (hrPresent != d.loggedPresentHr) {
            d.loggedPresentHr = hrPresent;
            LogErr("Overlay: Present1 failed %s (logged once, run continues; the sample is "
                   "marked invalid)\n",
                   HrString(hrPresent).c_str());
        }
        // Nothing reached the screen, so there are no statistics for this frame.
        // Copying the previous frame's would invent a present that never
        // happened: leave the sample invalid and count it instead.
        ++d.presentFailures;
        return true;
    }
    // DXGI_STATUS_OCCLUDED is a success code and expected while hidden.
    ++d.presentsOk;

    // ---- statistics. DISJOINT / WAS_STILL_DRAWING mean "no data this frame".
    DXGI_FRAME_STATISTICS fs = {};
    const HRESULT hrStats = d.swap->GetFrameStatistics(&fs);
    if (SUCCEEDED(hrStats)) {
        d.lastStats = fs;
    } else if (hrStats != DXGI_ERROR_FRAME_STATISTICS_DISJOINT &&
               hrStats != DXGI_ERROR_WAS_STILL_DRAWING) {
        if (hrStats != d.loggedStatsHr) {
            d.loggedStatsHr = hrStats;
            LogErr("Overlay: GetFrameStatistics failed %s (logged once)\n", HrString(hrStats).c_str());
        }
    }

    if (d.media) {
        DXGI_FRAME_STATISTICS_MEDIA fsm = {};
        const HRESULT hrM = d.media->GetFrameStatisticsMedia(&fsm);
        if (SUCCEEDED(hrM)) {
            d.lastCompositionMode = static_cast<int>(fsm.CompositionMode);
            d.lastApprovedPresentDuration = fsm.ApprovedPresentDuration;
        } else if (hrM != DXGI_ERROR_FRAME_STATISTICS_DISJOINT &&
                   hrM != DXGI_ERROR_WAS_STILL_DRAWING) {
            if (hrM != d.loggedMediaHr) {
                d.loggedMediaHr = hrM;
                LogErr("Overlay: GetFrameStatisticsMedia failed %s (logged once)\n",
                       HrString(hrM).c_str());
            }
        }
    }

    if (out) {
        out->presentCount = d.lastStats.PresentCount;
        out->presentRefreshCount = d.lastStats.PresentRefreshCount;
        out->syncRefreshCount = d.lastStats.SyncRefreshCount;
        out->syncQpcTime = d.lastStats.SyncQPCTime.QuadPart;
        out->compositionMode = d.lastCompositionMode;
        out->approvedPresentDuration = d.lastApprovedPresentDuration;
        // Only here: Present1 ran and succeeded, so this sample is a real
        // observation and may enter the statistics.
        out->valid = true;
        // phaseIndex is stamped by the caller, which owns the phase schedule.
    }
    return true;
}

}  // namespace np
