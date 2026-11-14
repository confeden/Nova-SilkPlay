// Linked against a temporary copy of nsp_overlay.cpp with Present1 fault injection.
#include "nsp_overlay.h"
#include <dxgi.h>
#include <cstdio>

extern HRESULT g_presentFault;

static RECT g_secondary{};
static BOOL CALLBACK FindSecondary(HMONITOR monitor, HDC, LPRECT, LPARAM) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info) && !(info.dwFlags & MONITORINFOF_PRIMARY))
        g_secondary = info.rcMonitor;
    return TRUE;
}

int main() {
    using Result = nsp::Overlay::PresentResult;
    int failures = 0, checks = 0;
    const auto expect = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
    };
    {
        nsp::Overlay empty;
        expect(empty.Present() == Result::DeviceLost, "missing swapchain");
        expect(!empty.SetFrameLatency(2), "missing swapchain cannot change queue");
    }
    EnumDisplayMonitors(nullptr, nullptr, FindSecondary, 0);
    if (IsRectEmpty(&g_secondary)) {
        std::puts("FAIL: secondary monitor required");
        return 2;
    }
    RECT rect{g_secondary.left + 32, g_secondary.top + 32,
              g_secondary.left + 96, g_secondary.top + 96};
    // Never show the target or overlay; no activation, cursor or foreground input.
    HWND target = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"NSP present test",
                                 WS_POPUP, rect.left, rect.top, 64, 64,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!target) return 2;
    {
        nsp::Overlay overlay;
        std::string error;
        if (!overlay.Create(target, rect, rect, &error)) {
            std::printf("FAIL: %s\n", error.c_str());
            DestroyWindow(target);
            return 2;
        }
        expect(overlay.SetFrameLatency(2), "compositor-paced queue");
        expect(overlay.SetFrameLatency(1), "return to rate-gated queue");
        const auto submit = [&]() {
            ID3D11RenderTargetView* rtv = nullptr;
            if (!overlay.AcquireBackBuffer(nullptr, &rtv)) return Result::DeviceLost;
            const float color[4] = {0, 0, 0, 1};
            overlay.Context()->ClearRenderTargetView(rtv, color);
            return overlay.Present();
        };
        g_presentFault = E_FAIL;
        for (int i = 0; i < 7; ++i)
            expect(submit() == Result::Skipped, "transient failure is skipped");
        g_presentFault = S_OK;
        expect(submit() == Result::Presented, "real Present1 succeeds after failures");
        g_presentFault = E_FAIL;
        for (int i = 0; i < 7; ++i)
            expect(submit() == Result::Skipped, "success resets failure streak");
        expect(submit() == Result::DeviceLost, "eighth consecutive failure recovers");
        expect(submit() == Result::Skipped, "threshold resets failure streak");
        g_presentFault = DXGI_ERROR_DEVICE_REMOVED;
        expect(submit() == Result::DeviceLost, "device removal recovers immediately");
        g_presentFault = DXGI_ERROR_DEVICE_RESET;
        expect(submit() == Result::DeviceLost, "device reset recovers immediately");
        expect(!overlay.IsShown(), "test never shows overlay");
    }
    DestroyWindow(target);
    std::printf("present fault test: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
