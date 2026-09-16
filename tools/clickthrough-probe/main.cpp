// clickthrough-probe — does real mouse input pass through our overlay window?
//
// The prototype's comments disagree with each other on this, and both claim a
// measurement: nsp_overlay.cpp says WS_EX_LAYERED "made no difference" and a
// synthetic click never reached Chrome; gotchas G33 says every click was
// delivered and hit-testing is a dead end. The engine currently hides itself
// whenever the pointer is on the video because of the first claim, which in
// fullscreen means "almost always". This probe settles it with real input.
//
// For each window-style configuration it creates an overlay exactly the way the
// prototype does (DComp visual + composition swapchain, WS_EX_NOREDIRECTIONBITMAP)
// over the middle of the test page, fills it with magenta, then:
//   * reads the composed desktop through DXGI Desktop Duplication to prove the
//     overlay is actually ON SCREEN (a click-through window nobody can see
//     proves nothing),
//   * reads the swapchain's composition mode (OVERLAY plane or COMPOSED),
//   * sends real input with SendInput — moves, a left click, a wheel notch —
//     and reads what the PAGE saw from its title (testmotion.html?input=1),
//   * counts which messages reached the overlay's own WndProc,
//   * for the non-topmost variant, checks the overlay is still above Chrome
//     after the click activated Chrome, and how late the foreground WinEvent is.
//
// It moves the real cursor and clicks the test page. Cursor position and the
// foreground window are restored at the end.
//
//   clickthrough_probe.exe [--title SUBSTR] [--only NAME]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// ----------------------------------------------------------------- utilities

int64_t Qpc() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}
double QpcMs(int64_t d) {
    static const int64_t f = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return li.QuadPart;
    }();
    return static_cast<double>(d) * 1000.0 / static_cast<double>(f);
}

void Pump(DWORD ms) {
    const int64_t t0 = Qpc();
    MSG msg;
    while (QpcMs(Qpc() - t0) < ms) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
}

std::string Narrow(const wchar_t* w) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

// ------------------------------------------------------------- the test page

struct PageCounts {
    int d = -1, u = -1, m = -1, w = -1;
    bool ok() const { return d >= 0; }
};

PageCounts ReadPage(HWND target) {
    PageCounts c;
    wchar_t title[512] = {};
    GetWindowTextW(target, title, ARRAYSIZE(title));
    const wchar_t* bar = wcsstr(title, L" | d");
    if (bar) swscanf_s(bar, L" | d%d u%d m%d w%d", &c.d, &c.u, &c.m, &c.w);
    return c;
}

struct FindCtx {
    std::wstring title;
    HWND found = nullptr;
};

HWND FindTargetWindow(const std::wstring& titleSub) {
    FindCtx ctx{titleSub};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<FindCtx*>(lp);
            if (!IsWindowVisible(h)) return TRUE;
            wchar_t cls[128] = {}, t[512] = {};
            GetClassNameW(h, cls, ARRAYSIZE(cls));
            GetWindowTextW(h, t, ARRAYSIZE(t));
            if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;
            if (!wcsstr(t, c->title.c_str())) return TRUE;
            c->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// ------------------------------------------------------ what the overlay saw

int g_nchittest = 0;
int g_mouseMsgs = 0;
int g_setCursor = 0;
bool g_answerHt = true;

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) {
        ++g_nchittest;
        if (g_answerHt) return HTTRANSPARENT;
    }
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_NCMOUSEMOVE ||
        msg == WM_NCLBUTTONDOWN || (msg >= 0x0245 && msg <= 0x0257) /* WM_POINTER* */)
        ++g_mouseMsgs;
    if (msg == WM_SETCURSOR) ++g_setCursor;
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------- foreground WinEvent

int64_t g_fgEventQpc = 0;
HWND g_fgEventHwnd = nullptr;
int g_reorderEvents = 0;

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                           DWORD) {
    if (ev == EVENT_SYSTEM_FOREGROUND && g_fgEventQpc == 0) {
        g_fgEventQpc = Qpc();
        g_fgEventHwnd = hwnd;
    }
    if (ev == EVENT_OBJECT_REORDER && idObject == OBJID_WINDOW && idChild == CHILDID_SELF)
        ++g_reorderEvents;
}

bool IsAbove(HWND ov, HWND target) {
    for (HWND w = GetWindow(target, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (w == ov) return true;
    return false;
}

void PlaceDirectlyAbove(HWND ov, HWND target) {
    HWND prev = GetWindow(target, GW_HWNDPREV);
    HWND after = HWND_TOP;
    if (prev && prev != ov && !(GetWindowLongW(prev, GWL_EXSTYLE) & WS_EX_TOPMOST)) after = prev;
    if (prev == ov) return;
    SetWindowPos(ov, after, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// ----------------------------------------------------------- the GPU side

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIOutputDuplication> dup;
    RECT outputRect{};
    ComPtr<ID3D11Texture2D> staging;
    bool haveFrame = false;
};

bool InitGpu(Gpu& g, POINT pt) {
    ComPtr<IDXGIFactory1> f1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return false;
    f1.As(&g.factory);
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    for (UINT a = 0; !output; ++a) {
        ComPtr<IDXGIAdapter1> ad;
        if (f1->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (PtInRect(&od.DesktopCoordinates, pt)) {
                adapter = ad;
                output = out;
                g.outputRect = od.DesktopCoordinates;
                break;
            }
        }
    }
    if (!output) {
        printf("no DXGI output contains the click point\n");
        return false;
    }
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                   &g.dev, nullptr, &g.ctx);
    if (FAILED(hr)) {
        printf("D3D11CreateDevice failed 0x%08lX\n", hr);
        return false;
    }
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(output.As(&out5))) {
        DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = out5->DuplicateOutput1(g.dev.Get(), 0, 1, fmts, &g.dup);
    }
    if (!g.dup) {
        ComPtr<IDXGIOutput1> out1;
        if (SUCCEEDED(output.As(&out1))) hr = out1->DuplicateOutput(g.dev.Get(), &g.dup);
    }
    if (!g.dup) printf("desktop duplication unavailable (0x%08lX) - visibility column will be '?'\n", hr);
    return true;
}

// Drains duplication frames for `ms`, keeping a copy of the newest desktop image.
void RefreshDesktop(Gpu& g, DWORD ms) {
    if (!g.dup) return;
    const int64_t t0 = Qpc();
    while (QpcMs(Qpc() - t0) < ms) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        const HRESULT hr = g.dup->AcquireNextFrame(20, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) return;
        ComPtr<ID3D11Texture2D> tex;
        if (fi.LastPresentTime.QuadPart != 0 && SUCCEEDED(res.As(&tex))) {
            if (!g.staging) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                g.dev->CreateTexture2D(&td, nullptr, &g.staging);
            }
            if (g.staging) {
                g.ctx->CopyResource(g.staging.Get(), tex.Get());
                g.haveFrame = true;
            }
        }
        g.dup->ReleaseFrame();
    }
}

// Returns 1 = magenta on screen, 0 = something else, -1 = unknown.
int ScreenIsMagenta(Gpu& g, POINT pt) {
    if (!g.haveFrame) return -1;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(g.staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return -1;
    const LONG x = pt.x - g.outputRect.left, y = pt.y - g.outputRect.top;
    const uint8_t* p = static_cast<uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
    const int b = p[0], gr = p[1], r = p[2];
    g.ctx->Unmap(g.staging.Get(), 0);
    return (r > 230 && gr < 25 && b > 230) ? 1 : 0;
}

// ------------------------------------------------------------- one config

struct Config {
    const char* name;
    bool overlay;
    bool layered;
    bool setAttrs;
    bool topmost;
    bool answerHt;
};

struct Result {
    int visible = -1;
    int compMode = -1;
    PageCounts before, after;
    int nchit = 0, mouse = 0, setcur = 0;
    std::string wfp;
    bool aboveAfterClick = false;
    double fgEventMs = -1.0;
    int reorder = 0;
};

const char* CompName(int m) {
    switch (m) {
        case 0: return "COMPOSED";
        case 1: return "OVERLAY";
        case 2: return "NONE";
        case 3: return "FAILURE";
        default: return "?";
    }
}

void SendMoves(POINT pt) {
    SetCursorPos(pt.x, pt.y);
    Pump(40);
    for (int i = 0; i < 4; ++i) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        in.mi.dx = (i % 2) ? -4 : 4;
        SendInput(1, &in, sizeof(in));
        Pump(25);
    }
}

void SendClickAndWheel() {
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
    Pump(40);
    INPUT w{};
    w.type = INPUT_MOUSE;
    w.mi.dwFlags = MOUSEEVENTF_WHEEL;
    w.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    SendInput(1, &w, sizeof(w));
}

Result RunConfig(Gpu& g, const Config& c, HWND target, const RECT& ovRect, POINT pt) {
    Result r;
    g_nchittest = g_mouseMsgs = g_setCursor = 0;
    g_answerHt = c.answerHt;
    g_fgEventQpc = 0;
    g_fgEventHwnd = nullptr;
    g_reorderEvents = 0;

    // Move the foreground AWAY from Chrome first, so the click below has an
    // activation to cause and the non-topmost variant is actually tested.
    HWND shell = GetShellWindow();
    if (shell) SetForegroundWindow(shell);
    Pump(80);

    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual> visual;

    if (c.overlay) {
        DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
        if (c.topmost) ex |= WS_EX_TOPMOST;
        if (c.layered) ex |= WS_EX_LAYERED;
        hwnd = CreateWindowExW(ex, L"NspClickProbe", L"clickthrough probe", WS_POPUP, ovRect.left,
                               ovRect.top, ovRect.right - ovRect.left, ovRect.bottom - ovRect.top,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) {
            printf("[%s] CreateWindowEx failed %lu\n", c.name, GetLastError());
            return r;
        }
        if (c.layered && c.setAttrs) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = static_cast<UINT>(ovRect.right - ovRect.left);
        scd.Height = static_cast<UINT>(ovRect.bottom - ovRect.top);
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr = g.factory->CreateSwapChainForComposition(g.dev.Get(), &scd, nullptr, &swap);
        ComPtr<IDXGIDevice> dxgiDev;
        g.dev.As(&dxgiDev);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp));
        if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget);
        if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&visual);
        if (SUCCEEDED(hr)) hr = visual->SetContent(swap.Get());
        if (SUCCEEDED(hr)) hr = dtarget->SetRoot(visual.Get());
        if (SUCCEEDED(hr)) hr = dcomp->Commit();
        if (FAILED(hr)) {
            printf("[%s] swapchain/DComp setup failed 0x%08lX\n", c.name, hr);
            DestroyWindow(hwnd);
            return r;
        }
        ShowWindow(hwnd, SW_SHOWNA);
        if (!c.topmost) PlaceDirectlyAbove(hwnd, target);

        // ~0.6 s of magenta presents, like a running engine.
        ComPtr<IDXGISwapChainMedia> media;
        swap.As(&media);
        const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        for (int i = 0; i < 90; ++i) {
            ComPtr<ID3D11Texture2D> back;
            swap->GetBuffer(0, IID_PPV_ARGS(&back));
            ComPtr<ID3D11RenderTargetView> rtv;
            g.dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
            g.ctx->ClearRenderTargetView(rtv.Get(), magenta);
            swap->Present(1, 0);
            Pump(6);
            if (i > 60 && media) {
                DXGI_FRAME_STATISTICS_MEDIA fsm{};
                if (SUCCEEDED(media->GetFrameStatisticsMedia(&fsm)))
                    r.compMode = static_cast<int>(fsm.CompositionMode);
            }
        }
    }

    RefreshDesktop(g, 250);
    r.visible = ScreenIsMagenta(g, pt);

    {
        HWND h = WindowFromPoint(pt);
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s",
                    h == hwnd && hwnd ? "OVERLAY" : (GetAncestor(h, GA_ROOT) == target ? "chrome" : "other"));
        r.wfp = buf;
    }

    r.before = ReadPage(target);
    SendMoves(pt);
    const int64_t clickQpc = Qpc();
    SendClickAndWheel();
    Pump(350);
    r.after = ReadPage(target);
    r.nchit = g_nchittest;
    r.mouse = g_mouseMsgs;
    r.setcur = g_setCursor;
    r.reorder = g_reorderEvents;
    if (g_fgEventQpc) r.fgEventMs = QpcMs(g_fgEventQpc - clickQpc);
    if (hwnd) r.aboveAfterClick = IsAbove(hwnd, target);

    if (hwnd) {
        visual->SetContent(nullptr);
        dtarget->SetRoot(nullptr);
        dcomp->Commit();
        DestroyWindow(hwnd);
        Pump(50);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::wstring title = L"Nova SilkPlay motion source";
    std::string only;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc) {
            wchar_t w[256];
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, w, 256);
            title = w;
        } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            only = argv[++i];
        }
    }

    HWND target = FindTargetWindow(title);
    if (!target) {
        printf("no Chrome window titled '*%s*' - launch prototype/run_testpage.py with ?input=1\n",
               Narrow(title.c_str()).c_str());
        return 2;
    }
    RECT cr{};
    GetClientRect(target, &cr);
    POINT tl{cr.left, cr.top}, br{cr.right, cr.bottom};
    ClientToScreen(target, &tl);
    ClientToScreen(target, &br);
    // Inset: never fully cover the window, so G33 (the occlusion throttle) cannot
    // confound the measurement the way it may have confounded the last one.
    const RECT ovRect{tl.x + 60, tl.y + 60, br.x - 60, br.y - 60};
    const POINT pt{(ovRect.left + ovRect.right) / 2, (ovRect.top + ovRect.bottom) / 2};
    if (!ReadPage(target).ok()) {
        printf("target title has no input counters - open testmotion.html?input=1\n");
        return 2;
    }
    printf("target 0x%p client %ld,%ld-%ld,%ld  overlay %ld,%ld-%ld,%ld  click at %ld,%ld  "
           "target topmost=%d\n",
           static_cast<void*>(target), tl.x, tl.y, br.x, br.y, ovRect.left, ovRect.top,
           ovRect.right, ovRect.bottom, pt.x, pt.y,
           (GetWindowLongW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NspClickProbe";
    RegisterClassExW(&wc);

    Gpu g;
    if (!InitGpu(g, pt)) return 3;

    HWINEVENTHOOK hk = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                       WinEventProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hk2 = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr,
                                        WinEventProc, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    HWND savedFg = GetForegroundWindow();

    const Config configs[] = {
        // name          overlay layered attrs topmost answerHt
        {"no-overlay", false, false, false, false, false},
        {"current", true, false, false, true, true},
        {"current-noHT", true, false, false, true, false},
        {"layered", true, true, true, true, true},
        {"layered-noHT", true, true, true, true, false},
        {"layered-noattr", true, true, false, true, true},
        {"layered-ztrack", true, true, true, false, true},
    };

    printf("\n%-15s %-7s %-9s %-8s %-18s %-6s %-6s %-6s %-9s %-7s %s\n", "config", "onscr",
           "plane", "WFP", "page d/u/m/w", "NCHIT", "mouse", "setcur", "above", "fg ms", "reorder");
    for (const auto& c : configs) {
        if (!only.empty() && only != c.name) continue;
        const Result r = RunConfig(g, c, target, ovRect, pt);
        char page[64];
        _snprintf_s(page, sizeof(page), _TRUNCATE, "%+d/%+d/%+d/%+d", r.after.d - r.before.d,
                    r.after.u - r.before.u, r.after.m - r.before.m, r.after.w - r.before.w);
        printf("%-15s %-7s %-9s %-8s %-18s %-6d %-6d %-6d %-9s %-7.1f %d\n", c.name,
               r.visible < 0 ? "?" : (r.visible ? "MAGENTA" : "no"),
               c.overlay ? CompName(r.compMode) : "-", r.wfp.c_str(), page, r.nchit, r.mouse,
               r.setcur, c.overlay ? (r.aboveAfterClick ? "yes" : "NO") : "-", r.fgEventMs,
               r.reorder);
        fflush(stdout);
    }

    SetCursorPos(savedCursor.x, savedCursor.y);
    if (savedFg) SetForegroundWindow(savedFg);
    if (hk) UnhookWinEvent(hk);
    if (hk2) UnhookWinEvent(hk2);
    return 0;
}
