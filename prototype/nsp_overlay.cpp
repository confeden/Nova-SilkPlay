// nsp_overlay.cpp — device + overlay HWND + DirectComposition + swapchain.
//
// The creation order and the window styles come from tools/overlay-probe's
// overlay_win.cpp, which is where they were measured (S-M2). Differences that
// matter here:
//   * ONE D3D11 device for capture, synthesis and presentation. The probe kept
//     the capture device separate so the measurement would not be perturbed;
//     a prototype wants the opposite — no cross-device seam at the generated
//     frame rate (directives §4 "device topology hazard").
//   * The swapchain is the size of the VIDEO RECT, not the client area, and the
//     visual is offset to it. Nothing is ever scaled (I13).
//   * DXGI_ALPHA_MODE_IGNORE: the overlay is opaque over the video rect (I10),
//     and captured frames whose alpha channel is 0 must not punch a hole in it.

#include "nsp_overlay.h"

#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nsp {
namespace {

constexpr wchar_t kClassName[] = L"NovaSilkPlayOverlay";
constexpr wchar_t kWindowText[] = L"Nova SilkPlay";

void AppendFmt(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n >= 0) {
        out.append(buf, static_cast<size_t>(n));
    } else {
        buf[sizeof(buf) - 1] = '\0';
        out.append(buf);
    }
}

// Hit tests that reached this window. With WS_EX_LAYERED | WS_EX_TRANSPARENT the
// system skips the window during hit-testing and this stays at zero; measured
// (tools/clickthrough-probe): 0 with LAYERED, 38 for one move/click/wheel burst
// without it. A non-zero count while the overlay is shown is therefore direct
// evidence that input is being swallowed, and main.cpp treats it as a tripwire.
// One overlay exists at a time and its messages arrive on the thread that created
// it, but the counter is read from the loop, so keep it atomic anyway.
std::atomic<uint32_t> g_hitTests{0};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            // Only while shown: that is when a hit test means the user's input was
            // taken away from the page.
            if (IsWindowVisible(hwnd)) g_hitTests.fetch_add(1, std::memory_order_relaxed);
            // Answering HTTRANSPARENT is NOT click-through across processes: it only
            // passes the hit test on to windows of the SAME thread, and Chrome's
            // window is not one, so the input is simply dropped. It stays as a
            // harmless last word; the style bits above are what actually work.
            return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        // NO PostQuitMessage HERE. This window is destroyed and rebuilt on every
        // re-engage — the target moving, resizing, or going fullscreen all route
        // through App::Disengage() -> overlay_.reset() -> DestroyWindow. Posting
        // WM_QUIT made that ordinary lifecycle event terminate the PROCESS: the
        // main loop treats WM_QUIT as "quit_ = true", so pressing F on YouTube
        // silently exited with code 0. The application decides when to quit
        // (Ctrl+Alt+X or the tray); a window closing does not.
        case WM_DESTROY: return 0;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

using PFN_DCompCreateDevice3 = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
using PFN_DCompCreateDevice2 = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);

}  // namespace

struct Overlay::Impl {
    HWND hwnd = nullptr;
    HWND target = nullptr;
    RECT hwndRect{};       // frozen at creation (I11)
    RECT videoRect{};      // screen space, what the visual covers
    HINSTANCE hinst = nullptr;
    bool ownsClass = false;
    bool shown = false;
    bool topmost = false;  // legacy placement; the default tracks the target instead
    bool zDegraded = false;  // tracking failed and the window fell back to topmost
    int zFailStreak = 0;
    uint64_t zFixes = 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel = static_cast<D3D_FEATURE_LEVEL>(0);

    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDXGISwapChain2> swap2;
    ComPtr<IDXGISwapChainMedia> media;
    HANDLE waitable = nullptr;
    UINT scW = 0, scH = 0;

    HMODULE dcompDll = nullptr;
    ComPtr<IDCompositionDesktopDevice> dev;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionRectangleClip> clip;

    // Back buffer of the frame currently being rendered, plus its sRGB RTV.
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> backRtv;

    int lastCompositionMode = -1;
    HRESULT loggedPresentHr = S_OK;
    uint64_t presents = 0;

    std::string report;

    bool CreateSwapchain(UINT w, UINT h, std::string* err);
    bool ApplyVisualRect();
    void Destroy();
};

bool Overlay::Impl::CreateSwapchain(UINT w, UINT h, std::string* err) {
    auto fail = [&](const char* what, HRESULT hr) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s: %s", what, HrString(hr).c_str());
        if (err) *err = buf;
        AppendFmt(report, "  *** %s ***\n", buf);
        return false;
    };

    back.Reset();
    backRtv.Reset();

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    scd.BufferCount = 3;
    scd.Scaling = DXGI_SCALING_STRETCH;  // required for CreateSwapChainForComposition
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> sc;
    HRESULT hr = factory->CreateSwapChainForComposition(device.Get(), &scd, nullptr, &sc);
    if (FAILED(hr)) return fail("CreateSwapChainForComposition failed", hr);

    swap = sc;
    swap2.Reset();
    media.Reset();
    if (waitable) {
        CloseHandle(waitable);
        waitable = nullptr;
    }
    if (SUCCEEDED(swap.As(&swap2))) {
        const HRESULT hrLat = swap2->SetMaximumFrameLatency(1);
        if (FAILED(hrLat))
            AppendFmt(report, "  SetMaximumFrameLatency(1) failed %s\n", HrString(hrLat).c_str());
        waitable = swap2->GetFrameLatencyWaitableObject();
    }
    if (FAILED(swap.As(&media)))
        AppendFmt(report, "  IDXGISwapChainMedia unavailable (composition mode stays -1)\n");

    scW = w;
    scH = h;
    AppendFmt(report, "  swapchain: %ux%u BGRA8 FLIP_DISCARD x3 alpha=IGNORE waitable=%s\n", w, h,
              waitable ? "yes" : "no");
    return true;
}

bool Overlay::Impl::ApplyVisualRect() {
    if (!visual || !dev) return false;

    const LONG offX = videoRect.left - hwndRect.left;
    const LONG offY = videoRect.top - hwndRect.top;
    const LONG w = RectW(videoRect);
    const LONG h = RectH(videoRect);
    if (w <= 0 || h <= 0) {
        LogErr("Overlay::ApplyVisualRect: empty rect %s", RectStr(videoRect).c_str());
        return false;
    }

    HRESULT hr = visual->SetOffsetX(static_cast<float>(offX));
    if (SUCCEEDED(hr)) hr = visual->SetOffsetY(static_cast<float>(offY));
    if (FAILED(hr)) {
        LogErr("Overlay::ApplyVisualRect: SetOffset failed %s", HrString(hr).c_str());
        return false;
    }

    if (!clip) {
        hr = dev->CreateRectangleClip(&clip);
        if (FAILED(hr) || !clip) {
            LogErr("Overlay::ApplyVisualRect: CreateRectangleClip failed %s", HrString(hr).c_str());
            return false;
        }
    }
    // Visual-local space: the swapchain content lives over [0,w) x [0,h).
    clip->SetLeft(0.0f);
    clip->SetTop(0.0f);
    clip->SetRight(static_cast<float>(w));
    clip->SetBottom(static_cast<float>(h));
    hr = visual->SetClip(clip.Get());
    if (FAILED(hr)) {
        LogErr("Overlay::ApplyVisualRect: SetClip failed %s", HrString(hr).c_str());
        return false;
    }

    hr = dev->Commit();
    if (FAILED(hr)) {
        LogErr("Overlay::ApplyVisualRect: Commit failed %s", HrString(hr).c_str());
        return false;
    }
    return true;
}

void Overlay::Impl::Destroy() {
    if (visual) {
        visual->SetContent(nullptr);
        visual->SetClip(static_cast<IDCompositionClip*>(nullptr));
    }
    if (dcompTarget) dcompTarget->SetRoot(nullptr);
    if (dev) dev->Commit();
    clip.Reset();
    visual.Reset();
    dcompTarget.Reset();
    dev.Reset();

    backRtv.Reset();
    back.Reset();
    media.Reset();
    swap2.Reset();
    if (waitable) {
        CloseHandle(waitable);
        waitable = nullptr;
    }
    swap.Reset();
    factory.Reset();

    if (ctx) {
        ctx->ClearState();
        ctx->Flush();
    }
    ctx.Reset();
    device.Reset();

    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (ownsClass) {
        UnregisterClassW(kClassName, hinst);
        ownsClass = false;
    }
    dcompDll = nullptr;  // deliberately left loaded
}

// ============================================================================

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}
Overlay::~Overlay() {
    if (impl_) impl_->Destroy();
}

ID3D11Device* Overlay::Device() const { return impl_->device.Get(); }
ID3D11DeviceContext* Overlay::Context() const { return impl_->ctx.Get(); }
HWND Overlay::Hwnd() const { return impl_->hwnd; }
const RECT& Overlay::VideoRect() const { return impl_->videoRect; }
bool Overlay::IsShown() const { return impl_->shown; }
int Overlay::LastCompositionMode() const { return impl_->lastCompositionMode; }
const std::string& Overlay::CreationReport() const { return impl_->report; }
uint32_t Overlay::HitTestsSeen() const { return g_hitTests.load(std::memory_order_relaxed); }
uint64_t Overlay::ZOrderFixes() const { return impl_->zFixes; }
bool Overlay::ZOrderDegraded() const { return impl_->zDegraded; }

void Overlay::Show() {
    if (!impl_->hwnd || impl_->shown) return;
    KeepAbove();
    ShowWindow(impl_->hwnd, SW_SHOWNA);  // never SW_SHOW: no activation, no focus steal
    KeepAbove();
    impl_->shown = true;
}

// THE OVERLAY LIVES DIRECTLY ABOVE THE TARGET IN Z-ORDER, NOT IN THE TOPMOST BAND.
//
// A topmost overlay paints over every ordinary window the user puts in front of
// the browser — a messenger, Explorer, a menu, a tooltip — for as long as it takes
// the occlusion test to notice and hide, and on the frame that window appears
// that is a visible flash of video on top of it. One slot above the target makes
// DWM do the right thing for free: whatever is above Chrome is above us too, so a
// covering window is drawn correctly on the very first frame and the occlusion
// test only decides whether generation is worth continuing, never whether the
// screen is correct.
//
// The price is that activating Chrome raises it above us (measured by the probe:
// a click on the page leaves the overlay below Chrome). The loop calls this every
// iteration, so the browser's own frame shows for at most an iteration or two at
// the moment the user clicks — the same picture a frame earlier, not a defect of
// the kind D22 names.
//
// FOREGROUND PROTECTION decides HOW. Measured from a background process with
// Chrome in the foreground (2026-09-13), for every style combination tried:
//   * SetWindowPos(HWND_TOP) and, for a window not already above Chrome,
//     SetWindowPos(HWND_NOTOPMOST) RETURN SUCCESS AND DO NOTHING — the window
//     stays under the foreground browser;
//   * inserting after a specific window (the one directly above Chrome) works;
//   * HWND_TOPMOST works, and demoting a topmost window with HWND_NOTOPMOST
//     lands it at the top of the normal band — above Chrome — because that is a
//     move DOWN.
// So the slot is always reached by one of those two, and the result is checked
// rather than trusted: a SetWindowPos that "succeeds" proves nothing here.
bool Overlay::KeepAbove() {
    Impl& d = *impl_;
    if (!d.hwnd || d.topmost || !d.target || !IsWindow(d.target)) return false;
    HWND prev = GetWindow(d.target, GW_HWNDPREV);
    if (prev == d.hwnd) {
        d.zFailStreak = 0;
        return false;
    }

    const auto isTop = [](HWND h) { return (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; };
    const bool targetTop = isTop(d.target);
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;
    if (prev && isTop(prev) == targetTop) {
        // The usual case for Chrome: its own invisible IME window sits directly
        // above it. Cross bands first (a demotion or a promotion, both allowed),
        // then insert directly under prev, i.e. directly above the target.
        if (isTop(d.hwnd) != targetTop)
            SetWindowPos(d.hwnd, targetTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(d.hwnd, prev, 0, 0, 0, 0, flags);
    } else if (targetTop) {
        // The target heads the topmost band: the top of that band is the slot.
        SetWindowPos(d.hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    } else {
        // The target heads the normal band. HWND_TOP would be refused silently;
        // promote, then demote to the top of the normal band.
        SetWindowPos(d.hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(d.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    }
    ++d.zFixes;

    if (GetWindow(d.target, GW_HWNDPREV) == d.hwnd) {
        d.zFailStreak = 0;
    } else if (++d.zFailStreak >= 60) {
        // Something keeps us out of the slot — another program claiming the same
        // one, or a rule the probe did not meet. An overlay stuck UNDER the
        // browser generates frames nobody sees, forever; a topmost one at least
        // works, and the occlusion test still pauses it for covering windows.
        SetWindowPos(d.hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
        d.topmost = true;
        d.zDegraded = true;
        LogErr("Overlay: could not stay directly above the target after %d attempts - falling "
               "back to a TOPMOST overlay for this engagement",
               d.zFailStreak);
    }
    return true;
}

void Overlay::Hide() {
    if (!impl_->hwnd || !impl_->shown) return;
    ShowWindow(impl_->hwnd, SW_HIDE);
    impl_->shown = false;
}

bool Overlay::Create(HWND target, const RECT& hwndRectScreen, const RECT& videoRectScreen,
                     std::string* err, bool topmost, bool layered) {
    Impl& d = *impl_;
    d.target = target;
    d.videoRect = videoRectScreen;
    d.topmost = topmost;
    g_hitTests.store(0, std::memory_order_relaxed);

    // THE WINDOW IS THE VIDEO RECT, NOT THE CLIENT AREA — and it is deliberately
    // never allowed to cover the target completely.
    //
    // MEASURED, because this one cost the owner an afternoon of "the browser
    // stopped responding": Chromium's native occlusion tracker marks a window
    // OCCLUDED when another window fully covers it, and then throttles that
    // tab's renderer. With this overlay spanning the whole client area, Chrome's
    // total CPU fell from 95.7 % to 20.6 % the moment the overlay appeared and
    // recovered when it hid. Clicks were being delivered the whole time — the
    // page simply had almost no budget to respond to them, which is
    // indistinguishable from input being swallowed.
    //
    // Occlusion needs FULL coverage: with the overlay one pixel shorter than the
    // client area, the same measurement gave 105.9 % instead of 15.9 %. So the
    // window is sized to the video rect, which in a windowed player is strictly
    // inside the client area and leaves Chrome visibly un-occluded. When the
    // video rect DOES span the whole client area (fullscreen), one pixel is
    // given back at the bottom edge — the row where the player's own control bar
    // lives and where I10 already requires us to hide.
    RECT windowRect = videoRectScreen;
    const bool coversAll = windowRect.left <= hwndRectScreen.left &&
                           windowRect.top <= hwndRectScreen.top &&
                           windowRect.right >= hwndRectScreen.right &&
                           windowRect.bottom >= hwndRectScreen.bottom;
    if (coversAll && RectH(windowRect) > 1) {
        windowRect.bottom -= 1;
    }
    d.hwndRect = windowRect;

    std::string& rep = d.report;
    rep.clear();

    auto fail = [&](const char* what, HRESULT hr) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s: %s", what, HrString(hr).c_str());
        if (err) *err = buf;
        AppendFmt(rep, "  *** CREATE FAILED: %s ***\n", buf);
        return false;
    };
    auto failMsg = [&](const char* what) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (GetLastError=%lu)", what, GetLastError());
        if (err) *err = buf;
        AppendFmt(rep, "  *** CREATE FAILED: %s ***\n", buf);
        return false;
    };

    if (RectEmpty(hwndRectScreen) || RectEmpty(videoRectScreen)) {
        if (err) *err = "Create: empty window or video rect";
        return false;
    }

    // ---- window class + window
    d.hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = d.hinst;
    wc.hCursor = nullptr;        // I12: we never draw or claim a cursor
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return failMsg("RegisterClassExW failed");
    } else {
        d.ownsClass = true;
    }

    // I9 + CLICK-THROUGH. WS_EX_TRANSPARENT alone does NOT make a top-level window
    // transparent to input — only WS_EX_LAYERED | WS_EX_TRANSPARENT does.
    // MEASURED with real SendInput over a shown overlay on the test page
    // (tools/clickthrough-probe, 2026-09-13): without LAYERED the page received
    // 0 of 1 click, 0 of 4 moves and 0 of 1 wheel notch while this window took 38
    // hit tests; with LAYERED it received all of them, the window took none, and
    // Desktop Duplication still read our magenta off the screen. An earlier note
    // here claimed LAYERED "made no difference"; that run was made while the
    // overlay fully covered Chrome, i.e. while G33 had the page throttled, and is
    // superseded. WindowFromPoint answered "chrome" in BOTH configurations, so it
    // cannot tell them apart — the hit-test counter above is the real test.
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                    WS_EX_TRANSPARENT | WS_EX_LAYERED;  // I9
    if (topmost) exStyle |= WS_EX_TOPMOST;
    if (!layered) exStyle &= ~static_cast<DWORD>(WS_EX_LAYERED);  // measurement only
    d.hwnd = CreateWindowExW(exStyle, kClassName, kWindowText, WS_POPUP, windowRect.left,
                             windowRect.top, static_cast<int>(RectW(windowRect)),
                             static_cast<int>(RectH(windowRect)), nullptr, nullptr, d.hinst,
                             nullptr);
    if (!d.hwnd) return failMsg("CreateWindowExW failed");
    // Fully opaque layering. Without it the probe still showed the picture and
    // still passed input, but a layered window that never declared its
    // attributes is one whose presentation is left to whatever the shell decides.
    if (layered && !SetLayeredWindowAttributes(d.hwnd, 0, 255, LWA_ALPHA))
        AppendFmt(rep, "  SetLayeredWindowAttributes failed, GetLastError=%lu\n", GetLastError());
    AppendFmt(rep, "  hwnd 0x%p over %s, video rect %s, %s\n", static_cast<void*>(d.hwnd),
              RectStr(hwndRectScreen).c_str(), RectStr(videoRectScreen).c_str(),
              topmost ? "TOPMOST (legacy)" : "z-order: directly above the target");
    KeepAbove();

    // ---- device
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required by DComp and the WGC interop
#ifdef NSP_D3D_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want,
                                   ARRAYSIZE(want), D3D11_SDK_VERSION, &d.device, &d.featureLevel,
                                   &d.ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice failed", hr);

    // Multithread protection: the WGC frame pool is free-threaded and its
    // delegates can touch the device from a pool thread.
    {
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(d.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d.device.As(&dxgiDevice);
    if (FAILED(hr)) return fail("QI IDXGIDevice failed", hr);
    // One frame of queued work is plenty and keeps the present latency honest.
    {
        ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1))) dxgiDevice1->SetMaximumFrameLatency(1);
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return fail("GetAdapter failed", hr);
    hr = adapter->GetParent(IID_PPV_ARGS(&d.factory));
    if (FAILED(hr)) return fail("GetParent(IDXGIFactory2) failed", hr);

    {
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(adapter->GetDesc(&ad)))
            AppendFmt(rep, "  adapter: %s (vendor 0x%04X device 0x%04X)\n",
                      Narrow(ad.Description).c_str(), ad.VendorId, ad.DeviceId);
    }

    // ---- swapchain sized to the video rect (I13: no scaling anywhere)
    if (!d.CreateSwapchain(static_cast<UINT>(RectW(videoRectScreen)),
                           static_cast<UINT>(RectH(videoRectScreen)), err))
        return false;

    // ---- DirectComposition
    d.dcompDll = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d.dcompDll) return failMsg("LoadLibrary(dcomp.dll) failed");
    auto create3 = reinterpret_cast<PFN_DCompCreateDevice3>(
        GetProcAddress(d.dcompDll, "DCompositionCreateDevice3"));
    auto create2 = reinterpret_cast<PFN_DCompCreateDevice2>(
        GetProcAddress(d.dcompDll, "DCompositionCreateDevice2"));

    HRESULT hrD = E_NOINTERFACE;
    if (create3) hrD = create3(dxgiDevice.Get(), IID_PPV_ARGS(&d.dev));
    if (!d.dev && create2) hrD = create2(dxgiDevice.Get(), IID_PPV_ARGS(&d.dev));
    if (!d.dev) return fail("no IDCompositionDesktopDevice", hrD);

    hr = d.dev->CreateTargetForHwnd(d.hwnd, TRUE, &d.dcompTarget);
    if (FAILED(hr)) return fail("CreateTargetForHwnd failed", hr);

    ComPtr<IDCompositionVisual2> v2;
    hr = d.dev->CreateVisual(&v2);
    if (FAILED(hr) || !v2) return fail("CreateVisual failed", hr);
    d.visual = v2;

    // Nearest-neighbour: there is nothing to filter, the visual is 1:1, and a
    // linear-filtered visual would resample the picture by a subpixel offset.
    v2->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

    hr = d.visual->SetContent(d.swap.Get());
    if (FAILED(hr)) return fail("SetContent failed", hr);
    hr = d.dcompTarget->SetRoot(d.visual.Get());
    if (FAILED(hr)) return fail("SetRoot failed", hr);
    if (!d.ApplyVisualRect()) {
        if (err) *err = "initial ApplyVisualRect failed";
        return false;
    }

    AppendFmt(rep, "  DComp: visual at offset %ld,%ld, clip %ldx%ld, hidden until engage\n",
              videoRectScreen.left - hwndRectScreen.left, videoRectScreen.top - hwndRectScreen.top,
              RectW(videoRectScreen), RectH(videoRectScreen));
    return true;
}

bool Overlay::SetVideoRect(const RECT& videoRectScreen) {
    Impl& d = *impl_;
    if (RectEmpty(videoRectScreen)) return false;

    const UINT w = static_cast<UINT>(RectW(videoRectScreen));
    const UINT h = static_cast<UINT>(RectH(videoRectScreen));
    if (w != d.scW || h != d.scH) {
        d.back.Reset();
        d.backRtv.Reset();
        const HRESULT hr = d.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                                                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(hr)) {
            LogErr("Overlay::SetVideoRect: ResizeBuffers(%ux%u) failed %s", w, h,
                   HrString(hr).c_str());
            return false;
        }
        d.scW = w;
        d.scH = h;
    }
    d.videoRect = videoRectScreen;
    return d.ApplyVisualRect();
}

bool Overlay::WaitPresentable(DWORD timeoutMs) {
    Impl& d = *impl_;
    if (!d.waitable) return true;
    // Non-alertable: an APC must not be mistaken for "the frame is ready".
    return WaitForSingleObjectEx(d.waitable, timeoutMs, FALSE) == WAIT_OBJECT_0;
}

bool Overlay::AcquireBackBuffer(ID3D11Texture2D** tex, ID3D11RenderTargetView** rtvSrgb) {
    Impl& d = *impl_;
    if (!d.swap) return false;

    // Bounded: two refreshes of the fastest output. A present that is due must
    // not be held up for longer than that by a swapchain that has not retired.
    WaitPresentable(12);

    if (!d.back) {
        HRESULT hr = d.swap->GetBuffer(0, IID_PPV_ARGS(&d.back));
        if (FAILED(hr)) {
            LogErr("Overlay::AcquireBackBuffer: GetBuffer failed %s", HrString(hr).c_str());
            return false;
        }
        // An _SRGB view over the _UNORM buffer: shaders read and write linear
        // light, the hardware does the transfer function. Warp and blend must
        // happen in linear light (directives §4, "format chain").
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = d.device->CreateRenderTargetView(d.back.Get(), &rd, &d.backRtv);
        if (FAILED(hr)) {
            // Fall back to the plain view rather than lose the frame: the blend
            // then happens in the encoded domain, which is wrong but visible and
            // debuggable, unlike a black overlay.
            static bool logged = false;
            if (!logged) {
                logged = true;
                LogErr("Overlay: sRGB RTV over the back buffer rejected (%s) - falling back to a "
                       "UNORM view; blending is NOT in linear light",
                       HrString(hr).c_str());
            }
            hr = d.device->CreateRenderTargetView(d.back.Get(), nullptr, &d.backRtv);
        }
        if (FAILED(hr)) {
            LogErr("Overlay::AcquireBackBuffer: CreateRenderTargetView failed %s",
                   HrString(hr).c_str());
            d.back.Reset();
            return false;
        }
    }
    if (tex) *tex = d.back.Get();
    if (rtvSrgb) *rtvSrgb = d.backRtv.Get();
    return true;
}

bool Overlay::Present() {
    Impl& d = *impl_;
    if (!d.swap) return false;

    DXGI_PRESENT_PARAMETERS pp = {};
    const HRESULT hr = d.swap->Present1(0, 0, &pp);

    // FLIP_DISCARD: buffer 0 is a different surface after every present.
    d.back.Reset();
    d.backRtv.Reset();

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LogErr("Overlay::Present: %s, device removed reason %s", HrString(hr).c_str(),
               HrString(d.device ? d.device->GetDeviceRemovedReason() : hr).c_str());
        return false;
    }
    if (FAILED(hr)) {
        if (hr != d.loggedPresentHr) {
            d.loggedPresentHr = hr;
            LogErr("Overlay::Present: Present1 failed %s (logged once)", HrString(hr).c_str());
        }
        return true;
    }
    ++d.presents;

    if (d.media) {
        DXGI_FRAME_STATISTICS_MEDIA fsm = {};
        if (SUCCEEDED(d.media->GetFrameStatisticsMedia(&fsm)))
            d.lastCompositionMode = static_cast<int>(fsm.CompositionMode);
    }
    return true;
}

}  // namespace nsp
