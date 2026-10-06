#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

struct Args {
    int monitor = 2;
    int seconds = 20;
    int load = 90;
    bool clip = false;
    bool windowed = false;
};

Args ParseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--monitor" && i + 1 < argc) {
            args.monitor = std::stoi(argv[++i]);
        } else if (arg == "--seconds" && i + 1 < argc) {
            args.seconds = std::stoi(argv[++i]);
        } else if (arg == "--load" && i + 1 < argc) {
            args.load = std::stoi(argv[++i]);
        } else if (arg == "--clip") {
            args.clip = true;
        } else if (arg == "--windowed") {
            args.windowed = true;
        } else {
            fprintf(stderr, "FAILED: Unknown argument: %s\n", arg.c_str());
            exit(1);
        }
    }
    return args;
}

std::string HrString(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

struct MonitorInfo {
    int index;
    int target_index;
    RECT rect;
    bool found;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    (void)hMonitor;
    (void)hdcMonitor;
    MonitorInfo* info = reinterpret_cast<MonitorInfo*>(dwData);
    info->index++;
    if (info->index == info->target_index) {
        info->rect = *lprcMonitor;
        info->found = true;
        return FALSE;
    }
    return TRUE;
}

const char kShaderSource[] = R"HLSL(
RWStructuredBuffer<float> buf : register(u0);
cbuffer Params : register(b0) {
    uint iters;
    uint pad[3];
};

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float v = buf[id.x];
    for (uint i = 0; i < iters; ++i) {
        v = v * 1.000001f + 0.000001f;
    }
    buf[id.x] = v;
}
)HLSL";

int main(int argc, char** argv) {
    Args args = ParseArgs(argc, argv);

    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        fprintf(stderr, "FAILED: SetProcessDpiAwarenessContext failed\n");
        return 1;
    }

    MonitorInfo minfo = {0, args.monitor, {0, 0, 1280, 720}, false};
    if (!args.windowed) {
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&minfo));
        if (!minfo.found) {
            minfo.rect.left = 0; minfo.rect.top = 0;
            minfo.rect.right = GetSystemMetrics(SM_CXSCREEN);
            minfo.rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FakeGameClass";
    RegisterClassExW(&wc);

    DWORD style = WS_POPUP;
    if (args.windowed) {
        style = WS_OVERLAPPEDWINDOW;
    }
    
    int x = args.windowed ? CW_USEDEFAULT : minfo.rect.left;
    int y = args.windowed ? CW_USEDEFAULT : minfo.rect.top;
    int w = args.windowed ? 1280 : (minfo.rect.right - minfo.rect.left);
    int h = args.windowed ? 720 : (minfo.rect.bottom - minfo.rect.top);

    HWND hwnd = CreateWindowExW(0, L"FakeGameClass", L"FakeGame", style,
                                x, y, w, h,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fprintf(stderr, "FAILED: CreateWindowExW failed\n");
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    if (args.clip) {
        if (!args.windowed) {
            ClipCursor(&minfo.rect);
        } else {
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&clientRect), 2);
            ClipCursor(&clientRect);
        }
    }

    struct ClipReleaser {
        ~ClipReleaser() { ClipCursor(nullptr); }
    } clip_releaser;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, &featureLevel, 1,
                                   D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: D3D11CreateDevice failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device.As(&dxgiDevice);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: device.As(IDXGIDevice) failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: GetAdapter failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    DXGI_ADAPTER_DESC adapterDesc;
    hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: GetDesc failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), &factory);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: GetParent(IDXGIFactory2) failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapchain;
    hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swapchain);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: CreateSwapChainForHwnd failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: GetBuffer failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: CreateRenderTargetView failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    ComPtr<ID3DBlob> blob, errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "fakegame.hlsl",
                    nullptr, nullptr, "CSMain", "cs_5_0", flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string("FAILED: D3DCompile failed: ") + HrString(hr);
        if (errors && errors->GetBufferPointer()) {
            msg += " - ";
            msg.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        fprintf(stderr, "%s\n", msg.c_str());
        return 1;
    }

    ComPtr<ID3D11ComputeShader> cs;
    hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: CreateComputeShader failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = (1 << 20) * sizeof(float);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(float);

    ComPtr<ID3D11Buffer> srvBuf;
    hr = device->CreateBuffer(&bufDesc, nullptr, &srvBuf);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: CreateBuffer (structured) failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 1 << 20;

    ComPtr<ID3D11UnorderedAccessView> uav;
    hr = device->CreateUnorderedAccessView(srvBuf.Get(), &uavDesc, &uav);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: CreateUnorderedAccessView failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    struct Constants {
        UINT iters;
        UINT pad[3];
    };
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(Constants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> cb;
    hr = device->CreateBuffer(&cbDesc, nullptr, &cb);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: CreateBuffer (constant) failed: %s\n", HrString(hr).c_str());
        return 1;
    }

    // Mapping load to iterations. MEASURED 2026-09-20 on the RTX 5060 Ti: at
    // --load 90 with Present(1, 0) the process's own 3D engine sat at 15 %, not
    // 90 - the vsync wait bounds the work per frame, so the old constant was an
    // order of magnitude short of a real game. 8000 iterations per percent puts
    // --load 90 near a game-like utilisation; it is still approximate, and the
    // right way to use this tool is to read the utilisation the gate reports
    // rather than to trust the number on the command line.
    UINT base_iters_per_percent = 8000;
    UINT iters = static_cast<UINT>(args.load * base_iters_per_percent);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr, "FAILED: Map constant buffer failed: %s\n", HrString(hr).c_str());
        return 1;
    }
    Constants* constants = reinterpret_cast<Constants*>(mapped.pData);
    constants->iters = iters;
    ctx->Unmap(cb.Get(), 0);

    RECT r = args.windowed ? RECT{x, y, x + w, y + h} : minfo.rect;
    printf("rect: %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    printf("pid: %lu\n", GetCurrentProcessId());
    printf("adapter: %ls (LUID: 0x%08lx_0x%08lx)\n", adapterDesc.Description,
           static_cast<unsigned long>(adapterDesc.AdapterLuid.HighPart),
           static_cast<unsigned long>(adapterDesc.AdapterLuid.LowPart));
    fflush(stdout);

    LARGE_INTEGER freq, t0, t_start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_start);
    t0 = t_start;
    int frames = 0;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        LARGE_INTEGER t_now;
        QueryPerformanceCounter(&t_now);
        double elapsed = static_cast<double>(t_now.QuadPart - t_start.QuadPart) / static_cast<double>(freq.QuadPart);
        if (elapsed >= args.seconds) {
            break;
        }

        double dt = static_cast<double>(t_now.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
        if (dt >= 1.0) {
            printf("%d fps\n", frames);
            fflush(stdout);
            frames = 0;
            t0 = t_now;
        }

        float c[4];
        c[0] = static_cast<float>(0.5 + 0.5 * sin(elapsed));
        c[1] = static_cast<float>(0.5 + 0.5 * sin(elapsed * 1.3));
        c[2] = static_cast<float>(0.5 + 0.5 * sin(elapsed * 1.7));
        c[3] = 1.0f;

        ctx->ClearRenderTargetView(rtv.Get(), c);
        
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->Dispatch(1024, 1, 1);

        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

        hr = swapchain->Present(1, 0);
        if (FAILED(hr)) {
            fprintf(stderr, "FAILED: Present failed: %s\n", HrString(hr).c_str());
            return 1;
        }
        frames++;
    }

    printf("exiting\n");
    return 0;
}
