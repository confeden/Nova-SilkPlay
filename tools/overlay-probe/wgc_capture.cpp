// wgc_capture.cpp — Windows.Graphics.Capture half of the S-M2 probe.
//
// The job of this file is EVIDENCE, not throughput. It answers:
//   * does GraphicsCaptureSession even start on this machine, unelevated, with no
//     package identity;
//   * do the frames contain real pixels while Chrome's video sits on an MPO plane
//     (or do we get black / a frozen frame);
//   * what do the three interesting session knobs actually DO here —
//     G4  MinUpdateInterval (IGraphicsCaptureSession5), G14 IsBorderRequired
//     (IGraphicsCaptureSession3 + the Borderless capability), and
//     IsCursorCaptureEnabled (IGraphicsCaptureSession2).
// All three live on *optional* interfaces behind impl::require<>, so calling them
// is a runtime QueryInterface that throws hresult_no_interface on an older
// contract. Every one of them is wrapped and RECORDED — a failure here is a
// first-class finding, not something to hide.
//
// Threading: free-threaded frame pool + TryGetNextFrame() polling, so there is no
// DispatcherQueue and no message-pump reentrancy anywhere in this file.

#include "wgc_capture.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <objbase.h>

// C++/WinRT projections first, interop headers after them (the interop headers
// pull in the ABI-side windows.graphics.capture.h / windows.ui.composition.h).
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace np {
namespace {

namespace wf   = winrt::Windows::Foundation;
namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wacc = winrt::Windows::Security::Authorization::AppCapabilityAccess;

using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// The statistics grid. 64x36 keeps the 16:9 aspect and is ~2.3k samples, enough
// for a stable mean/stddev and a hash that survives dither but not real motion.
constexpr uint32_t kGridW = 64;
constexpr uint32_t kGridH = 36;

// Bounded wait for the capability request. Anything longer than this on a
// consent-free API is itself the finding.
constexpr DWORD kAccessRequestTimeoutMs = 5000;

// Written into the pixel-statistics fields of a sample whose readback could not be
// mapped without stalling. main.cpp turns a non-finite double into an empty CSV
// cell and a JSON null, so an unobserved value can never masquerade as 0.0.
constexpr double kUnobserved = std::numeric_limits<double>::quiet_NaN();

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return std::string("<format error>");
    }
    if (static_cast<size_t>(n) < sizeof(buf)) {
        return std::string(buf, static_cast<size_t>(n));
    }
    std::string big(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    big.resize(static_cast<size_t>(n));
    return big;
}

const char* AccessStatusName(wacc::AppCapabilityAccessStatus s) {
    switch (s) {
        case wacc::AppCapabilityAccessStatus::DeniedBySystem:     return "DeniedBySystem";
        case wacc::AppCapabilityAccessStatus::NotDeclaredByApp:   return "NotDeclaredByApp";
        case wacc::AppCapabilityAccessStatus::DeniedByUser:       return "DeniedByUser";
        case wacc::AppCapabilityAccessStatus::UserPromptRequired: return "UserPromptRequired";
        case wacc::AppCapabilityAccessStatus::Allowed:            return "Allowed";
        default: break;
    }
    return "<unknown>";
}

const char* ApartmentTypeName(APTTYPE t) {
    switch (t) {
        case APTTYPE_CURRENT: return "CURRENT";
        case APTTYPE_STA:     return "STA";
        case APTTYPE_MTA:     return "MTA";
        case APTTYPE_NA:      return "NA";
        case APTTYPE_MAINSTA: return "MAINSTA";
        default: break;
    }
    return "<unknown>";
}

bool CurrentThreadIsSta() {
    APTTYPE apt = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qual = APTTYPEQUALIFIER_NONE;
    if (FAILED(CoGetApartmentType(&apt, &qual))) {
        return false;  // no apartment at all: a plain wait is correct.
    }
    return apt == APTTYPE_STA || apt == APTTYPE_MAINSTA;
}

// Blocking completion of an IAsyncOperation without coroutines. IAsyncOperation::get()
// would do, but it deadlocks (and debug-asserts) if the process turned out to be STA
// after a tolerated RPC_E_CHANGED_MODE, so the wait is explicit and bounded.
// The event is owned by a shared_ptr so a late completion callback after a timeout
// still has a live handle to signal.
template <typename TResult>
HRESULT AwaitOperation(wf::IAsyncOperation<TResult> const& op, DWORD timeoutMs, TResult* result) {
    if (!op) {
        return E_POINTER;
    }
    std::shared_ptr<void> evt(CreateEventW(nullptr, TRUE, FALSE, nullptr),
                              [](HANDLE h) { if (h) { CloseHandle(h); } });
    if (!evt.get()) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    try {
        op.Completed([evt](wf::IAsyncOperation<TResult> const&, wf::AsyncStatus) {
            SetEvent(evt.get());
        });
    } catch (winrt::hresult_error const& e) {
        return e.code();
    }

    HANDLE handle = evt.get();
    if (CurrentThreadIsSta()) {
        DWORD index = 0;
        const HRESULT hr = CoWaitForMultipleHandles(
            COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            timeoutMs, 1, &handle, &index);
        if (hr == RPC_S_CALLPENDING) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (FAILED(hr)) {
            return hr;
        }
    } else {
        const DWORD w = WaitForSingleObject(handle, timeoutMs);
        if (w == WAIT_TIMEOUT) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (w != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    try {
        *result = op.GetResults();
    } catch (winrt::hresult_error const& e) {
        return e.code();
    }
    return S_OK;
}

struct GridStats {
    double   mean = 0.0;
    double   stdev = 0.0;
    uint64_t hash = 0;
};

// Sampled statistics over a kGridW x kGridH lattice of the mapped BGRA rows.
//
// luma uses the Rec.709 coefficients on the RAW 8-bit values: this is deliberately
// NOT gamma-correct. The probe only needs "is this black" and "did this change",
// and staying in the encoded domain keeps the numbers comparable with what a
// screenshot tool would report.
GridStats ComputeGridStats(const uint8_t* base, uint32_t rowPitch, uint32_t w, uint32_t h, const RECT* crop = nullptr) {
    GridStats out;
    if (!base || w == 0 || h == 0) {
        return out;
    }

    uint32_t x0 = 0, y0 = 0;
    uint32_t cw = w, ch = h;
    if (crop) {
        x0 = static_cast<uint32_t>((std::max)(0L, crop->left));
        y0 = static_cast<uint32_t>((std::max)(0L, crop->top));
        uint32_t x1 = (std::min)(w, static_cast<uint32_t>((std::max)(0L, crop->right)));
        uint32_t y1 = (std::min)(h, static_cast<uint32_t>((std::max)(0L, crop->bottom)));
        if (x1 <= x0 || y1 <= y0) {
            return out;
        }
        cw = x1 - x0;
        ch = y1 - y0;
    }

    double sum = 0.0;
    double sumSq = 0.0;
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a 64 offset basis
    constexpr uint64_t kFnvPrime = 1099511628211ULL;

    for (uint32_t gy = 0; gy < kGridH; ++gy) {
        // Sample cell centres so the grid never lands on the very first/last row.
        uint32_t y = y0 + static_cast<uint32_t>((static_cast<uint64_t>(gy) * 2 + 1) * ch / (2ULL * kGridH));
        if (y >= y0 + ch) {
            y = y0 + ch - 1;
        }
        const uint8_t* row = base + static_cast<size_t>(y) * rowPitch;
        for (uint32_t gx = 0; gx < kGridW; ++gx) {
            uint32_t x = x0 + static_cast<uint32_t>((static_cast<uint64_t>(gx) * 2 + 1) * cw / (2ULL * kGridW));
            if (x >= x0 + cw) {
                x = x0 + cw - 1;
            }
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            const double b = static_cast<double>(px[0]);
            const double g = static_cast<double>(px[1]);
            const double r = static_cast<double>(px[2]);

            const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            sum += luma;
            sumSq += luma * luma;

            // Quantise to 4 bits per channel before hashing so 1-2 LSB of dither
            // or video-decoder noise does not defeat duplicate detection.
            const uint8_t q0 = static_cast<uint8_t>((px[2] & 0xF0) | (px[1] >> 4));  // RRRRGGGG
            const uint8_t q1 = static_cast<uint8_t>(px[0] & 0xF0);                   // BBBB0000
            hash = (hash ^ q0) * kFnvPrime;
            hash = (hash ^ q1) * kFnvPrime;
        }
    }

    const double n = static_cast<double>(kGridW) * static_cast<double>(kGridH);
    out.mean = sum / n;
    const double var = (sumSq / n) - (out.mean * out.mean);
    out.stdev = var > 0.0 ? std::sqrt(var) : 0.0;
    out.hash = hash;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

struct WgcCapture::Impl {
    // Apartment / support facts. ownsApartment is set ONLY when our own
    // CoInitializeEx returned S_OK, i.e. this call established the apartment and
    // therefore must undo it.
    bool    ownsApartment = false;
    HRESULT coInitHr = E_FAIL;

    // Our own D3D11 device (deliberately not the presentation device).
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL                   featureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
    LUID                                adapterLuid{};

    wgdx::Direct3D11::IDirect3DDevice rtDevice{nullptr};
    wgc::GraphicsCaptureItem          item{nullptr};
    wgc::Direct3D11CaptureFramePool   pool{nullptr};
    wgc::GraphicsCaptureSession       session{nullptr};
    winrt::event_token                closedToken{};

    // The item.Closed flag is owned INDEPENDENTLY of Impl. item.Closed(token)
    // unregisters the delegate but does not wait for an invocation that is already
    // running on some other thread, so the delegate must not touch `this` — a
    // window closing during ~WgcCapture would be a use-after-free write. Same
    // idiom as the event handle in AwaitOperation().
    std::shared_ptr<std::atomic<bool>> itemClosed =
        std::make_shared<std::atomic<bool>>(false);
    bool started = false;
    bool fatal = false;            // an hresult_error escaped a poll; stop capturing.
    bool closedLogged = false;
    bool formatWarned = false;
    RECT cropRect = {0, 0, 0, 0};
    bool useCropRect = false;

    // Readback cache. See the comment in Poll(): this staging path is a
    // PROBE-ONLY cost and is not how the product will ingest frames.
    //
    // TWO staging textures, used alternately: the CPU maps the frame whose copy
    // was issued earlier while the GPU is still retiring the current one. One
    // frame of pipelining is what lets the map use D3D11_MAP_FLAG_DO_NOT_WAIT and
    // keeps Poll() non-blocking, as wgc_capture.h promises.
    winrt::com_ptr<ID3D11Texture2D> staging[2];
    UINT        stagingW = 0;
    UINT        stagingH = 0;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;
    int         stagingIndex = 0;      // slot the NEXT CopyResource writes into
    uint32_t    stagingAllocs = 0;

    // One D3D11_QUERY_EVENT per staging slot, ended right after that slot's copy.
    // MEASURED on this machine (RTX 5060 Ti / build 26100, obj\maptest): a bare
    // Map(D3D11_MAP_FLAG_DO_NOT_WAIT) on a staging texture returns
    // DXGI_ERROR_WAS_STILL_DRAWING FOREVER — still failing 16 ms after
    // CopyResource + Flush — because nothing ever polls the driver to retire the
    // resource. Poll the event query first and the same map succeeds in ~0 ms.
    winrt::com_ptr<ID3D11Query> copyDone[2];

    // The frame whose CopyResource is in flight. Its timing fields are already
    // final; only the pixel statistics are still missing.
    bool          hasPending = false;
    int           pendingIndex = 0;
    CaptureSample pending{};
    uint32_t      pendingStatW = 0;
    uint32_t      pendingStatH = 0;
    double        pendingCopyMs = 0.0;   // CPU cost already charged to that sample

    uint64_t framesSeen = 0;         // frames dequeued whose copy was issued
    uint64_t statsUnavailable = 0;   // DXGI_ERROR_WAS_STILL_DRAWING at map time
    // Frames dequeued from the pool that produce NO CaptureSample at all (surface
    // access, staging allocation or an exception). Distinct from statsUnavailable,
    // where the sample IS emitted, just without pixel statistics.
    uint64_t framesDropped = 0;

    uint64_t prevHash = 0;
    bool     hasPrevHash = false;

    std::string report;

    void Add(std::string line) {
        report += line;
        report += '\n';
    }

    // Settles the frame whose copy is in flight and appends its sample to `out`.
    // Returns true if a sample was appended.
    //
    // The map is DO_NOT_WAIT: if the copy has not retired, the pixel statistics
    // for that frame are LOST rather than waited for, and the loss is recorded
    // both in statsUnavailable and in the sample (statsValid == false, luma NaN).
    // The arrival fields — qpc, systemRelativeTime100ns, width, height — were
    // filled when the frame was dequeued and are emitted either way, so the
    // frame-rate and frame-arrival measurement is unaffected by a skipped map.
    bool ResolvePending(std::vector<CaptureSample>* out) {
        if (!hasPending || !out) {
            return false;
        }
        hasPending = false;
        CaptureSample sample = pending;

        const int64_t t0 = QpcNow();
        // Readiness gate. S_FALSE = the copy has not retired; give up on this
        // frame's pixels rather than wait for the GPU. DONOTFLUSH is safe because
        // Poll() explicitly flushed the copy that this query was ended after.
        HRESULT hr = context->GetData(copyDone[pendingIndex].get(), nullptr, 0,
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (hr == S_OK) {
            hr = context->Map(staging[pendingIndex].get(), 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        } else if (SUCCEEDED(hr)) {
            hr = DXGI_ERROR_WAS_STILL_DRAWING;   // S_FALSE: not retired yet
        }
        if (SUCCEEDED(hr)) {
            const GridStats stats = ComputeGridStats(
                static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                pendingStatW, pendingStatH, useCropRect ? &cropRect : nullptr);
            context->Unmap(staging[pendingIndex].get(), 0);
            sample.meanLuma = stats.mean;
            sample.stdLuma = stats.stdev;
            sample.gridHash = stats.hash;
            sample.isBlack = (stats.mean < 1.0) && (stats.stdev < 1.0);
            // Compared against the last frame we actually SAW: after a skipped
            // map the neighbour is the previous observed frame, not the previous
            // delivered one.
            sample.isDuplicate = hasPrevHash && (stats.hash == prevHash);
            sample.statsValid = true;
            prevHash = stats.hash;
            hasPrevHash = true;
        } else {
            sample.statsValid = false;
            sample.meanLuma = kUnobserved;
            sample.stdLuma = kUnobserved;
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ++statsUnavailable;
            } else {
                LogErr("WGC: staging readback (GetData/Map, DO_NOT_WAIT) failed: %s"
                       " - capture stopped.", HrString(hr).c_str());
                fatal = true;
            }
        }
        // Only the CPU time this sample actually cost: the copy plus this map
        // attempt. The staging allocation is deliberately NOT in here.
        sample.readbackMs = pendingCopyMs + QpcToMs(QpcNow() - t0);
        out->push_back(sample);
        return true;
    }
};

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}

WgcCapture::~WgcCapture() {
    Stop();
}

bool WgcCapture::Start(HWND target, const Options& opt, std::string* err) {
    Impl& s = *impl_;
    s.report.clear();

    auto fail = [&](const std::string& message) -> bool {
        if (err) {
            *err = message;
        }
        s.Add(Fmt("FATAL: %s", message.c_str()));
        return false;
    };

    // ---------------------------------------------------------------- apartment
    // CoInitializeEx directly, NOT winrt::init_apartment(): init_apartment throws
    // only on failure and discards S_FALSE, so it cannot tell "this call created
    // the MTA" from "it was already there". On the real code path main() has
    // already called CoInitializeEx(MTA) on this very thread, so the honest answer
    // is S_FALSE every time. Record the HRESULT verbatim.
    s.coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (s.coInitHr == S_OK) {
        s.ownsApartment = true;
        s.Add(Fmt("apartment.init: CoInitializeEx(MTA) -> %s - this call initialised the "
                  "apartment (balanced by CoUninitialize in Stop())",
                  HrString(s.coInitHr).c_str()));
    } else if (s.coInitHr == S_FALSE) {
        // Already initialised on this thread in the SAME mode. S_FALSE also bumps
        // the thread's COM reference count, but the count is deliberately left
        // outstanding: main() owns this apartment for the whole process, and an
        // extra count is harmless where an unbalanced CoUninitialize would not be.
        s.Add(Fmt("apartment.init: CoInitializeEx(MTA) -> %s - apartment already "
                  "initialised on this thread, same mode (main() owns it)",
                  HrString(s.coInitHr).c_str()));
    } else if (s.coInitHr == RPC_E_CHANGED_MODE) {
        // Someone else already picked a DIFFERENT apartment for this thread.
        // Tolerated: free-threaded pool + polling works from any apartment.
        s.Add(Fmt("apartment.init: CoInitializeEx(MTA) -> %s - apartment already "
                  "initialised in a different mode - tolerated",
                  HrString(s.coInitHr).c_str()));
    } else {
        return fail(Fmt("CoInitializeEx(MTA) failed: %s", HrString(s.coInitHr).c_str()));
    }
    {
        APTTYPE apt = APTTYPE_CURRENT;
        APTTYPEQUALIFIER qual = APTTYPEQUALIFIER_NONE;
        const HRESULT hr = CoGetApartmentType(&apt, &qual);
        if (SUCCEEDED(hr)) {
            s.Add(Fmt("apartment.actual: %s (qualifier %d)", ApartmentTypeName(apt), static_cast<int>(qual)));
        } else {
            s.Add(Fmt("apartment.actual: CoGetApartmentType failed %s", HrString(hr).c_str()));
        }
    }

    // ---------------------------------------------------------------- support
    bool supported = false;
    try {
        supported = wgc::GraphicsCaptureSession::IsSupported();
        s.Add(Fmt("GraphicsCaptureSession.IsSupported: %s", supported ? "true" : "false"));
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("GraphicsCaptureSession.IsSupported: THREW %s", HrString(e.code()).c_str()));
    }
    if (!supported) {
        return fail("GraphicsCaptureSession::IsSupported() returned false");
    }

    // ---------------------------------------------------------------- D3D11
    {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,   // required by the WinRT D3D interop
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            s.device.put(), &s.featureLevel, s.context.put());
        if (FAILED(hr)) {
            return fail(Fmt("D3D11CreateDevice failed: %s", HrString(hr).c_str()));
        }
        s.Add(Fmt("d3d11.featureLevel: 0x%04X", static_cast<unsigned>(s.featureLevel)));

        auto dxgiDevice = s.device.try_as<IDXGIDevice>();
        if (!dxgiDevice) {
            return fail("ID3D11Device does not expose IDXGIDevice");
        }

        winrt::com_ptr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(adapter.put());
        if (SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC desc{};
            hr = adapter->GetDesc(&desc);
            if (SUCCEEDED(hr)) {
                s.adapterLuid = desc.AdapterLuid;
                s.Add(Fmt("d3d11.adapterLuid: %08X:%08X  vendor=0x%04X device=0x%04X",
                          static_cast<unsigned>(s.adapterLuid.HighPart),
                          static_cast<unsigned>(s.adapterLuid.LowPart),
                          static_cast<unsigned>(desc.VendorId),
                          static_cast<unsigned>(desc.DeviceId)));
            } else {
                s.Add(Fmt("d3d11.adapterLuid: IDXGIAdapter::GetDesc failed %s", HrString(hr).c_str()));
            }
        } else {
            s.Add(Fmt("d3d11.adapterLuid: IDXGIDevice::GetAdapter failed %s", HrString(hr).c_str()));
        }

        // Readiness gate for the pipelined readback (see Impl::copyDone).
        {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_EVENT;
            for (int i = 0; i < 2; ++i) {
                const HRESULT qhr = s.device->CreateQuery(&qd, s.copyDone[i].put());
                if (FAILED(qhr) || !s.copyDone[i]) {
                    return fail(Fmt("CreateQuery(D3D11_QUERY_EVENT) failed: %s",
                                    HrString(qhr).c_str()));
                }
            }
            s.Add("d3d11.copyDoneQueries: 2 x D3D11_QUERY_EVENT created "
                  "(readback readiness gate)");
        }

        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
        if (FAILED(hr) || !inspectable) {
            return fail(Fmt("CreateDirect3D11DeviceFromDXGIDevice failed: %s", HrString(hr).c_str()));
        }
        try {
            s.rtDevice = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();
        } catch (winrt::hresult_error const& e) {
            return fail(Fmt("IInspectable -> IDirect3DDevice failed: %s", HrString(e.code()).c_str()));
        }
        s.Add("interop.CreateDirect3D11DeviceFromDXGIDevice: ok");
    }

    // ---------------------------------------------------------------- item
    {
        if (!target || !IsWindow(target)) {
            return fail("target HWND is not a window");
        }
        winrt::hresult_error factoryError{};
        auto interop = winrt::try_get_activation_factory<wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>(factoryError);
        if (!interop) {
            return fail(Fmt("IGraphicsCaptureItemInterop activation failed: %s",
                            HrString(factoryError.code()).c_str()));
        }
        HRESULT hr = S_OK;
        if (opt.wgcSource == Options::WgcSource::Monitor) {
            HMONITOR hmon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
            hr = interop->CreateForMonitor(
                hmon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(s.item));
            if (FAILED(hr) || !s.item) {
                return fail(Fmt("IGraphicsCaptureItemInterop::CreateForMonitor failed: %s", HrString(hr).c_str()));
            }
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(hmon, &mi);
            s.Add(Fmt("source: Monitor (HMONITOR %p, rcMonitor %ld,%ld,%ld,%ld)", 
                      hmon, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom));

            RECT rc;
            GetClientRect(target, &rc);
            POINT pt = { 0, 0 };
            ClientToScreen(target, &pt);
            
            s.cropRect.left = pt.x - mi.rcMonitor.left;
            s.cropRect.top = pt.y - mi.rcMonitor.top;
            s.cropRect.right = s.cropRect.left + (rc.right - rc.left);
            s.cropRect.bottom = s.cropRect.top + (rc.bottom - rc.top);
            s.useCropRect = true;

            s.Add(Fmt("crop: %ld,%ld,%ld,%ld", s.cropRect.left, s.cropRect.top, s.cropRect.right, s.cropRect.bottom));
        } else {
            hr = interop->CreateForWindow(
                target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(s.item));
            if (FAILED(hr) || !s.item) {
                return fail(Fmt("IGraphicsCaptureItemInterop::CreateForWindow failed: %s", HrString(hr).c_str()));
            }
            s.useCropRect = false;
        }
        try {
            const auto size = s.item.Size();
            s.Add(Fmt("item.Size: %dx%d", size.Width, size.Height));
            std::wstring name(s.item.DisplayName().c_str());
            s.Add(Fmt("item.DisplayName: %s", Narrow(name).c_str()));
        } catch (winrt::hresult_error const& e) {
            s.Add(Fmt("item.Size/DisplayName: THREW %s", HrString(e.code()).c_str()));
        }
    }

    // ---------------------------------------------------------------- frame pool
    try {
        const auto size = s.item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            return fail(Fmt("item size is degenerate (%dx%d)", size.Width, size.Height));
        }
        // 2 buffers: the probe drains every tick, and a deeper pool would only
        // hide latency we are here to measure.
        s.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s.rtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        s.Add("framePool.CreateFreeThreaded: ok (B8G8R8A8UIntNormalized, 2 buffers)");
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("Direct3D11CaptureFramePool::CreateFreeThreaded failed: %s", HrString(e.code()).c_str()));
    }

    try {
        s.session = s.pool.CreateCaptureSession(s.item);
        s.Add("framePool.CreateCaptureSession: ok");
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("Direct3D11CaptureFramePool::CreateCaptureSession failed: %s", HrString(e.code()).c_str()));
    }

    // ------------------------------------------------- G14: borderless capability
    // Order matters: the capability must be requested BEFORE IsBorderRequired is
    // touched. On a non-packaged install this is expected to be denied — the
    // policy allowlist for the "graphicsCaptureWithoutBorder" capability takes
    // Package Family Names only. Recording the exact status IS the deliverable.
    if (opt.wgcTryBorderless) {
        auto status = wacc::AppCapabilityAccessStatus::DeniedBySystem;
        HRESULT hr = E_FAIL;
        try {
            auto op = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless);
            hr = AwaitOperation(op, kAccessRequestTimeoutMs, &status);
        } catch (winrt::hresult_error const& e) {
            hr = e.code();
        }
        if (SUCCEEDED(hr)) {
            s.Add(Fmt("GraphicsCaptureAccess.RequestAccessAsync(Borderless): %s", AccessStatusName(status)));
        } else {
            s.Add(Fmt("GraphicsCaptureAccess.RequestAccessAsync(Borderless): FAILED %s", HrString(hr).c_str()));
        }

        // IGraphicsCaptureSession3. Two distinct failure shapes are interesting:
        // the setter throwing (older contract / access denied), and the setter
        // succeeding while the yellow border stays on screen (G14's "silently
        // ignored"). The read-back below cannot distinguish the second case on its
        // own — the captured pixels are the real evidence.
        try {
            s.session.IsBorderRequired(false);
            s.Add("session.IsBorderRequired(false): setter returned S_OK");
        } catch (winrt::hresult_error const& e) {
            s.Add(Fmt("session.IsBorderRequired(false): THREW %s", HrString(e.code()).c_str()));
        }
        try {
            const bool readBack = s.session.IsBorderRequired();
            s.Add(Fmt("session.IsBorderRequired [readback]: %s", readBack ? "true" : "false"));
        } catch (winrt::hresult_error const& e) {
            s.Add(Fmt("session.IsBorderRequired [readback]: THREW %s", HrString(e.code()).c_str()));
        }
    } else {
        s.Add("session.IsBorderRequired: skipped (opt.wgcTryBorderless == false)");
    }

    // ------------------------------------------------- IGraphicsCaptureSession2
    try {
        s.session.IsCursorCaptureEnabled(opt.wgcCursor);
        s.Add(Fmt("session.IsCursorCaptureEnabled(%s): setter returned S_OK", opt.wgcCursor ? "true" : "false"));
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("session.IsCursorCaptureEnabled: THREW %s (IGraphicsCaptureSession2 unavailable?)",
                  HrString(e.code()).c_str()));
    }

    // ------------------------------------------------- G4: MinUpdateInterval
    // Defaults to 0; on build 26100 anything below 1000 us caps WGC at ~50 fps,
    // so it must be set explicitly. IGraphicsCaptureSession5 is a recent contract:
    // an exception here means this machine cannot exceed that cap at all, which is
    // a first-class finding for the whole browser path.
    {
        const auto requested = std::chrono::duration_cast<wf::TimeSpan>(
            std::chrono::duration<double, std::milli>(opt.wgcMinUpdateIntervalMs));
        try {
            s.session.MinUpdateInterval(requested);
            s.Add(Fmt("session.MinUpdateInterval(%.3f ms = %lld x100ns): setter returned S_OK",
                      opt.wgcMinUpdateIntervalMs, static_cast<long long>(requested.count())));
        } catch (winrt::hresult_error const& e) {
            s.Add(Fmt("session.MinUpdateInterval(%.3f ms): THREW %s "
                      "(IGraphicsCaptureSession5 unavailable -> G4 cap cannot be lifted)",
                      opt.wgcMinUpdateIntervalMs, HrString(e.code()).c_str()));
        }
        try {
            const auto readBack = s.session.MinUpdateInterval();
            s.Add(Fmt("session.MinUpdateInterval [readback]: %lld x100ns (%.3f ms)",
                      static_cast<long long>(readBack.count()),
                      static_cast<double>(readBack.count()) / 10000.0));
        } catch (winrt::hresult_error const& e) {
            s.Add(Fmt("session.MinUpdateInterval [readback]: THREW %s", HrString(e.code()).c_str()));
        }
    }

    // ------------------------------------------------- item Closed
    try {
        // A FRESH flag per session, captured BY VALUE. The delegate fires on an
        // arbitrary thread and item.Closed(token) does not wait for an invocation
        // already in flight, so the lambda must keep the flag alive on its own
        // rather than reach back into Impl (which Stop()/~WgcCapture may be
        // destroying at that instant).
        s.itemClosed = std::make_shared<std::atomic<bool>>(false);
        auto flag = s.itemClosed;
        s.closedToken = s.item.Closed([flag](wgc::GraphicsCaptureItem const&, wf::IInspectable const&) {
            flag->store(true, std::memory_order_release);
        });
        s.Add("item.Closed: handler registered");
    } catch (winrt::hresult_error const& e) {
        s.Add(Fmt("item.Closed: registration THREW %s", HrString(e.code()).c_str()));
    }

    // ------------------------------------------------- go
    try {
        s.session.StartCapture();
        s.Add("session.StartCapture: ok");
    } catch (winrt::hresult_error const& e) {
        return fail(Fmt("GraphicsCaptureSession::StartCapture failed: %s", HrString(e.code()).c_str()));
    }

    s.started = true;
    s.fatal = false;
    s.hasPrevHash = false;
    s.hasPending = false;
    s.stagingIndex = 0;
    s.framesSeen = 0;
    s.statsUnavailable = 0;
    s.framesDropped = 0;
    return true;
}

size_t WgcCapture::Poll(uint32_t phaseIndex, std::vector<CaptureSample>* out) {
    Impl& s = *impl_;
    if (!s.started || s.fatal || !out) {
        return 0;
    }
    if (s.itemClosed->load(std::memory_order_acquire) && !s.closedLogged) {
        s.closedLogged = true;
        LogErr("WGC: capture item reported Closed (target window gone).");
    }

    size_t appended = 0;

    // Settle the frame left in flight by the previous call FIRST. Its copy has had
    // a whole compositor tick (6.06 ms at 165 Hz) to retire, so the DO_NOT_WAIT map
    // below essentially always succeeds; doing it here rather than at the end of
    // the last call is what buys the GPU that time without the CPU ever waiting.
    // Cost: a sample is appended one tick after the frame arrived, so a frame that
    // lands in the final tick of a phase is appended during the next phase. Its
    // own phaseIndex/qpc still say where it came from.
    if (s.ResolvePending(out)) {
        ++appended;
    }
    if (s.fatal) {
        return appended;
    }

    for (;;) {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        try {
            frame = s.pool.TryGetNextFrame();
        } catch (winrt::hresult_error const& e) {
            LogErr("WGC: TryGetNextFrame failed: %s - capture stopped.", HrString(e.code()).c_str());
            s.fatal = true;
            return appended;
        }
        if (!frame) {
            break;
        }
        ++s.framesSeen;   // every frame the pool handed us, however it ends up

        CaptureSample sample;
        sample.qpc = QpcNow();
        sample.phaseIndex = phaseIndex;

        bool ok = false;
        try {
            sample.systemRelativeTime100ns = frame.SystemRelativeTime().count();
            const auto content = frame.ContentSize();
            sample.width  = content.Width  > 0 ? static_cast<uint32_t>(content.Width)  : 0u;
            sample.height = content.Height > 0 ? static_cast<uint32_t>(content.Height) : 0u;

            winrt::com_ptr<ID3D11Texture2D> tex;
            {
                auto access = frame.Surface().as<DxgiInterfaceAccess>();
                const HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
                if (FAILED(hr) || !tex) {
                    LogErr("WGC: IDirect3DDxgiInterfaceAccess::GetInterface(ID3D11Texture2D) failed: %s",
                           HrString(hr).c_str());
                    s.fatal = true;
                    ++s.framesDropped;
                    frame.Close();
                    return appended;
                }
            }

            // ---- probe-only readback -------------------------------------------
            // The whole frame is copied to a CPU-readable STAGING texture and mapped.
            // CopySubresourceRegion cannot scale and any real downsample is shader
            // work, so a full-surface copy + a strided CPU walk is the only
            // shader-free way to get statistics. This is a MEASUREMENT cost: the
            // product will never read frames back to the CPU — it keeps them on the
            // GPU and feeds them straight to the interpolator. readbackMs exists so
            // this cost can be subtracted from anything else the probe reports.
            //
            // The copy is only ISSUED here; the matching map happens one frame (or
            // one Poll) later in ResolvePending(). A 2560x1440 BGRA frame is 14.7 MB
            // and this runs inside a 6.06 ms compositor tick, so a blocking map
            // would perturb the very present cadence the probe measures.
            D3D11_TEXTURE2D_DESC srcDesc{};
            tex->GetDesc(&srcDesc);

            if (!s.staging[0] || !s.staging[1] || s.stagingW != srcDesc.Width ||
                s.stagingH != srcDesc.Height || s.stagingFormat != srcDesc.Format) {
                // The in-flight frame's pixels live in a texture that is about to be
                // released, so settle it while it is still alive.
                if (s.ResolvePending(out)) {
                    ++appended;
                }
                s.staging[0] = nullptr;
                s.staging[1] = nullptr;
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width = srcDesc.Width;
                sd.Height = srcDesc.Height;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Format = srcDesc.Format;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;

                // Timed on its own: a multi-megabyte CreateTexture2D is a ONE-OFF
                // and must never land inside a sample's readbackMs, where main.cpp's
                // p99 (≈ the maximum over a ~100-frame phase) would report it as the
                // steady-state readback cost.
                const int64_t tAlloc = QpcNow();
                HRESULT hr = S_OK;
                for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
                    hr = s.device->CreateTexture2D(&sd, nullptr, s.staging[i].put());
                }
                const double allocMs = QpcToMs(QpcNow() - tAlloc);
                if (FAILED(hr) || !s.staging[0] || !s.staging[1]) {
                    LogErr("WGC: staging CreateTexture2D(%ux%u fmt=%d) failed: %s",
                           sd.Width, sd.Height, static_cast<int>(sd.Format), HrString(hr).c_str());
                    s.fatal = true;
                    ++s.framesDropped;
                    s.staging[0] = nullptr;
                    s.staging[1] = nullptr;
                    tex = nullptr;
                    frame.Close();
                    return appended;
                }
                s.stagingW = sd.Width;
                s.stagingH = sd.Height;
                s.stagingFormat = sd.Format;
                s.stagingIndex = 0;
                ++s.stagingAllocs;

                // Recorded in the report string AND on stderr: the caller snapshots
                // StartupReport() right after Start(), before the first Poll(), so
                // the report line alone would never reach the JSON.
                const std::string allocLine = Fmt(
                    "staging.alloc[%u]: 2 x %ux%u fmt=%d (~%.2f MB each) in %.3f ms "
                    "(one-off, excluded from readbackMs)",
                    s.stagingAllocs, sd.Width, sd.Height, static_cast<int>(sd.Format),
                    static_cast<double>(sd.Width) * sd.Height * 4.0 / (1024.0 * 1024.0),
                    allocMs);
                s.Add(allocLine);
                LogErr("WGC: %s", allocLine.c_str());

                if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM && !s.formatWarned) {
                    s.formatWarned = true;
                    LogErr("WGC: frame format is %d, not DXGI_FORMAT_B8G8R8A8_UNORM (87); "
                           "statistics assume 4-byte BGRA.", static_cast<int>(sd.Format));
                }
            }

            // Statistics cover the CONTENT region only. The pool's surfaces stay at
            // the item size for the whole run (the probe never calls Recreate),
            // so a shrunk window leaves stale pixels outside ContentSize.
            uint32_t statW = sample.width  ? sample.width  : s.stagingW;
            uint32_t statH = sample.height ? sample.height : s.stagingH;
            if (statW > s.stagingW) { statW = s.stagingW; }
            if (statH > s.stagingH) { statH = s.stagingH; }

            // The timer starts HERE — after the staging cache check — so the first
            // frame of a run is charged the same readback cost as every other frame.
            const int64_t t0 = QpcNow();
            s.context->CopyResource(s.staging[s.stagingIndex].get(), tex.get());
            // End the slot's event query immediately after the copy, then submit.
            // Without the flush the copy can sit in the command buffer (nothing
            // else on this device ever presents) and the query would never signal;
            // Flush() only hands the commands to the GPU, it does not wait.
            s.context->End(s.copyDone[s.stagingIndex].get());
            s.context->Flush();
            const double copyMs = QpcToMs(QpcNow() - t0);
            tex = nullptr;  // release the pool's surface before Close().

            // Two staging textures only, so the previous frame's slot is needed
            // again on the next iteration: settle it now, while THIS copy is in
            // flight. In the common 0-or-1-frame-per-tick case there is nothing
            // pending here (Poll() settled it on entry) and this is a no-op.
            if (s.ResolvePending(out)) {
                ++appended;
            }

            s.pending = sample;
            s.pendingIndex = s.stagingIndex;
            s.pendingStatW = statW;
            s.pendingStatH = statH;
            s.pendingCopyMs = copyMs;
            s.hasPending = true;
            s.stagingIndex ^= 1;
            ok = true;
        } catch (winrt::hresult_error const& e) {
            LogErr("WGC: frame processing threw %s - capture stopped.", HrString(e.code()).c_str());
            s.fatal = true;
            ++s.framesDropped;
        } catch (...) {
            LogErr("WGC: frame processing threw a non-HRESULT exception - capture stopped.");
            s.fatal = true;
            ++s.framesDropped;
        }

        try {
            frame.Close();
        } catch (winrt::hresult_error const& e) {
            LogErr("WGC: Direct3D11CaptureFrame::Close failed: %s", HrString(e.code()).c_str());
        }

        // `sample` is NOT appended here: it is now the pending frame and will be
        // appended by ResolvePending() once its copy has retired.
        if (!ok || s.fatal) {
            return appended;
        }
    }

    return appended;
}

void WgcCapture::Stop() {
    Impl& s = *impl_;

    // Never silently: how many delivered frames ended up without pixel statistics
    // is part of the evidence. Reported on stderr as well as in the report string,
    // because the caller snapshots StartupReport() before the first Poll().
    if (s.framesSeen > 0 || s.statsUnavailable > 0) {
        const std::string line = Fmt(
            "capture.stats: %llu frame(s) delivered, %llu without pixel statistics "
            "(%.2f%%, Map returned DXGI_ERROR_WAS_STILL_DRAWING)",
            static_cast<unsigned long long>(s.framesSeen),
            static_cast<unsigned long long>(s.statsUnavailable),
            s.framesSeen ? 100.0 * static_cast<double>(s.statsUnavailable) /
                               static_cast<double>(s.framesSeen)
                         : 0.0);
        s.Add(line);
        LogErr("WGC: %s", line.c_str());
    }
    if (s.hasPending) {
        // The last copy never got a Poll() to settle it. Dropping one frame is
        // honest; guessing its statistics would not be.
        s.hasPending = false;
        ++s.framesDropped;
        LogErr("WGC: 1 frame was still in flight at Stop() and is not in the sample set.");
    }

    if (s.item && s.closedToken) {
        try {
            s.item.Closed(s.closedToken);
        } catch (...) {
            // The revoke path is noexcept in the projection; ignore anything the
            // ABI still manages to raise while tearing down.
        }
        s.closedToken = {};
    }
    if (s.session) {
        try {
            s.session.Close();
        } catch (winrt::hresult_error const& e) {
            LogErr("WGC: GraphicsCaptureSession::Close failed: %s", HrString(e.code()).c_str());
        }
        s.session = nullptr;
    }
    if (s.pool) {
        try {
            s.pool.Close();
        } catch (winrt::hresult_error const& e) {
            LogErr("WGC: Direct3D11CaptureFramePool::Close failed: %s", HrString(e.code()).c_str());
        }
        s.pool = nullptr;
    }
    s.item = nullptr;
    s.rtDevice = nullptr;

    s.staging[0] = nullptr;
    s.staging[1] = nullptr;
    s.copyDone[0] = nullptr;
    s.copyDone[1] = nullptr;
    s.stagingW = 0;
    s.stagingH = 0;
    s.stagingFormat = DXGI_FORMAT_UNKNOWN;
    s.stagingIndex = 0;

    if (s.context) {
        s.context->ClearState();
        s.context->Flush();
        s.context = nullptr;
    }
    s.device = nullptr;

    s.started = false;
    s.hasPrevHash = false;
    s.framesSeen = 0;
    s.statsUnavailable = 0;
    s.framesDropped = 0;
    // A delegate that is still in flight owns this flag through its own
    // shared_ptr, so clearing it here can never race with the handler.
    s.itemClosed->store(false, std::memory_order_release);
    s.closedLogged = false;

    // Only undo what this object did: ownsApartment is set solely on the S_OK
    // return, i.e. when OUR CoInitializeEx established the MTA. On the real code
    // path main() got there first (S_FALSE) and owns the apartment for the whole
    // process, so nothing happens here. Drop C++/WinRT's cached activation
    // factories first — they live in this apartment and would dangle after it goes.
    if (s.ownsApartment) {
        s.ownsApartment = false;
        winrt::clear_factory_cache();
        CoUninitialize();
    }
    s.coInitHr = E_FAIL;
}

size_t WgcCapture::FramesWithoutPixelStats() const {
    // Frames that were dequeued but produced NO CaptureSample. Frames that produced
    // a sample whose pixel statistics had to be skipped (statsValid == false) are
    // NOT counted here: they ARE in the sample set, and Stop() reports their count
    // separately as capture.stats.
    //
    // The frame whose copy is still in flight counts too, so that the ledger closes
    // for a caller that reads this BEFORE Stop() (as main.cpp does): frames
    // delivered == samples appended + FramesWithoutPixelStats().
    return static_cast<size_t>(impl_->framesDropped) + (impl_->hasPending ? 1u : 0u);
}

const std::string& WgcCapture::StartupReport() const {
    return impl_->report;
}

}  // namespace np
