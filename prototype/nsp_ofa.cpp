// nsp_ofa.cpp — NVOFA over D3D11.

#include "nsp_ofa.h"

#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

// The vendored SDK headers (MIT, see third_party/nvofapi/README.md).
#include "../third_party/nvofapi/nvOpticalFlowCommon.h"
#include "../third_party/nvofapi/nvOpticalFlowD3D11.h"

using Microsoft::WRL::ComPtr;

namespace nsp {
namespace {

using PFN_CreateInstanceD3D11 = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
using PFN_GetMaxApiVersion = NV_OF_STATUS(NVOFAPI*)(uint32_t*);

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return "<format error>";
    return std::string(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                 : sizeof(buf) - 1);
}

const char* StatusName(NV_OF_STATUS s) {
    switch (s) {
        case NV_OF_SUCCESS: return "SUCCESS";
        case NV_OF_ERR_OF_NOT_AVAILABLE: return "OF_NOT_AVAILABLE";
        case NV_OF_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
        case NV_OF_ERR_DEVICE_DOES_NOT_EXIST: return "DEVICE_DOES_NOT_EXIST";
        case NV_OF_ERR_INVALID_PTR: return "INVALID_PTR";
        case NV_OF_ERR_INVALID_PARAM: return "INVALID_PARAM";
        case NV_OF_ERR_INVALID_CALL: return "INVALID_CALL";
        case NV_OF_ERR_INVALID_VERSION: return "INVALID_VERSION";
        case NV_OF_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case NV_OF_ERR_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case NV_OF_ERR_UNSUPPORTED_FEATURE: return "UNSUPPORTED_FEATURE";
        case NV_OF_ERR_GENERIC: return "GENERIC";
        default: return "<unknown>";
    }
}

const char* DxgiFormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_NV12: return "NV12";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R16G16_SINT: return "R16G16_SINT";
        case DXGI_FORMAT_R16_SINT: return "R16_SINT";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R8_UINT: return "R8_UINT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        default: return "<other>";
    }
}

}  // namespace

struct OfaFlow::Impl {
    ID3D11Device* device = nullptr;      // borrowed
    ID3D11DeviceContext* ctx = nullptr;  // borrowed
    HMODULE dll = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST fn{};
    NvOFHandle hOf = nullptr;

    UINT w = 0, h = 0, grid = 4;      // the picture
    UINT alignW = 0, alignH = 0;      // what the hardware is actually given
    UINT gridW = 0, gridH = 0;        // vectors across the ALIGNED surface

    ComPtr<ID3D11Texture2D> inPrev, inNext;
    ComPtr<ID3D11UnorderedAccessView> inPrevUav, inNextUav;
    ComPtr<ID3D11Texture2D> flowFwd, flowBwd, costFwd, costBwd;
    ComPtr<ID3D11Texture2D> hint;
    ComPtr<ID3D11UnorderedAccessView> hintUav;
    bool hintsOn = false;
    ComPtr<ID3D11ShaderResourceView> flowFwdSrv, flowBwdSrv, costFwdSrv, costBwdSrv;

    NvOFGPUBufferHandle bPrev = nullptr, bNext = nullptr;
    NvOFGPUBufferHandle bFlowFwd = nullptr, bFlowBwd = nullptr;
    NvOFGPUBufferHandle bCostFwd = nullptr, bCostBwd = nullptr;
    NvOFGPUBufferHandle bHint = nullptr;

    bool ready = false;
    std::string report;

    void Add(const std::string& line) {
        report += line;
        report += '\n';
    }

    // Every format the hardware offers for a usage, and whether `want` is there.
    bool QueryFormats(NV_OF_BUFFER_USAGE usage, const char* usageName, DXGI_FORMAT want,
                      DXGI_FORMAT* chosen) {
        uint32_t count = 0;
        NV_OF_STATUS st = fn.nvOFGetSurfaceFormatCountD3D11(hOf, usage, NV_OF_MODE_OPTICALFLOW,
                                                            &count);
        if (st != NV_OF_SUCCESS || count == 0) {
            Add(Fmt("formats[%s]: count query -> %s (count %u)", usageName, StatusName(st), count));
            return false;
        }
        std::vector<DXGI_FORMAT> formats(count, DXGI_FORMAT_UNKNOWN);
        st = fn.nvOFGetSurfaceFormatD3D11(hOf, usage, NV_OF_MODE_OPTICALFLOW, formats.data());
        if (st != NV_OF_SUCCESS) {
            Add(Fmt("formats[%s]: list query -> %s", usageName, StatusName(st)));
            return false;
        }
        std::string list;
        for (uint32_t i = 0; i < count; ++i) {
            if (i) list += ", ";
            list += DxgiFormatName(formats[i]);
        }
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i] == want) found = true;
        }
        *chosen = found ? want : formats[0];
        Add(Fmt("formats[%s]: %s -> using %s%s", usageName, list.c_str(),
                DxgiFormatName(*chosen), found ? "" : " (WANTED FORMAT ABSENT)"));
        return true;
    }

    bool MakeTexture(UINT tw, UINT th, DXGI_FORMAT fmt, UINT bind, ComPtr<ID3D11Texture2D>* out) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = tw;
        td.Height = th;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind;
        const HRESULT hr = device->CreateTexture2D(&td, nullptr, out->ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            Add(Fmt("CreateTexture2D(%ux%u %s) failed %s", tw, th, DxgiFormatName(fmt),
                    HrString(hr).c_str()));
            return false;
        }
        return true;
    }

    bool Register(ID3D11Texture2D* tex, NvOFGPUBufferHandle* out, const char* what) {
        const NV_OF_STATUS st = fn.nvOFRegisterResourceD3D11(hOf, tex, out);
        if (st != NV_OF_SUCCESS) {
            Add(Fmt("nvOFRegisterResourceD3D11(%s) -> %s", what, StatusName(st)));
            return false;
        }
        return true;
    }
};

OfaFlow::OfaFlow() : impl_(std::make_unique<Impl>()) {}
OfaFlow::~OfaFlow() { Destroy(); }

bool OfaFlow::Ready() const { return impl_->ready; }
UINT OfaFlow::GridSize() const { return impl_->grid; }
UINT OfaFlow::GridW() const { return impl_->gridW; }
UINT OfaFlow::GridH() const { return impl_->gridH; }
UINT OfaFlow::SurfaceW() const { return impl_->alignW; }
UINT OfaFlow::SurfaceH() const { return impl_->alignH; }
ID3D11Texture2D* OfaFlow::InputPrev() const { return impl_->inPrev.Get(); }
ID3D11Texture2D* OfaFlow::InputNext() const { return impl_->inNext.Get(); }
ID3D11UnorderedAccessView* OfaFlow::InputPrevUav() const { return impl_->inPrevUav.Get(); }
ID3D11UnorderedAccessView* OfaFlow::InputNextUav() const { return impl_->inNextUav.Get(); }
ID3D11ShaderResourceView* OfaFlow::FlowFwdSrv() const { return impl_->flowFwdSrv.Get(); }
ID3D11ShaderResourceView* OfaFlow::FlowBwdSrv() const { return impl_->flowBwdSrv.Get(); }
ID3D11ShaderResourceView* OfaFlow::CostFwdSrv() const { return impl_->costFwdSrv.Get(); }
ID3D11ShaderResourceView* OfaFlow::CostBwdSrv() const { return impl_->costBwdSrv.Get(); }
const std::string& OfaFlow::Report() const { return impl_->report; }
ID3D11UnorderedAccessView* OfaFlow::HintUav() const { return impl_->hintUav.Get(); }
bool OfaFlow::HintsEnabled() const { return impl_->hintsOn; }

bool OfaFlow::Create(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h, UINT gridSize,
                     std::string* err, bool wantHints) {
    Impl& d = *impl_;
    Destroy();
    d.device = device;
    d.ctx = ctx;
    d.w = w;
    d.h = h;
    d.grid = (gridSize == 1 || gridSize == 2 || gridSize == 4) ? gridSize : 4;
    d.report.clear();

    auto fail = [&](const std::string& m) {
        d.Add("FATAL: " + m);
        if (err) *err = m;
        Destroy();
        return false;
    };

    if (!device || !ctx || w == 0 || h == 0) return fail("bad arguments");

    d.dll = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d.dll) return fail(Fmt("nvofapi64.dll not loadable (GetLastError=%lu)", GetLastError()));

    auto getMax = reinterpret_cast<PFN_GetMaxApiVersion>(
        GetProcAddress(d.dll, "NvOFGetMaxSupportedApiVersion"));
    auto createInstance = reinterpret_cast<PFN_CreateInstanceD3D11>(
        GetProcAddress(d.dll, "NvOFAPICreateInstanceD3D11"));
    if (!createInstance) return fail("NvOFAPICreateInstanceD3D11 export missing");

    if (getMax) {
        uint32_t ver = 0;
        const NV_OF_STATUS st = getMax(&ver);
        d.Add(Fmt("NvOFGetMaxSupportedApiVersion -> %s, driver max %u.%u (header %u.%u)",
                  StatusName(st), ver >> 4, ver & 0xF, NV_OF_API_MAJOR_VERSION,
                  NV_OF_API_MINOR_VERSION));
        if (st == NV_OF_SUCCESS && ver < NV_OF_API_VERSION)
            return fail(Fmt("driver supports API %u.%u, headers are %u.%u", ver >> 4, ver & 0xF,
                            NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION));
    }

    d.fn = {};
    NV_OF_STATUS st = createInstance(NV_OF_API_VERSION, &d.fn);
    if (st != NV_OF_SUCCESS) return fail(Fmt("NvOFAPICreateInstanceD3D11 -> %s", StatusName(st)));
    if (!d.fn.nvCreateOpticalFlowD3D11 || !d.fn.nvOFInit || !d.fn.nvOFExecute ||
        !d.fn.nvOFRegisterResourceD3D11)
        return fail("the function list came back with null entries");
    d.Add("NvOFAPICreateInstanceD3D11: ok");

    st = d.fn.nvCreateOpticalFlowD3D11(device, ctx, &d.hOf);
    if (st != NV_OF_SUCCESS || !d.hOf)
        return fail(Fmt("nvCreateOpticalFlowD3D11 -> %s", StatusName(st)));
    d.Add("nvCreateOpticalFlowD3D11: ok (our own device, no interop)");

    DXGI_FORMAT inFmt = DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT outFmt = DXGI_FORMAT_R16G16_SINT;
    DXGI_FORMAT costFmt = DXGI_FORMAT_R32_UINT;
    if (d.fn.nvOFGetSurfaceFormatCountD3D11 && d.fn.nvOFGetSurfaceFormatD3D11) {
        d.QueryFormats(NV_OF_BUFFER_USAGE_INPUT, "input", DXGI_FORMAT_R8_UNORM, &inFmt);
        d.QueryFormats(NV_OF_BUFFER_USAGE_OUTPUT, "output", DXGI_FORMAT_R16G16_SINT, &outFmt);
        d.QueryFormats(NV_OF_BUFFER_USAGE_COST, "cost", DXGI_FORMAT_R32_UINT, &costFmt);
    } else {
        d.Add("surface format query unavailable - assuming R8_UNORM / R16G16_SINT / R32_UINT");
    }
    if (inFmt != DXGI_FORMAT_R8_UNORM)
        return fail(Fmt("hardware will not take R8_UNORM input (offered %s); the luma path would "
                        "have to change",
                        DxgiFormatName(inFmt)));

    // The hardware works in tiles: the header's ROI rules want x/width aligned
    // to 32*grid and y/height to 8*grid, and an unaligned surface is read with a
    // row stride that is not ours — which showed up as a motion field ramping
    // smoothly from top to bottom on content that has no such motion.
    auto alignUp = [](UINT v, UINT a) { return ((v + a - 1) / a) * a; };
    d.alignW = alignUp(w, 32 * d.grid);
    d.alignH = alignUp(h, 8 * d.grid);
    d.gridW = d.alignW / d.grid;
    d.gridH = d.alignH / d.grid;

    // ---- init. hintGridSize stays UNDEFINED while hints are off (G13: setting
    // it anyway passes creation and then TDRs on execute).
    NV_OF_INIT_PARAMS ip = {};
    ip.width = d.alignW;
    ip.height = d.alignH;
    ip.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(d.grid);
    // G13: hintGridSize must stay UNDEFINED while hints are off — setting it
    // anyway passes creation and then TDRs on execute.
    ip.hintGridSize = wantHints ? static_cast<NV_OF_HINT_VECTOR_GRID_SIZE>(d.grid)
                                : NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    ip.mode = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel = NV_OF_PERF_LEVEL_FAST;
    ip.enableExternalHints = wantHints ? NV_OF_TRUE : NV_OF_FALSE;
    ip.enableOutputCost = NV_OF_TRUE;   // the occlusion evidence
    ip.hPrivData = nullptr;
    ip.enableRoi = NV_OF_FALSE;
    ip.predDirection = NV_OF_PRED_DIRECTION_BOTH;  // forward AND backward in one execute
    ip.enableGlobalFlow = NV_OF_FALSE;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;

    st = d.fn.nvOFInit(d.hOf, &ip);
    if (st != NV_OF_SUCCESS) return fail(Fmt("nvOFInit -> %s", StatusName(st)));
    d.Add(Fmt("nvOFInit: ok  picture %ux%u -> surface %ux%u (aligned to %u x %u), grid %u, "
              "%ux%u vectors, FAST, cost on, both directions",
              w, h, d.alignW, d.alignH, 32 * d.grid, 8 * d.grid, d.grid, d.gridW, d.gridH));

    // ---- surfaces
    if (!d.MakeTexture(d.alignW, d.alignH, inFmt,
                       D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &d.inPrev) ||
        !d.MakeTexture(d.alignW, d.alignH, inFmt,
                       D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &d.inNext) ||
        !d.MakeTexture(d.gridW, d.gridH, outFmt, D3D11_BIND_SHADER_RESOURCE, &d.flowFwd) ||
        !d.MakeTexture(d.gridW, d.gridH, outFmt, D3D11_BIND_SHADER_RESOURCE, &d.flowBwd) ||
        !d.MakeTexture(d.gridW, d.gridH, costFmt, D3D11_BIND_SHADER_RESOURCE, &d.costFwd) ||
        !d.MakeTexture(d.gridW, d.gridH, costFmt, D3D11_BIND_SHADER_RESOURCE, &d.costBwd))
        return fail("surface allocation failed (see report)");

    if (wantHints) {
        // The hint grid equals the output grid because hintGridSize was set to
        // outGridSize; the header allows it to be coarser, which would change
        // these dimensions.
        if (!d.MakeTexture(d.gridW, d.gridH, outFmt,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &d.hint))
            return fail("hint surface allocation failed (see report)");
        d.hintsOn = true;
    }

    HRESULT hr = device->CreateUnorderedAccessView(d.inPrev.Get(), nullptr, &d.inPrevUav);
    if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(d.inNext.Get(), nullptr, &d.inNextUav);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(d.flowFwd.Get(), nullptr, &d.flowFwdSrv);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(d.flowBwd.Get(), nullptr, &d.flowBwdSrv);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(d.costFwd.Get(), nullptr, &d.costFwdSrv);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(d.costBwd.Get(), nullptr, &d.costBwdSrv);
    if (FAILED(hr)) return fail(Fmt("view creation failed %s", HrString(hr).c_str()));

    if (d.hintsOn) {
        hr = device->CreateUnorderedAccessView(d.hint.Get(), nullptr, &d.hintUav);
        if (FAILED(hr)) return fail(Fmt("hint UAV creation failed %s", HrString(hr).c_str()));
        if (!d.Register(d.hint.Get(), &d.bHint, "external hint"))
            return fail("hint registration failed (see report)");
        d.Add(Fmt("external hints: ON, hintGridSize %u, %ux%u vectors, S10.5", d.grid, d.gridW, d.gridH));
    }

    if (!d.Register(d.inPrev.Get(), &d.bPrev, "input prev") ||
        !d.Register(d.inNext.Get(), &d.bNext, "input next") ||
        !d.Register(d.flowFwd.Get(), &d.bFlowFwd, "flow fwd") ||
        !d.Register(d.flowBwd.Get(), &d.bFlowBwd, "flow bwd") ||
        !d.Register(d.costFwd.Get(), &d.bCostFwd, "cost fwd") ||
        !d.Register(d.costBwd.Get(), &d.bCostBwd, "cost bwd"))
        return fail("resource registration failed (see report)");
    d.Add("nvOFRegisterResourceD3D11: 6 of our own textures registered");

    d.ready = true;
    return true;
}

bool OfaFlow::Execute() {
    Impl& d = *impl_;
    if (!d.ready) return false;

    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = d.bPrev;
    in.referenceFrame = d.bNext;
    in.externalHints = d.hintsOn ? d.bHint : nullptr;
    // MEASURED: with temporal hints ON the engine feeds its previous output back
    // as a prior and the field drifts without bound — |v| p50 39 px, p95 528 px,
    // max 1307 px on content whose true motion is 21 and 42 px, ramping smoothly
    // down the frame. With them OFF the same scene gives p50 21.6 px, exactly the
    // ground truth. Our source pairs are 42 ms apart with real scene motion
    // between them, which is not the short-baseline case the hints assume.
    in.disableTemporalHints = NV_OF_TRUE;
    in.hPrivData = nullptr;
    in.numRois = 0;
    in.roiData = nullptr;

    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = d.bFlowFwd;
    out.outputCostBuffer = d.bCostFwd;
    out.hPrivData = nullptr;
    out.bwdOutputBuffer = d.bFlowBwd;
    out.bwdOutputCostBuffer = d.bCostBwd;
    out.globalFlowBuffer = nullptr;

    const NV_OF_STATUS st = d.fn.nvOFExecute(d.hOf, &in, &out);
    if (st != NV_OF_SUCCESS) {
        static NV_OF_STATUS logged = NV_OF_SUCCESS;
        if (st != logged) {
            logged = st;
            LogErr("NVOFA: nvOFExecute -> %s (logged once per distinct status)", StatusName(st));
        }
        return false;
    }
    return true;
}

void OfaFlow::Destroy() {
    Impl& d = *impl_;
    if (d.hOf && d.fn.nvOFUnregisterResourceD3D11) {
        NvOFGPUBufferHandle* handles[] = {&d.bPrev,    &d.bNext,    &d.bFlowFwd,
                                          &d.bFlowBwd, &d.bCostFwd, &d.bCostBwd, &d.bHint};
        for (auto* hp : handles) {
            if (*hp) {
                d.fn.nvOFUnregisterResourceD3D11(*hp);
                *hp = nullptr;
            }
        }
    }
    if (d.hOf && d.fn.nvOFDestroy) d.fn.nvOFDestroy(d.hOf);
    d.hOf = nullptr;

    d.flowFwdSrv.Reset();
    d.flowBwdSrv.Reset();
    d.costFwdSrv.Reset();
    d.costBwdSrv.Reset();
    d.inPrevUav.Reset();
    d.inNextUav.Reset();
    d.flowFwd.Reset();
    d.flowBwd.Reset();
    d.costFwd.Reset();
    d.costBwd.Reset();
    d.inPrev.Reset();
    d.inNext.Reset();

    // The DLL is deliberately left loaded: unloading a graphics runtime after
    // its last object is released buys nothing and risks a worker thread.
    d.dll = nullptr;
    d.ready = false;
}

}  // namespace nsp
