// Isolate per-cycle resource retention without showing windows or injecting input.
#include "nsp_capture.h"
#include "nsp_ofa.h"
#include "nsp_overlay.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <windows.graphics.capture.interop.h>

using Microsoft::WRL::ComPtr;

static void Pump(DWORD milliseconds) {
    const ULONGLONG end = GetTickCount64() + milliseconds;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    } while (GetTickCount64() < end);
}

static bool Cycle(const std::string& kind, HWND target, const std::shared_ptr<nsp::CaptureRuntime>& runtime,
                  std::string* error) {
    namespace wgc = winrt::Windows::Graphics::Capture;
    if (kind == "item-factory") {
        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        return factory != nullptr;
    }
    if (kind == "access-factory") {
        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureAccess, wgc::IGraphicsCaptureAccessStatics>();
        return factory != nullptr;
    }
    if (kind == "access" || kind == "access-close") {
        auto operation = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless);
        const auto status = operation.get();
        if (status != winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed) {
            *error = "borderless access was not allowed";
            return false;
        }
        if (kind == "access-close") operation.Close();
        return true;
    }
    if (kind == "item" || kind == "item-event") {
        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(factory->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                       winrt::put_abi(item)));
        if (kind == "item-event") {
            auto token = item.Closed([](auto const&, auto const&) {});
            item.Closed(token);
        }
        return true;
    }
    if (kind == "scout") {
        nsp::DamageScout scout(runtime);
        if (!scout.Start(target, error)) return false;
        std::vector<nsp::TimedDamage> damage;
        bool received = false;
        for (int i = 0; i < 100 && !received; ++i) {
            if (!scout.Poll(&damage)) { *error = "scout became invalid"; return false; }
            received = !damage.empty();
            Pump(10);
        }
        if (!received) { *error = "scout received no frames"; return false; }
        return true;
    }
    if (kind == "overlay") {
        RECT client{};
        if (!GetClientRect(target, &client)) { *error = "GetClientRect failed"; return false; }
        POINT origin{};
        if (!ClientToScreen(target, &origin)) { *error = "ClientToScreen failed"; return false; }
        RECT screen{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
        nsp::Overlay overlay;
        if (!overlay.Create(target, screen, screen, error)) return false;
        Pump(200);  // Keep the HWND hidden.
        return true;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context);
    if (FAILED(hr)) { *error = "D3D11CreateDevice failed"; return false; }
    if (kind == "capture") {
        nsp::Capture capture(runtime);
        nsp::CaptureOptions options;
        if (!capture.Start(device.Get(), context.Get(), target, RECT{0, 0, 512, 288}, options, error))
            return false;
        for (int i = 0; i < 100 && !capture.FramesIngested(); ++i) { capture.Poll(); Pump(10); }
        if (!capture.FramesIngested()) { *error = "capture received no frames"; return false; }
    } else if (kind == "ofa") {
        nsp::OfaFlow flow;
        if (!flow.Create(device.Get(), context.Get(), 512, 288, 4, error, true)) return false;
        Pump(200);
    } else if (kind != "device") {
        *error = "unknown kind";
        return false;
    }
    context->ClearState();
    context->Flush();
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: resource_cycle device|overlay|scout|capture|ofa|access|access-close|item|item-event HWND [cycles]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(strtoull(argv[2], nullptr, 0)));
    wchar_t title[512]{};
    GetWindowTextW(target, title, 512);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    RECT bounds{};
    if (!IsWindow(target) || !wcsstr(title, L"NSP windowed page") ||
        !GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &monitor) ||
        (monitor.dwFlags & MONITORINFOF_PRIMARY) || !GetWindowRect(target, &bounds) ||
        bounds.left < monitor.rcMonitor.left || bounds.top < monitor.rcMonitor.top ||
        bounds.right > monitor.rcMonitor.right || bounds.bottom > monitor.rcMonitor.bottom) {
        fprintf(stderr, "target must be the test page entirely on a secondary monitor\n");
        return 2;
    }
    const int cycles = argc > 3 ? atoi(argv[3]) : 20;
    if (cycles < 1 || cycles > 1000) return 2;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init)) return 1;
    auto runtime = std::make_shared<nsp::CaptureRuntime>();
    printf("kind,cycle,handles\n");
    for (int i = 0; i < cycles; ++i) {
        std::string error;
        bool ok = false;
        try {
            ok = Cycle(argv[1], target, runtime, &error);
        } catch (winrt::hresult_error const& e) {
            error = "WinRT error " + std::to_string(static_cast<unsigned>(e.code()));
        }
        if (!ok) {
            fprintf(stderr, "cycle %d: %s\n", i, error.c_str());
            runtime.reset();
            CoUninitialize();
            return 1;
        }
        Pump(250);
        DWORD handles = 0;
        if (!GetProcessHandleCount(GetCurrentProcess(), &handles)) {
            runtime.reset(); CoUninitialize(); return 1;
        }
        printf("%s,%d,%lu\n", argv[1], i, handles);
        fflush(stdout);
    }
    runtime.reset();
    CoUninitialize();
    return 0;
}
