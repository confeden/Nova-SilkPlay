// nsp_common.cpp — logging, time, window enumeration.

#include "nsp_common.h"

#include <limits>

#include <dwmapi.h>
#include <psapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace nsp {
namespace {

int64_t QpcFreq() {
    static const int64_t f = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return li.QuadPart ? li.QuadPart : 1;
    }();
    return f;
}

int64_t StartQpc() {
    static const int64_t t0 = QpcNow();
    return t0;
}

void VLogTo(FILE* f, const char* fmt, va_list ap) {
    const double ms = QpcToMs(QpcNow() - StartQpc());
    char buf[2048];
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    if (n < 0) buf[sizeof(buf) - 1] = '\0';
    fprintf(f, "[%10.3f] %s", ms, buf);
    const size_t len = strlen(buf);
    if (len == 0 || buf[len - 1] != '\n') fputc('\n', f);
    fflush(f);
}

std::wstring ProcessImageName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH] = {};
    DWORD n = ARRAYSIZE(path);
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        out.assign(path, n);
        const size_t slash = out.find_last_of(L'\\');
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
    }
    CloseHandle(h);
    return out;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

struct EnumCtx {
    std::vector<TargetWindow>* out;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    // Never this process's own windows: none of them is a capture target, and asking a window
    // that another of our threads owns for its title SENDS it a message — the Settings window
    // would make the engine thread wait on the tray's UI thread (I15).
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) == 0) return TRUE;

    TargetWindow tw;
    if (!DescribeWindow(hwnd, &tw)) return TRUE;
    if (RectEmpty(tw.clientScreen)) return TRUE;
    ctx->out->push_back(tw);
    return TRUE;
}

// Case-insensitive substring test on wide strings.
bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::towlower);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return h.find(n) != std::wstring::npos;
}

}  // namespace

// ------------------------------------------------------------------- logging

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLogTo(stdout, fmt, ap);
    va_end(ap);
}

void LogErr(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLogTo(stderr, fmt, ap);
    va_end(ap);
}

std::string HrString(HRESULT hr) {
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08lX", static_cast<unsigned long>(hr));
    std::string out(buf);
    switch (hr) {
        case S_OK: out += " (S_OK)"; break;
        case S_FALSE: out += " (S_FALSE)"; break;
        case E_INVALIDARG: out += " (E_INVALIDARG)"; break;
        case E_OUTOFMEMORY: out += " (E_OUTOFMEMORY)"; break;
        case E_NOINTERFACE: out += " (E_NOINTERFACE)"; break;
        case E_ACCESSDENIED: out += " (E_ACCESSDENIED)"; break;
        case E_FAIL: out += " (E_FAIL)"; break;
        case DXGI_ERROR_DEVICE_REMOVED: out += " (DXGI_ERROR_DEVICE_REMOVED)"; break;
        case DXGI_ERROR_DEVICE_RESET: out += " (DXGI_ERROR_DEVICE_RESET)"; break;
        case DXGI_ERROR_WAS_STILL_DRAWING: out += " (DXGI_ERROR_WAS_STILL_DRAWING)"; break;
        case DXGI_ERROR_INVALID_CALL: out += " (DXGI_ERROR_INVALID_CALL)"; break;
        default: break;
    }
    return out;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

// ---------------------------------------------------------------------- time

int64_t QpcNow() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t QpcPerSecond() { return QpcFreq(); }

double QpcToMs(int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(QpcFreq());
}

double QpcToSec(int64_t ticks) {
    return static_cast<double>(ticks) / static_cast<double>(QpcFreq());
}

double Qpc100nsNow() {
    return static_cast<double>(QpcNow()) * 10000000.0 / static_cast<double>(QpcFreq());
}

// ------------------------------------------------------------------ geometry

std::string RectStr(const RECT& r) {
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%ld,%ld %ldx%ld", r.left, r.top, RectW(r), RectH(r));
    return std::string(buf);
}

// ------------------------------------------------------------------- device

bool CreateRenderDevice(ID3D11Device** device, ID3D11DeviceContext** ctx, std::string* err) {
    if (!device || !ctx) return false;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // DComp/D2D interop and the WGC projection
#ifdef NSP_D3D_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL got = static_cast<D3D_FEATURE_LEVEL>(0);
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want,
                                   ARRAYSIZE(want), D3D11_SDK_VERSION, device, &got, ctx);
    if (FAILED(hr)) {
        if (err) *err = "D3D11CreateDevice failed: " + HrString(hr);
        return false;
    }
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED((*device)->QueryInterface(__uuidof(ID3D10Multithread),
                                            reinterpret_cast<void**>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    return true;
}

// ---------------------------------------------------------------- displays

double MonitorRefreshHz(const RECT& screenRect, bool* exact) {
    if (exact) *exact = false;
    RECT r = screenRect;
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    if (!mon) return 60.0;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 60.0;

    // QueryDisplayConfig first: it reports the rational rate (165/1, or the
    // 60000/1001 kind), where EnumDisplaySettings rounds to a whole number.
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS &&
        pathCount > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount ? modeCount : 1);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                               modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < pathCount; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof(src);
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                if (_wcsicmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;
                const DISPLAYCONFIG_RATIONAL& rr = paths[i].targetInfo.refreshRate;
                if (rr.Denominator != 0) {
                    if (exact) *exact = true;
                    return static_cast<double>(rr.Numerator) / static_cast<double>(rr.Denominator);
                }
            }
        }
    }

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return static_cast<double>(dm.dmDisplayFrequency);
    return 60.0;
}

double DesktopMaxRefreshHz() {
    double best = 0.0;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                const double hz = MonitorRefreshHz(mi.rcMonitor, nullptr);
                double* out = reinterpret_cast<double*>(lp);
                if (hz > *out) *out = hz;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best > 1.0 ? best : 60.0;
}

// ------------------------------------------------------------- target window

bool IsFullscreenWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return false;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME) != 0) return false;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return EqualRect(&wr, &mi.rcMonitor) != FALSE;
}

bool DescribeWindow(HWND hwnd, TargetWindow* out) {
    if (!hwnd || !IsWindow(hwnd) || !out) return false;

    TargetWindow tw;
    tw.hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &tw.pid);

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    tw.cls = cls;

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    tw.title = title;

    tw.exe = ProcessImageName(tw.pid);
    tw.cloaked = IsCloaked(hwnd);

    if (!GetWindowRect(hwnd, &tw.windowRect)) return false;

    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return false;
    POINT pts[2] = {{cr.left, cr.top}, {cr.right, cr.bottom}};
    // MapWindowPoints returns 0 BOTH for failure and for a legitimate zero offset
    // — which is exactly what a maximised window at 0,0 has. GetLastError is not
    // cleared on success, so it must be zeroed first or a stale error from any
    // earlier call makes the most interesting window on the desktop invisible.
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(hwnd, nullptr, pts, 2) && GetLastError() != ERROR_SUCCESS) return false;
    tw.clientScreen = RECT{pts[0].x, pts[0].y, pts[1].x, pts[1].y};

    // The origin Windows.Graphics.Capture uses for a window item is the DWM
    // extended frame bounds, NOT GetWindowRect (which includes the invisible
    // resize border). Getting this wrong shifts the whole picture by ~7 px.
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &tw.frameBounds,
                                     sizeof(tw.frameBounds)))) {
        tw.frameBounds = tw.windowRect;
    }
    tw.fullscreen = IsFullscreenWindow(hwnd);

    *out = tw;
    return true;
}

std::vector<TargetWindow> EnumerateWindows() {
    std::vector<TargetWindow> out;
    EnumCtx ctx{&out};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

bool FindTarget(const std::wstring& cls, const std::wstring& titleSub, DWORD pid,
                TargetWindow* out, const std::wstring& exeSub, bool requireFullscreen,
                bool verbose) {
    if (!out) return false;
    const auto all = EnumerateWindows();
    const TargetWindow* best = nullptr;
    long long bestArea = 0;
    // The window the user is actually looking at wins over the biggest one. With
    // several browser windows open, "largest" attaches to whichever happens to be
    // maximised on the other monitor, which is never what was meant. Foreground
    // is only a preference: if it does not match the filters we fall back to
    // area, so a video on a second monitor still gets picked up.
    const HWND fg = GetForegroundWindow();
    for (const auto& w : all) {
        if (w.cloaked) continue;
        if (!cls.empty() && _wcsicmp(w.cls.c_str(), cls.c_str()) != 0) continue;
        if (!ContainsNoCase(w.title, titleSub)) continue;
        if (!ContainsNoCase(w.exe, exeSub)) continue;
        if (pid != 0 && w.pid != pid) continue;
        if (requireFullscreen && !w.fullscreen) continue;
        long long area =
            static_cast<long long>(RectW(w.clientScreen)) * RectH(w.clientScreen);
        if (w.hwnd == fg) area = (std::numeric_limits<long long>::max)();
        if (area > bestArea) {
            bestArea = area;
            best = &w;
        }
    }
    if (!best) {
        // Say WHY, per candidate: "no window matched" on its own sends the reader
        // hunting for a window that is right there on screen.
        for (const auto& w : all) {
            if (!verbose) break;
            if (!cls.empty() && _wcsicmp(w.cls.c_str(), cls.c_str()) != 0) continue;
            LogErr("  candidate 0x%p pid=%lu %s \"%s\": %s%s%s%s%s", static_cast<void*>(w.hwnd), w.pid,
                   Narrow(w.exe).c_str(), Narrow(w.title).c_str(), w.cloaked ? "CLOAKED " : "",
                   (pid != 0 && w.pid != pid) ? "pid mismatch " : "",
                   !ContainsNoCase(w.title, titleSub) ? "title mismatch " : "",
                   !ContainsNoCase(w.exe, exeSub) ? "exe mismatch " : "",
                   (requireFullscreen && !w.fullscreen) ? "not fullscreen" : "");
        }
        return false;
    }
    *out = *best;
    return true;
}

bool VideoRectObscured(HWND target, const RECT& videoScreen, HWND ignore,
                       OccluderInfo* who, const std::vector<std::wstring>* ignoreExes,
                       bool ignoreWidgets, OccluderInfo* widget) {
    static constexpr double kWidgetMaxAreaFraction = 0.10;
    if (widget) *widget = OccluderInfo{};
    if (!target || !IsWindow(target)) return false;
    if (RectW(videoScreen) <= 0 || RectH(videoScreen) <= 0) return false;

    // Measured 2026-09-13 on 2560x1440 primary + secondary:
    //   GetWindowRect for maximised Chrome: -8,-8  2576x1408
    //   DWMWA_EXTENDED_FRAME_BOUNDS       :  0,0   2560x1392
    // The 8 px invisible resize border reaches into the secondary monitor, so
    // GetWindowRect alone would flag that Chrome as an occluder of any video on
    // the far-left edge of the SECONDARY. Rule 4 corrects that by preferring the
    // extended frame bounds. Rule 8 (kMinDepthPx) then catches anything that
    // still touches an edge by only a sliver — e.g. Quotty, a resident topmost
    // utility sitting at x=2559, exactly one pixel column into the primary,
    // which under a plain any-overlap rule would pause fullscreen video forever.
    // A window must reach at least 8 px into the picture from any edge it
    // touches; anything whose overlap is fully interior counts at any size.
    static constexpr LONG kMinDepthPx = 8;

    // The first occluder is only reported once the walk has actually REACHED the
    // target: the z-order can change under a walk (the user clicks Chrome and it
    // rises past `w`), and a walk that runs off the bottom has been looking at
    // windows BELOW the browser (found in review). After a hit the rest of the walk
    // does nothing but look for the target.
    bool found = false;
    OccluderInfo first;
    HWND w = GetTopWindow(nullptr);
    for (; w && w != target; w = GetWindow(w, GW_HWNDNEXT)) {
        if (found) continue;
        // 1. Basic visibility.
        if (w == ignore) continue;
        if (!IsWindowVisible(w)) continue;
        if (IsIconic(w)) continue;

        // 2. Quick geometric reject before any DWM call — keeps this loop cheap
        //    enough to run many times a second.
        RECT wr{};
        if (!GetWindowRect(w, &wr)) continue;
        if (RectW(wr) <= 0 || RectH(wr) <= 0) continue;
        RECT tmp{};
        if (!IntersectRect(&tmp, &wr, &videoScreen)) continue;

        // 3. A cloaked window (background virtual desktop, suspended UWP app)
        //    occupies z-order but draws no pixels; treating it as an occluder
        //    would pause the engine permanently.
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
            cloaked != 0)
            continue;

        // 4. Prefer DWM extended frame bounds (excludes the invisible resize
        //    border) — see the measured facts in the comment above.
        RECT bounds{};
        RECT efb{};
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS,
                                            &efb, sizeof(efb))) &&
            RectW(efb) > 0 && RectH(efb) > 0) {
            bounds = efb;
        } else {
            bounds = wr;
        }

        // 5. Window region: the region box is relative to the window rect
        //    origin, so it must be shifted to screen coordinates before use.
        RECT rb{};
        const int rt = GetWindowRgnBox(w, &rb);
        if (rt == NULLREGION) continue;
        if (rt == SIMPLEREGION || rt == COMPLEXREGION) {
            OffsetRect(&rb, wr.left, wr.top);
            RECT clipped{};
            if (!IntersectRect(&clipped, &bounds, &rb)) continue;
            bounds = clipped;
        }
        // rt == ERROR means no region is set — keep bounds as-is.

        // 6. Layered windows that draw nothing, or that exist to be looked through.
        //    A constant alpha of 0 is invisible. A layered window with NO attribute
        //    state is either per-pixel (UpdateLayeredWindow) or was never set up,
        //    and together with WS_EX_TRANSPARENT that is an overlay or a helper —
        //    e.g. the visible 16x16 "Thread Event Target" every winit/Tao app puts
        //    at 0,0 at the top of the normal band (found in review). Skipping it
        //    cannot show a wrong picture: whatever it does draw is above the target
        //    and therefore above our overlay; it only avoids a pause that never
        //    clears. Click-through windows with a real constant alpha still count.
        {
            const LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
            if (ex & WS_EX_LAYERED) {
                BYTE alpha = 255;
                DWORD flags = 0;
                const bool haveAttrs = GetLayeredWindowAttributes(w, nullptr, &alpha, &flags) != FALSE;
                if (haveAttrs && (flags & LWA_ALPHA) && alpha == 0) continue;
                if (!haveAttrs && (ex & WS_EX_TRANSPARENT)) continue;
            }
        }

        // 7. Final intersection against the video rect.
        RECT o{};
        if (!IntersectRect(&o, &bounds, &videoScreen)) continue;

        // 8. Sliver rule — see the measured facts in the comment above.
        //    A window touching an edge of the video must reach at least
        //    kMinDepthPx pixels into the picture from that edge.
        if ((o.left   == videoScreen.left  && RectW(o) < kMinDepthPx) ||
            (o.right  == videoScreen.right && RectW(o) < kMinDepthPx) ||
            (o.top    == videoScreen.top   && RectH(o) < kMinDepthPx) ||
            (o.bottom == videoScreen.bottom && RectH(o) < kMinDepthPx))
            continue;

        // 8b. Widgets: small always-on-top windows (see the header). The size is
        //     the window's own, not its overlap, so a big always-on-top window
        //     pushed mostly off the picture still counts. Measured: Quotty is
        //     386x92, 1.7 % of a 1920x1080 picture.
        if (ignoreWidgets && (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            const double area = static_cast<double>(RectW(bounds)) * RectH(bounds);
            const double picture = static_cast<double>(RectW(videoScreen)) * RectH(videoScreen);
            if (area <= kWidgetMaxAreaFraction * picture) {
                if (widget && !widget->hwnd) {
                    widget->hwnd = w;
                    GetWindowThreadProcessId(w, &widget->pid);
                    wchar_t wcls[128] = {};
                    GetClassNameW(w, wcls, ARRAYSIZE(wcls));
                    widget->cls = wcls;
                    widget->exe = ProcessImageName(widget->pid);
                    widget->bounds = bounds;
                    widget->overlap = o;
                }
                continue;
            }
        }

        // 9. The user's ignore list. Looked up only here, for a window that really
        //    covers the picture, because it costs an OpenProcess.
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        std::wstring exe;
        if (ignoreExes && !ignoreExes->empty()) {
            exe = ProcessImageName(pid);
            std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
            if (std::find(ignoreExes->begin(), ignoreExes->end(), exe) != ignoreExes->end())
                continue;
        }

        // 10. Found an occluder; confirm it by reaching the target.
        found = true;
        {
            first.hwnd = w;
            first.pid = pid;
            wchar_t cls[128] = {};
            GetClassNameW(w, cls, ARRAYSIZE(cls));
            first.cls = cls;
            first.exe = exe.empty() ? ProcessImageName(pid) : exe;
            first.bounds = bounds;
            first.overlap = o;
        }
    }
    if (!found || w != target) return false;
    if (who) *who = first;
    return true;
}

}  // namespace nsp
