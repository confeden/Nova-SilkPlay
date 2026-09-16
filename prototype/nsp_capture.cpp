// nsp_capture.cpp — WGC -> two-slot GPU ring of video-rect crops.

#include "nsp_capture.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <objbase.h>

// C++/WinRT projections first, interop headers after.
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>

namespace nsp {
namespace {

namespace wf = winrt::Windows::Foundation;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wacc = winrt::Windows::Security::Authorization::AppCapabilityAccess;

using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

constexpr DWORD kAccessRequestTimeoutMs = 5000;

// ---------------------------------------------------------------------------
// WHICH CAPTURED FRAMES ARE VIDEO FRAMES.
//
// WGC delivers a frame whenever ANY part of the window changes, and a player
// changes a lot besides its video: the controls fade in and out when the pointer
// moves, the progress bar and clock tick, the title changes, a play/pause flash
// animates. MEASURED on testmotion.html?controls=1 (2026-09-13): while the
// controls faded, frames arrived at the full compositor rate with the same video
// frame in them; the arrival-based period estimate fell from 24 to 37-55 fps,
// ~90 % of presents juddered by more than 4 ms, and it took seconds to recover
// after the pointer stopped. That is exactly "the cursor on the video ruins
// frame generation".
//
// Two tests, cheapest first:
//   1. DWM's dirty regions (build 26100+). A frame whose damage does not reach
//      the middle band of the picture - a progress label, the caption bar, the
//      window border lighting up, a centred play/pause flash - cannot carry a new
//      video frame. Measured shapes: progress 207x50, flash 151x151, border
//      strips 1-8 px, against 938x526 for a video frame.
//   2. The fade itself is NOT separable by damage: Chrome reports one union rect
//      per compositor frame, so a top bar and a bottom bar fading together damage
//      the whole picture exactly like a new video frame does. What does separate
//      them is content: the same decoded video frame recomposited under a fading
//      bar is bit-identical in the MIDDLE band, where no bar reaches. EVERY pixel
//      of that band is compared on the GPU, in 16x16 blocks. A sparse grid was
//      tried first (64x24 samples, one per ~26x23 px) and measured missing real
//      frames: on analytic scene A3 — one object moving 11 px per frame over a
//      static background — the engine saw 13.7 of 24 frames a second, because a
//      small object's edges moved between the samples (tools/ls-compare).
//
// A frame that passes is a new video frame: the ring advances and the cadence
// sees it. A frame that fails is the same video frame with different UI: it
// REPLACES the newest slot in place, keeping that slot's timestamp, so the UI
// stays current while the pair and the cadence do not move.
constexpr double kBandLeft = 0.06, kBandRight = 0.94;  // of the source rect width
constexpr double kBandTop = 0.20, kBandBottom = 0.72;  // of its height: clear of bars and captions
constexpr UINT kBlockPx = 16;                          // the band is compared in blocks this size
constexpr float kLevelThreshold = 3.5f / 255.0f;       // a per-channel change of 4+ code values
constexpr UINT kMinChangedPixelsInBlock = 2;           // of its 64 samples (every 2nd pixel both ways)
constexpr UINT kMinChangedSamples = 1;                 // changed BLOCKS; fewer is "identical"
constexpr double kCoverageHalfLifeSec = 1.0;

constexpr char kChangeTestHlsl[] = R"(
Texture2D<float4> NewF : register(t0);
Texture2D<float4> OldF : register(t1);
RWStructuredBuffer<uint> Blocks : register(u0);
cbuffer C : register(b0) { uint x0; uint y0; uint block; uint nbx; uint nby; float thr; uint pad0; uint pad1; };
// One thread per block, each writing only its own element: no shared counter.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nbx || id.y >= nby) return;
    uint changed = 0;
    uint ox = x0 + id.x * block, oy = y0 + id.y * block;
    [loop] for (uint j = 0; j < block; j += 2) {
        [loop] for (uint i = 0; i < block; i += 2) {
            int3 p = int3(ox + i, oy + j, 0);
            float3 d = abs(NewF.Load(p).rgb - OldF.Load(p).rgb);
            if (max(d.r, max(d.g, d.b)) > thr) changed += 1;
        }
    }
    Blocks[id.y * nbx + id.x] = changed;
}
)";

struct ChangeCb {
    UINT x0, y0, block, nbx, nby;
    float thr;
    UINT pad0, pad1;
};

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return std::string("<format error>");
    return std::string(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                 : sizeof(buf) - 1);
}

const char* AccessStatusName(wacc::AppCapabilityAccessStatus s) {
    switch (s) {
        case wacc::AppCapabilityAccessStatus::DeniedBySystem: return "DeniedBySystem";
        case wacc::AppCapabilityAccessStatus::NotDeclaredByApp: return "NotDeclaredByApp";
        case wacc::AppCapabilityAccessStatus::DeniedByUser: return "DeniedByUser";
        case wacc::AppCapabilityAccessStatus::UserPromptRequired: return "UserPromptRequired";
        case wacc::AppCapabilityAccessStatus::Allowed: return "Allowed";
        default: return "<unknown>";
    }
}

// Bounded, coroutine-free wait on an IAsyncOperation (see the probe: ::get()
// deadlocks if the apartment turned out to be STA).
template <typename TResult>
HRESULT AwaitOperation(wf::IAsyncOperation<TResult> const& op, DWORD timeoutMs, TResult* result) {
    if (!op) return E_POINTER;
    std::shared_ptr<void> evt(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](HANDLE h) {
        if (h) CloseHandle(h);
    });
    if (!evt.get()) return HRESULT_FROM_WIN32(GetLastError());
    try {
        op.Completed([evt](wf::IAsyncOperation<TResult> const&, wf::AsyncStatus) {
            SetEvent(evt.get());
        });
    } catch (winrt::hresult_error const& e) {
        return e.code();
    }
    const DWORD w = WaitForSingleObject(evt.get(), timeoutMs);
    if (w == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (w != WAIT_OBJECT_0) return E_FAIL;
    try {
        *result = op.GetResults();
    } catch (winrt::hresult_error const& e) {
        return e.code();
    }
    return S_OK;
}

}  // namespace

struct Capture::Impl {
    ID3D11Device* device = nullptr;         // borrowed: owned by Overlay
    ID3D11DeviceContext* ctx = nullptr;     // borrowed
    // Five slots, five roles, and every change of role is an index shuffle — no
    // pixel is copied twice.
    //   older         the video frame before `prev`. The engine keeps a frame of
    //                 history so that it can show a source period LATE with a
    //                 jitter margin and still always have the pair that brackets
    //                 the moment on screen (P17, main.cpp "PACING").
    //   prev, newest  the newest pair. `newest` is the latest capture of the
    //                 newest video frame, UI refreshes included.
    //   ref           that video frame EXACTLY as it was captured. The content
    //                 test compares against it, not against `newest`: measured
    //                 against the last refresh instead, a change arriving a few
    //                 samples at a time (a slow pan, a dim scene) never added up
    //                 to a detection however long it went on (found in review).
    //                 `newest` starts on the same slot and leaves it at the first
    //                 refresh.
    //   pending       where the next capture lands until it has been classified.
    static constexpr int kSlots = 5;
    winrt::com_ptr<ID3D11Texture2D> ring[kSlots];
    winrt::com_ptr<ID3D11ShaderResourceView> ringSrv[kSlots];     // _SRGB: synthesis reads linear light
    winrt::com_ptr<ID3D11ShaderResourceView> ringSrvRaw[kSlots];  // _UNORM: the change test compares code values
    int older = -1, prev = -1, ref = -1, newest = -1;  // -1 = nothing yet
    int pending = 0;
    int filled = 0;                  // how many video frames are held (0..3)
    double olderTime100ns = 0.0;     // SystemRelativeTime of the three video frames
    double prevTime100ns = 0.0;
    double newestTime100ns = 0.0;

    // A slot no role uses. Five slots and at most four roles besides `pending`
    // guarantee one exists.
    int FreeSlot(int a, int b, int c, int d) const {
        for (int i = 0; i < kSlots; ++i)
            if (i != a && i != b && i != c && i != d) return i;
        return -1;
    }

    // The change test (see "WHICH CAPTURED FRAMES ARE VIDEO FRAMES").
    winrt::com_ptr<ID3D11ComputeShader> changeCs;
    winrt::com_ptr<ID3D11Buffer> changeCb, changeOut, changeStaging;
    winrt::com_ptr<ID3D11UnorderedAccessView> changeUav;
    UINT blocksX = 0, blocksY = 0, bandX0 = 0, bandY0 = 0;  // the block grid over the band
    bool changeTestOk = false;
    double coverageMax = 0.0;    // decaying max of the band coverage of recent VIDEO frames
    // The damage rect of recent video frames. One <video> damages the same quad
    // with every frame; a play/pause flash, a spinner or a tooltip changes shape
    // from frame to frame. The most common recent rect is "the picture".
    static constexpr int kQuadHistory = 16;
    RECT videoRects[kQuadHistory] = {};
    int videoRectCount = 0, videoRectNext = 0;
    void RememberVideoRect(const RECT& r) {
        videoRects[videoRectNext] = r;
        videoRectNext = (videoRectNext + 1) % kQuadHistory;
        if (videoRectCount < kQuadHistory) ++videoRectCount;
    }
    // True when `r` is, within 2 px, the rect at least 4 of the recent video
    // frames damaged. False while no such rect exists yet.
    bool IsVideoQuad(const RECT& r) const {
        // NB: `near` and `small` are macros in the Windows headers.
        const auto closeTo = [](const RECT& a, const RECT& b) {
            return labs(a.left - b.left) <= 2 && labs(a.top - b.top) <= 2 &&
                   labs(a.right - b.right) <= 2 && labs(a.bottom - b.bottom) <= 2;
        };
        int best = 0;
        RECT mode{};
        for (int i = 0; i < videoRectCount; ++i) {
            int n = 0;
            for (int j = 0; j < videoRectCount; ++j)
                if (closeTo(videoRects[i], videoRects[j])) ++n;
            if (n > best) {
                best = n;
                mode = videoRects[i];
            }
        }
        return best >= 4 && closeTo(mode, r);
    }
    double lastPictureActivity100ns = 0.0;  // Qpc100nsNow() of the last picture repaint
    double lastSeen100ns = 0.0;  // SystemRelativeTime of the last capture looked at
    CaptureStats stats;

    RECT srcRect{};
    UINT surfaceW = 0, surfaceH = 0;  // the pool's surface size

    wgdx::Direct3D11::IDirect3DDevice rtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token closedToken{};
    std::shared_ptr<std::atomic<bool>> itemClosed = std::make_shared<std::atomic<bool>>(false);

    bool started = false;
    bool needsReengage = false;
    bool logDirty = false;
    bool videoFrameTest = true;
    uint64_t framesIngested = 0;
    std::string report;

    void Add(const std::string& line) {
        report += line;
        report += '\n';
    }

    bool AllocRing(UINT w, UINT h, std::string* err);
    bool CreateChangeTest(std::string* err);

    // THE CONTENT TEST IS ASYNCHRONOUS. A blocking readback waits for every GPU
    // command queued before it — the warp, NVOFA — and measured at fullscreen it
    // waited 3.5-4.4 ms on average and 20 ms at worst, a whole 60 Hz frame. The
    // test is submitted instead, and Poll() collects the answer on a later call;
    // until then no further capture is taken, so captures are still decided in
    // order. Everything test 1 learned about the capture waits in `waiting`.
    struct Capture1 {
        double t100 = 0.0;
        double dtSec = 0.0;
        bool haveRegions = false;
        int nRects = -1;
        double coverage = 0.0;
        double bar = 0.0;
        RECT damageBox{};
        std::string rectText;
    };
    bool testPending = false;
    Capture1 waiting;
    std::chrono::steady_clock::time_point testSubmitted;
    bool StartTest(int a, int b);
    // True once the answer is in (`*changed` = changed blocks, -1 if unreadable).
    bool TryFinishTest(int* changed);
    // Applies a verdict to the ring, the cadence inputs and the statistics.
    void Commit(bool advance, const char* why, int changed, const Capture1& c, int* ingested);
};

bool Capture::Impl::AllocRing(UINT w, UINT h, std::string* err) {
    for (int i = 0; i < kSlots; ++i) {
        ring[i] = nullptr;
        ringSrv[i] = nullptr;
        ringSrvRaw[i] = nullptr;
    }
    older = prev = ref = newest = -1;
    pending = 0;
    filled = 0;
    olderTime100ns = prevTime100ns = newestTime100ns = 0.0;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    // TYPELESS, not UNORM: an SRV may only reinterpret the format when the
    // resource itself is typeless (E_INVALIDARG otherwise). Copies from the WGC
    // frame's B8G8R8A8_UNORM surface are still legal — same typeless family.
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    // _SRGB view: the synthesis shader samples linear light (directives §4).
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;

    D3D11_SHADER_RESOURCE_VIEW_DESC rawd = sd;
    rawd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    for (int i = 0; i < kSlots; ++i) {
        HRESULT hr = device->CreateTexture2D(&td, nullptr, ring[i].put());
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(ring[i].get(), &sd, ringSrv[i].put());
        if (SUCCEEDED(hr))
            hr = device->CreateShaderResourceView(ring[i].get(), &rawd, ringSrvRaw[i].put());
        if (FAILED(hr)) {
            if (err) *err = Fmt("ring CreateTexture2D/SRV (%ux%u) failed: %s", w, h,
                                HrString(hr).c_str());
            return false;
        }
    }
    return true;
}

bool Capture::Impl::CreateChangeTest(std::string* err) {
    changeTestOk = false;
    winrt::com_ptr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kChangeTestHlsl, sizeof(kChangeTestHlsl) - 1, "change_test", nullptr,
                            nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(),
                            errors.put());
    if (FAILED(hr)) {
        if (err)
            *err = Fmt("change test compile failed %s: %s", HrString(hr).c_str(),
                       errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                     changeCs.put());

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(ChangeCb);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&cbd, nullptr, changeCb.put());

    const LONG sw = RectW(srcRect), sh = RectH(srcRect);
    bandX0 = static_cast<UINT>(sw * kBandLeft);
    bandY0 = static_cast<UINT>(sh * kBandTop);
    blocksX = (static_cast<UINT>(sw * kBandRight) - bandX0) / kBlockPx;
    blocksY = (static_cast<UINT>(sh * kBandBottom) - bandY0) / kBlockPx;
    if (blocksX == 0 || blocksY == 0) {
        if (err) *err = "picture too small for the content test";
        return false;
    }

    D3D11_BUFFER_DESC od = {};
    od.ByteWidth = sizeof(UINT) * blocksX * blocksY;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    od.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    od.StructureByteStride = sizeof(UINT);
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&od, nullptr, changeOut.put());
    if (SUCCEEDED(hr)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = blocksX * blocksY;
        hr = device->CreateUnorderedAccessView(changeOut.get(), &ud, changeUav.put());
    }
    D3D11_BUFFER_DESC sdsc = od;  // the staging twin of a structured buffer keeps its misc flags
    sdsc.Usage = D3D11_USAGE_STAGING;
    sdsc.BindFlags = 0;
    sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&sdsc, nullptr, changeStaging.put());
    if (FAILED(hr)) {
        if (err) *err = Fmt("change test resources failed %s", HrString(hr).c_str());
        return false;
    }
    changeTestOk = true;
    return true;
}

bool Capture::Impl::StartTest(int a, int b) {
    if (!changeTestOk || a < 0 || b < 0) return false;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(changeCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    ChangeCb c{};
    c.x0 = bandX0;
    c.y0 = bandY0;
    c.block = kBlockPx;
    c.nbx = blocksX;
    c.nby = blocksY;
    c.thr = kLevelThreshold;
    memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(changeCb.get(), 0);

    ID3D11ShaderResourceView* srvs[2] = {ringSrvRaw[a].get(), ringSrvRaw[b].get()};
    ID3D11UnorderedAccessView* uav = changeUav.get();
    ID3D11Buffer* cb = changeCb.get();
    ctx->CSSetShader(changeCs.get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->Dispatch((blocksX + 7) / 8, (blocksY + 7) / 8, 1);
    ID3D11ShaderResourceView* nullSrv[2] = {nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CopyResource(changeStaging.get(), changeOut.get());
    // Without a flush the dispatch can sit in the runtime's command buffer until
    // the next present, and a non-blocking Map would never see it finish.
    ctx->Flush();
    testPending = true;
    testSubmitted = std::chrono::steady_clock::now();
    return true;
}

bool Capture::Impl::TryFinishTest(int* changed) {
    D3D11_MAPPED_SUBRESOURCE r = {};
    const HRESULT hr = ctx->Map(changeStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &r);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    testPending = false;
    const double latencyMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - testSubmitted).count();
    ++stats.contentTests;
    stats.testWaitMsSum += latencyMs;
    stats.testWaitMsMax = (std::max)(stats.testWaitMsMax, latencyMs);
    if (FAILED(hr)) {
        *changed = -1;
        return true;
    }
    const auto* blocks = static_cast<const UINT*>(r.pData);
    UINT n = 0;
    for (UINT i = 0, total = blocksX * blocksY; i < total; ++i)
        if (blocks[i] >= kMinChangedPixelsInBlock) ++n;
    ctx->Unmap(changeStaging.get(), 0);
    *changed = static_cast<int>(n);
    return true;
}

void Capture::Impl::Commit(bool advance, const char* why, int changed, const Capture1& c, int* ingested) {
    // The picture was repainted — by a new video frame or by the same one
    // recomposited — whenever the damage reached the band and was not dismissed
    // as small UI. That, not "a new video frame", is what tells a still shot from
    // a stopped source.
    if (advance || changed >= 0 || !c.haveRegions) lastPictureActivity100ns = Qpc100nsNow();
    if (advance && c.haveRegions) {
        coverageMax = (std::max)(coverageMax, c.coverage);
        if (changed >= 0) RememberVideoRect(c.damageBox);
    }
    if (logDirty)
        Log("[frame] dt %6.1f ms  n %d cover %.2f (max %.2f)  changed %d  -> %s (%s)%s", c.dtSec * 1000.0,
            c.nRects, c.coverage, coverageMax, changed, advance ? "VIDEO" : "ui", why, c.rectText.c_str());
    if (advance) {
        if (filled >= 2) {
            older = prev;
            olderTime100ns = prevTime100ns;
        }
        if (filled >= 1) {
            prev = newest;  // with its latest UI, not the raw capture
            prevTime100ns = newestTime100ns;
        }
        ref = newest = pending;
        newestTime100ns = c.t100;
        pending = FreeSlot(older, prev, ref, -1);
        if (filled < 3) ++filled;
        ++framesIngested;
        ++*ingested;
    } else {
        // Same video frame, newer UI: it becomes `newest` and keeps that frame's
        // timestamp, so neither the pair nor the cadence moves; `ref` stays the
        // frame as first captured.
        newest = pending;
        pending = FreeSlot(older, prev, ref, newest);
        ++stats.uiRefreshes;
    }
}

Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() { Stop(); }

const RECT& Capture::SourceRect() const { return impl_->srcRect; }
uint64_t Capture::FramesIngested() const { return impl_->framesIngested; }
bool Capture::NeedsReengage() const { return impl_->needsReengage; }
const std::string& Capture::StartupReport() const { return impl_->report; }

ID3D11ShaderResourceView* Capture::NewestSrv() const {
    const Impl& s = *impl_;
    if (s.newest < 0 || s.filled < 1) return nullptr;
    return s.ringSrv[s.newest].get();
}

ID3D11Texture2D* Capture::NewestTex() const {
    const Impl& s = *impl_;
    if (s.newest < 0 || s.filled < 1) return nullptr;
    return s.ring[s.newest].get();
}

ID3D11Texture2D* Capture::PreviousTex() const {
    const Impl& s = *impl_;
    if (s.prev < 0 || s.filled < 2) return nullptr;
    return s.ring[s.prev].get();
}

ID3D11ShaderResourceView* Capture::PreviousSrv() const {
    const Impl& s = *impl_;
    if (s.prev < 0 || s.filled < 2) return nullptr;
    return s.ringSrv[s.prev].get();
}

double Capture::NewestTime100ns() const {
    const Impl& s = *impl_;
    return (s.newest < 0) ? 0.0 : s.newestTime100ns;
}

double Capture::PreviousTime100ns() const {
    const Impl& s = *impl_;
    return (s.prev < 0 || s.filled < 2) ? 0.0 : s.prevTime100ns;
}

ID3D11ShaderResourceView* Capture::OlderSrv() const {
    const Impl& s = *impl_;
    if (s.older < 0 || s.filled < 3) return nullptr;
    return s.ringSrv[s.older].get();
}

double Capture::OlderTime100ns() const {
    const Impl& s = *impl_;
    return (s.older < 0 || s.filled < 3) ? 0.0 : s.olderTime100ns;
}

double Capture::LastPictureActivity100ns() const { return impl_->lastPictureActivity100ns; }

CaptureStats Capture::TakeStats() {
    CaptureStats out = impl_->stats;
    impl_->stats = CaptureStats{};
    return out;
}

bool Capture::SetSourceOffset(LONG x, LONG y) {
    Impl& s = *impl_;
    const LONG w = RectW(s.srcRect), h = RectH(s.srcRect);
    if (w <= 0 || h <= 0) return false;
    s.srcRect = RECT{x, y, x + w, y + h};
    return true;
}

bool Capture::Start(ID3D11Device* device, ID3D11DeviceContext* ctx, HWND target,
                    const RECT& srcRect, const CaptureOptions& opt, std::string* err) {
    Impl& s = *impl_;
    s.report.clear();
    s.device = device;
    s.ctx = ctx;
    s.srcRect = srcRect;
    s.needsReengage = false;

    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        s.Add("FATAL: " + m);
        return false;
    };

    if (!device || !ctx) return fail("no device");
    if (!target || !IsWindow(target)) return fail("target HWND is not a window");
    if (RectEmpty(srcRect)) return fail("empty source rect");

    bool supported = false;
    try {
        supported = wgc::GraphicsCaptureSession::IsSupported();
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("GraphicsCaptureSession::IsSupported threw %s", HrString(e.code()).c_str()));
    }
    if (!supported) return fail("GraphicsCaptureSession::IsSupported() == false");

    // ---- the WinRT wrapper around OUR (borrowed) device
    {
        winrt::com_ptr<IDXGIDevice> dxgi;
        const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(dxgi.put()));
        if (FAILED(hr)) return fail(Fmt("QI IDXGIDevice failed: %s", HrString(hr).c_str()));
        winrt::com_ptr<::IInspectable> inspectable;
        const HRESULT hr2 = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put());
        if (FAILED(hr2) || !inspectable)
            return fail(Fmt("CreateDirect3D11DeviceFromDXGIDevice failed: %s", HrString(hr2).c_str()));
        try {
            s.rtDevice = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();
        } catch (winrt::hresult_error const& e) {
            return fail(Fmt("IInspectable -> IDirect3DDevice failed: %s", HrString(e.code()).c_str()));
        }
    }

    // ---- the item
    {
        winrt::hresult_error factoryError{};
        auto interop = winrt::try_get_activation_factory<wgc::GraphicsCaptureItem,
                                                         ::IGraphicsCaptureItemInterop>(factoryError);
        if (!interop)
            return fail(Fmt("IGraphicsCaptureItemInterop activation failed: %s",
                            HrString(factoryError.code()).c_str()));
        const HRESULT hr = interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                    winrt::put_abi(s.item));
        if (FAILED(hr) || !s.item)
            return fail(Fmt("CreateForWindow failed: %s", HrString(hr).c_str()));
    }

    winrt::Windows::Graphics::SizeInt32 size{};
    try {
        size = s.item.Size();
        s.Add(Fmt("item.Size: %dx%d  (\"%s\")", size.Width, size.Height,
                  Narrow(std::wstring(s.item.DisplayName().c_str())).c_str()));
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("item.Size threw %s", HrString(e.code()).c_str()));
    }
    if (size.Width <= 0 || size.Height <= 0)
        return fail(Fmt("degenerate item size %dx%d", size.Width, size.Height));
    s.surfaceW = static_cast<UINT>(size.Width);
    s.surfaceH = static_cast<UINT>(size.Height);

    // ---- the ring, sized to the video rect
    {
        std::string ringErr;
        if (!s.AllocRing(static_cast<UINT>(RectW(srcRect)), static_cast<UINT>(RectH(srcRect)),
                         &ringErr))
            return fail(ringErr);
        s.Add(Fmt("ring: 4 x %ldx%ld BGRA8 (SRV format BGRA8_SRGB), source rect %s", RectW(srcRect),
                  RectH(srcRect), RectStr(srcRect).c_str()));
        std::string ctErr;
        s.coverageMax = 0.0;
        s.lastSeen100ns = 0.0;
        s.lastPictureActivity100ns = 0.0;
        s.videoRectCount = s.videoRectNext = 0;
        s.stats = CaptureStats{};
        if (s.CreateChangeTest(&ctErr)) {
            s.Add(Fmt("video-frame test: dirty regions + every middle-band pixel (%ux%u blocks of %u px)",
                      s.blocksX, s.blocksY, kBlockPx));
        } else {
            s.Add("content test UNAVAILABLE (" + ctErr + ") - captures are told apart by damage alone, "
                  "so a player-UI fade counts as a video frame");
        }
    }

    // ---- pool + session
    try {
        s.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s.rtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        s.session = s.pool.CreateCaptureSession(s.item);
        s.Add("framePool.CreateFreeThreaded + CreateCaptureSession: ok");
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("frame pool / session creation failed: %s", HrString(e.code()).c_str()));
    }

    // G14: the capability must be requested BEFORE IsBorderRequired is touched.
    // Measured on this machine (2026-09-06): Allowed, readback false — i.e. an
    // unpackaged per-user process CAN drop the yellow capture border here.
    if (opt.tryBorderless) {
        auto status = wacc::AppCapabilityAccessStatus::DeniedBySystem;
        HRESULT hr = E_FAIL;
        try {
            auto op = wgc::GraphicsCaptureAccess::RequestAccessAsync(
                wgc::GraphicsCaptureAccessKind::Borderless);
            hr = AwaitOperation(op, kAccessRequestTimeoutMs, &status);
        } catch (winrt::hresult_error const& e) {
            hr = e.code();
        }
        s.Add(SUCCEEDED(hr) ? Fmt("RequestAccessAsync(Borderless): %s", AccessStatusName(status))
                            : Fmt("RequestAccessAsync(Borderless): FAILED %s", HrString(hr).c_str()));
        try {
            s.session.IsBorderRequired(false);
            s.Add(Fmt("IsBorderRequired(false) -> readback %s",
                      s.session.IsBorderRequired() ? "true (BORDER STILL ON)" : "false"));
        } catch (winrt::hresult_error const& e) {
            s.Add(Fmt("IsBorderRequired: THREW %s", HrString(e.code()).c_str()));
        }
    }

    // Build 26100 reports which parts of the window changed with every frame.
    // ReportOnly: the frame is still complete, the regions are information.
    s.logDirty = opt.logDirty;
    s.videoFrameTest = opt.videoFrameTest;
    try {
        s.session.DirtyRegionMode(wgc::GraphicsCaptureDirtyRegionMode::ReportOnly);
        s.Add("DirtyRegionMode(ReportOnly): ok");
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("DirtyRegionMode: unavailable %s", HrString(e.code()).c_str()));
    }

    try {
        s.session.IsCursorCaptureEnabled(opt.captureCursor);  // I12: false
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("IsCursorCaptureEnabled: THREW %s", HrString(e.code()).c_str()));
    }

    // G4: without this WGC caps near 50 fps on build 26100.
    try {
        const auto requested = std::chrono::duration_cast<wf::TimeSpan>(
            std::chrono::duration<double, std::milli>(opt.minUpdateIntervalMs));
        s.session.MinUpdateInterval(requested);
        s.Add(Fmt("MinUpdateInterval(%.3f ms) -> readback %.3f ms", opt.minUpdateIntervalMs,
                  static_cast<double>(s.session.MinUpdateInterval().count()) / 10000.0));
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("MinUpdateInterval: THREW %s (G4 cap cannot be lifted)",
                  HrString(e.code()).c_str()));
    }

    try {
        s.itemClosed = std::make_shared<std::atomic<bool>>(false);
        auto flag = s.itemClosed;
        s.closedToken = s.item.Closed([flag](wgc::GraphicsCaptureItem const&, wf::IInspectable const&) {
            flag->store(true, std::memory_order_release);
        });
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("item.Closed registration THREW %s", HrString(e.code()).c_str()));
    }

    try {
        s.session.StartCapture();
        s.Add("session.StartCapture: ok");
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("StartCapture failed: %s", HrString(e.code()).c_str()));
    }

    s.started = true;
    s.framesIngested = 0;
    return true;
}

int Capture::Poll() {
    Impl& s = *impl_;
    if (!s.started || s.needsReengage) return 0;
    if (s.itemClosed->load(std::memory_order_acquire)) {
        LogErr("Capture: item Closed (target window gone) - re-engage needed");
        s.needsReengage = true;
        return 0;
    }

    int ingested = 0;
    // A capture still waiting for its content test is decided before any newer
    // one is taken; newer captures wait in the frame pool meanwhile.
    const auto conclude = [&](int changed) {
        bool advance = true;
        const char* why = "middle band changed";
        if (changed < 0) {
            why = "content test failed";
        } else if (static_cast<UINT>(changed) < kMinChangedSamples) {
            advance = false;
            why = "middle band identical";
        } else if (s.waiting.haveRegions && s.waiting.coverage < s.waiting.bar) {
            // A real frame under the bar: the bar was inflated by a union, so it
            // re-seats on this picture's own size.
            s.coverageMax = s.waiting.coverage;
        }
        s.Commit(advance, why, changed, s.waiting, &ingested);
    };
    if (s.testPending) {
        int changed = -1;
        if (!s.TryFinishTest(&changed)) return 0;
        conclude(changed);
    }
    for (;;) {
        bool leftPending = false;
        wgc::Direct3D11CaptureFrame frame{nullptr};
        try {
            frame = s.pool.TryGetNextFrame();
        } catch (winrt::hresult_error const& e) {
            LogErr("Capture: TryGetNextFrame failed %s - capture stopped", HrString(e.code()).c_str());
            s.needsReengage = true;
            return ingested;
        }
        if (!frame) break;

        bool sizeChanged = false;
        try {
            const auto content = frame.ContentSize();
            if (content.Width > 0 && content.Height > 0 &&
                (static_cast<UINT>(content.Width) != s.surfaceW ||
                 static_cast<UINT>(content.Height) != s.surfaceH)) {
                LogErr("Capture: content size changed %ux%u -> %dx%d - re-engage needed", s.surfaceW,
                       s.surfaceH, content.Width, content.Height);
                sizeChanged = true;
            }

            if (!sizeChanged) {
                winrt::com_ptr<ID3D11Texture2D> tex;
                auto access = frame.Surface().as<DxgiInterfaceAccess>();
                const HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
                if (FAILED(hr) || !tex) {
                    LogErr("Capture: GetInterface(ID3D11Texture2D) failed %s", HrString(hr).c_str());
                    s.needsReengage = true;
                    frame.Close();
                    return ingested;
                }

                D3D11_TEXTURE2D_DESC srcDesc{};
                tex->GetDesc(&srcDesc);

                const double t100 = static_cast<double>(frame.SystemRelativeTime().count());
                const double dtSec =
                    s.lastSeen100ns > 0.0 ? (std::max)(0.0, (t100 - s.lastSeen100ns) / 1e7) : 0.0;
                s.lastSeen100ns = t100;

                // ---- test 1: where did DWM say the window changed?
                const LONG sw = RectW(s.srcRect), sh = RectH(s.srcRect);
                const RECT band{static_cast<LONG>(sw * kBandLeft), static_cast<LONG>(sh * kBandTop),
                                static_cast<LONG>(sw * kBandRight), static_cast<LONG>(sh * kBandBottom)};
                const double bandArea = static_cast<double>(RectW(band)) * RectH(band);
                bool haveRegions = false;
                int nRects = -1;
                double coverage = 0.0;
                RECT damageBox{};  // bounding box of this capture's damage, source-rect local
                std::string rectText;
                try {
                    auto f2 = frame.try_as<wgc::IDirect3D11CaptureFrame2>();
                    if (f2) {
                        auto regions = f2.DirtyRegions();
                        haveRegions = true;
                        nRects = static_cast<int>(regions.Size());
                        for (uint32_t i = 0; i < regions.Size(); ++i) {
                            const auto g = regions.GetAt(i);
                            const RECT local{g.X - s.srcRect.left, g.Y - s.srcRect.top,
                                             g.X - s.srcRect.left + g.Width,
                                             g.Y - s.srcRect.top + g.Height};
                            RECT hit{};
                            if (i == 0) {
                                damageBox = local;
                            } else {
                                UnionRect(&damageBox, &damageBox, &local);
                            }
                            if (bandArea > 0.0 && IntersectRect(&hit, &local, &band))
                                coverage += static_cast<double>(RectW(hit)) * RectH(hit) / bandArea;
                            if (s.logDirty && i < 4)
                                rectText += Fmt(" [%ld,%ld %dx%d]", local.left, local.top, g.Width, g.Height);
                        }
                        coverage = (std::min)(1.0, coverage);
                    }
                } catch (winrt::hresult_error const&) {
                    haveRegions = false;  // no regions on this build: test 2 decides alone
                }
                // A decaying maximum OF VIDEO FRAMES, so "large" means large for
                // THIS picture: a vertical video in fullscreen damages a third of
                // the band and still clears the bar, a centred flash does not.
                // Only frames the content test called video raise it — a fade
                // damages the whole band too, and letting it raise the bar threw
                // that vertical video's real frames away for a second after every
                // controls fade (found in review).
                s.coverageMax *= std::exp(-dtSec * 0.6931 / kCoverageHalfLifeSec);

                // Clamp the source box to the captured surface. A rect that hangs
                // off the edge would make CopySubresourceRegion a no-op and the
                // ring would silently hold a stale frame.
                LONG l = (std::max)(0L, s.srcRect.left);
                LONG t = (std::max)(0L, s.srcRect.top);
                LONG r = (std::min)(static_cast<LONG>(srcDesc.Width), s.srcRect.right);
                LONG b = (std::min)(static_cast<LONG>(srcDesc.Height), s.srcRect.bottom);
                if (r > l && b > t) {
                    D3D11_BOX box = {};
                    box.left = static_cast<UINT>(l);
                    box.top = static_cast<UINT>(t);
                    box.right = static_cast<UINT>(r);
                    box.bottom = static_cast<UINT>(b);
                    box.front = 0;
                    box.back = 1;
                    // Destination offset keeps the crop aligned when the source
                    // rect was clamped on the left/top edge.
                    const UINT dstX = static_cast<UINT>(l - s.srcRect.left);
                    const UINT dstY = static_cast<UINT>(t - s.srcRect.top);
                    s.ctx->CopySubresourceRegion(s.ring[s.pending].get(), 0, dstX, dstY, 0, tex.get(),
                                                 0, &box);

                    Impl::Capture1 c;
                    c.t100 = t100;
                    c.dtSec = dtSec;
                    c.haveRegions = haveRegions;
                    c.nRects = nRects;
                    c.coverage = coverage;
                    c.damageBox = damageBox;
                    c.rectText = std::move(rectText);
                    c.bar = (std::max)(0.02, 0.5 * s.coverageMax);
                    if (!s.videoFrameTest) {
                        s.Commit(true, "test disabled", -1, c, &ingested);
                    } else if (s.filled < 2) {
                        s.Commit(true, "first frames", -1, c, &ingested);
                    } else {
                        // Damage that misses the band cannot be a video frame. Damage
                        // that is merely SMALL for this picture is dismissed without a
                        // content test only if it is not the picture's own quad (a
                        // flash, a spinner, a tooltip change shape every frame), or,
                        // until that quad is known, only early in the source period:
                        // a frame that arrived together with a controls fade is one
                        // union rect over the whole band, and a vertical video's own
                        // frames (a third of the band) fell under the bar for a second
                        // after every fade (found in review).
                        const double lastGap = (std::min)(1000000.0, s.newestTime100ns - s.prevTime100ns);
                        const bool frameDue = lastGap <= 0.0 || (t100 - s.newestTime100ns) >= 0.5 * lastGap;
                        const bool missesBand = haveRegions && (nRects == 0 || coverage < 0.02);
                        const bool smallDamage = haveRegions && s.changeTestOk && coverage < c.bar;
                        const bool quadKnown = s.videoRectCount >= 4;
                        const bool isQuad = smallDamage && s.IsVideoQuad(damageBox);
                        const bool smallReject = smallDamage && !isQuad && (quadKnown || !frameDue);
                        if (missesBand || smallReject) {
                            s.Commit(false,
                                     missesBand ? "damage misses the middle of the picture"
                                                : "small damage that is not the picture",
                                     -1, c, &ingested);
                        } else if (s.changeTestOk && s.StartTest(s.pending, s.ref)) {
                            // ---- test 2: is the middle of the picture any different?
                            s.waiting = std::move(c);
                            int changed = -1;
                            if (s.TryFinishTest(&changed)) {
                                conclude(changed);
                            } else {
                                leftPending = true;
                            }
                        } else {
                            s.Commit(true, "no content test", -1, c, &ingested);
                        }
                    }
                }
            }
        } catch (winrt::hresult_error const& e) {
            LogErr("Capture: frame processing threw %s", HrString(e.code()).c_str());
            s.needsReengage = true;
        } catch (...) {
            LogErr("Capture: frame processing threw a non-HRESULT exception");
            s.needsReengage = true;
        }

        try {
            frame.Close();
        } catch (...) {
        }
        if (sizeChanged) s.needsReengage = true;
        if (s.needsReengage || leftPending) break;
    }
    return ingested;
}

void Capture::Stop() {
    Impl& s = *impl_;
    if (s.item && s.closedToken) {
        try {
            s.item.Closed(s.closedToken);
        } catch (...) {
        }
        s.closedToken = {};
    }
    if (s.session) {
        try {
            s.session.Close();
        } catch (...) {
        }
        s.session = nullptr;
    }
    if (s.pool) {
        try {
            s.pool.Close();
        } catch (...) {
        }
        s.pool = nullptr;
    }
    s.item = nullptr;
    s.rtDevice = nullptr;
    for (int i = 0; i < Impl::kSlots; ++i) {
        s.ring[i] = nullptr;
        s.ringSrv[i] = nullptr;
        s.ringSrvRaw[i] = nullptr;
    }
    s.changeCs = nullptr;
    s.changeCb = nullptr;
    s.changeOut = nullptr;
    s.changeStaging = nullptr;
    s.changeUav = nullptr;
    s.changeTestOk = false;
    s.testPending = false;
    s.lastPictureActivity100ns = 0.0;
    s.older = s.prev = s.ref = s.newest = -1;
    s.pending = 0;
    s.filled = 0;
    s.olderTime100ns = s.prevTime100ns = s.newestTime100ns = 0.0;
    s.started = false;
    s.itemClosed->store(false, std::memory_order_release);
}

}  // namespace nsp
