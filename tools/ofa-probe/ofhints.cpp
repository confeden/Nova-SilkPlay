// ofhints.cpp — does this GPU's NVOFA accept EXTERNAL HINTS, and under what constraints?
//
// Standalone probe. Brings up NVOFA over D3D11 with exactly the sequence in
// prototype/nsp_ofa.cpp, dumps EVERY NV_OF_CAPS the driver reports (handling both
// the scalar and the list shape of the two-stage nvOFGetCaps query), then tries to
// create a hint-enabled session at each NV_OF_HINT_VECTOR_GRID_SIZE value.
//
//   build:  build.cmd
//   run:    ofhints.exe [--exec] [W H]
//
//   --exec  additionally submits ONE nvOFExecute with a zero-filled hint buffer.
//           OFF by default: tools/ofa-probe/README.md records that an ill-formed
//           hint configuration is a full TDR, and a TDR takes every other process
//           on this GPU down with it.
//
// Reports in the style of the other probes in this directory: sections, then a
// clearly-marked VERDICT block. Nothing here writes to the project tree.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The vendored SDK headers (MIT, see third_party/nvofapi/README.md).
#include "../../third_party/nvofapi/nvOpticalFlowCommon.h"
#include "../../third_party/nvofapi/nvOpticalFlowD3D11.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- small helpers

using PFN_CreateInstanceD3D11 = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
using PFN_GetMaxApiVersion = NV_OF_STATUS(NVOFAPI*)(uint32_t*);

static void Line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static const char* StatusName(NV_OF_STATUS s) {
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

static const char* CapName(int c) {
    switch (c) {
        case NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES: return "NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES";
        case NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES:   return "NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES";
        case NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE:   return "NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE";
        case NV_OF_CAPS_SUPPORT_HINT_WITH_ST_MODE:   return "NV_OF_CAPS_SUPPORT_HINT_WITH_ST_MODE";
        case NV_OF_CAPS_WIDTH_MIN:                   return "NV_OF_CAPS_WIDTH_MIN";
        case NV_OF_CAPS_HEIGHT_MIN:                  return "NV_OF_CAPS_HEIGHT_MIN";
        case NV_OF_CAPS_WIDTH_MAX:                   return "NV_OF_CAPS_WIDTH_MAX";
        case NV_OF_CAPS_HEIGHT_MAX:                  return "NV_OF_CAPS_HEIGHT_MAX";
        case NV_OF_CAPS_SUPPORT_ROI:                 return "NV_OF_CAPS_SUPPORT_ROI";
        case NV_OF_CAPS_SUPPORT_ROI_MAX_NUM:         return "NV_OF_CAPS_SUPPORT_ROI_MAX_NUM";
        case NV_OF_CAPS_SUPPORT_STEREO:              return "NV_OF_CAPS_SUPPORT_STEREO";
        default: return "<cap beyond the header's enum>";
    }
}

static const char* DxgiFormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_NV12: return "NV12";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R16G16_SINT: return "R16G16_SINT";
        case DXGI_FORMAT_R16G16_UINT: return "R16G16_UINT";
        case DXGI_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
        case DXGI_FORMAT_R16_SINT: return "R16_SINT";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R8_UINT: return "R8_UINT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        default: return "<other>";
    }
}

static const char* HintGridName(uint32_t g) {
    switch (g) {
        case NV_OF_HINT_VECTOR_GRID_SIZE_1: return "NV_OF_HINT_VECTOR_GRID_SIZE_1";
        case NV_OF_HINT_VECTOR_GRID_SIZE_2: return "NV_OF_HINT_VECTOR_GRID_SIZE_2";
        case NV_OF_HINT_VECTOR_GRID_SIZE_4: return "NV_OF_HINT_VECTOR_GRID_SIZE_4";
        case NV_OF_HINT_VECTOR_GRID_SIZE_8: return "NV_OF_HINT_VECTOR_GRID_SIZE_8";
        default: return "<not a NV_OF_HINT_VECTOR_GRID_SIZE value>";
    }
}

static uint32_t AlignUp(uint32_t v, uint32_t a) { return ((v + a - 1) / a) * a; }

// --------------------------------------------------------------- probe state

struct Probe {
    HMODULE dll = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST fn{};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;

    uint32_t w = 2560, h = 1440;
    uint32_t grid = 4;  // the engine's outGridSize
    uint32_t alignW = 0, alignH = 0, gridW = 0, gridH = 0;

    // results carried into the verdict
    bool sessionEverCreated = false;
    bool capsQueryWorked = false;
    bool hintWithOfModeKnown = false;
    uint32_t hintWithOfMode = 0;
    bool hintWithStModeKnown = false;
    uint32_t hintWithStMode = 0;
    std::vector<uint32_t> hintGridSizes;      // as reported by the driver
    bool hintGridSizesKnown = false;
    std::vector<DXGI_FORMAT> hintFormats;     // DXGI formats offered for USAGE_HINT
    bool hintFormatsKnown = false;
    // per hint grid size: 0 = untested, 1 = nvOFInit succeeded, 2 = nvOFInit rejected
    NV_OF_STATUS initStatus[9] = {};
    bool initTried[9] = {};
    int execAttempted = 0;                    // -1 skipped, 0 none, 1 tried
    NV_OF_STATUS execStatus = NV_OF_SUCCESS;
    uint32_t execHintGrid = 0;
    bool execFlowLooksSane = false;
    std::string execNote;
};

static void DumpLastError(Probe& p, NvOFHandle hOf) {
    if (!p.fn.nvOFGetLastError || !hOf) return;
    char buf[512] = {};
    uint32_t size = sizeof(buf);
    const NV_OF_STATUS st = p.fn.nvOFGetLastError(hOf, buf, &size);
    if (st == NV_OF_SUCCESS && buf[0]) Line("      driver last error: %s", buf);
}

// Bring the API up. Returns false only for a hard bring-up failure (no DLL, no
// export, version mismatch) — that is a *different* answer from "unsupported".
static bool LoadApi(Probe& p) {
    p.dll = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!p.dll) {
        Line("  nvofapi64.dll NOT loadable (GetLastError=%lu)", GetLastError());
        return false;
    }
    Line("  nvofapi64.dll: loaded from System32");

    auto getMax = reinterpret_cast<PFN_GetMaxApiVersion>(
        GetProcAddress(p.dll, "NvOFGetMaxSupportedApiVersion"));
    auto createInstance = reinterpret_cast<PFN_CreateInstanceD3D11>(
        GetProcAddress(p.dll, "NvOFAPICreateInstanceD3D11"));
    if (!createInstance) {
        Line("  NvOFAPICreateInstanceD3D11 export MISSING");
        return false;
    }

    if (getMax) {
        uint32_t ver = 0;
        const NV_OF_STATUS st = getMax(&ver);
        Line("  NvOFGetMaxSupportedApiVersion -> %s, driver max %u.%u (headers %u.%u)",
             StatusName(st), ver >> 4, ver & 0xF, NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION);
        if (st == NV_OF_SUCCESS && ver < NV_OF_API_VERSION) {
            Line("  FATAL: driver API %u.%u is older than the vendored headers %u.%u", ver >> 4,
                 ver & 0xF, NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION);
            return false;
        }
    } else {
        Line("  NvOFGetMaxSupportedApiVersion export missing (continuing)");
    }

    p.fn = {};
    const NV_OF_STATUS st = createInstance(NV_OF_API_VERSION, &p.fn);
    if (st != NV_OF_SUCCESS) {
        Line("  NvOFAPICreateInstanceD3D11 -> %s", StatusName(st));
        return false;
    }
    Line("  NvOFAPICreateInstanceD3D11: ok");
    Line("  function list: nvCreateOpticalFlowD3D11=%s nvOFInit=%s nvOFGetCaps=%s "
         "nvOFExecute=%s nvOFGetSurfaceFormat*=%s",
         p.fn.nvCreateOpticalFlowD3D11 ? "yes" : "NULL", p.fn.nvOFInit ? "yes" : "NULL",
         p.fn.nvOFGetCaps ? "yes" : "NULL", p.fn.nvOFExecute ? "yes" : "NULL",
         (p.fn.nvOFGetSurfaceFormatCountD3D11 && p.fn.nvOFGetSurfaceFormatD3D11) ? "yes" : "NULL");
    return p.fn.nvCreateOpticalFlowD3D11 != nullptr && p.fn.nvOFInit != nullptr;
}

// Create the D3D11 device and report which adapter we actually got.
static bool MakeDevice(Probe& p) {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) !=
                         DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d = {};
            adapter->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr,
                                nullptr);
            Line("  adapter[%u]: %s  vendor=0x%04X device=0x%04X vram=%llu MB", i, name, d.VendorId,
                 d.DeviceId, static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20));
            if (d.VendorId == 0x10DE) break;  // NVIDIA
        }
    }

    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter.Get(),
                                   adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, 0, want, ARRAYSIZE(want), D3D11_SDK_VERSION, &p.dev,
                                   &got, &p.ctx);
    if (FAILED(hr)) {
        Line("  D3D11CreateDevice FAILED hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    Line("  D3D11CreateDevice: ok, feature level %u.%u", (got >> 12) & 0xF, (got >> 8) & 0xF);
    return true;
}

// A fresh session, initialised exactly the way nsp_ofa.cpp does it (hints OFF).
// `hintGrid` non-zero switches external hints ON for the attempt.
static NV_OF_STATUS MakeSession(Probe& p, uint32_t hintGrid, NvOFHandle* out,
                                NV_OF_STATUS* createStatus) {
    *out = nullptr;
    NvOFHandle hOf = nullptr;
    NV_OF_STATUS st = p.fn.nvCreateOpticalFlowD3D11(p.dev.Get(), p.ctx.Get(), &hOf);
    if (createStatus) *createStatus = st;
    if (st != NV_OF_SUCCESS || !hOf) return st;

    NV_OF_INIT_PARAMS ip = {};
    ip.width = p.alignW;
    ip.height = p.alignH;
    ip.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(p.grid);
    ip.hintGridSize = hintGrid ? static_cast<NV_OF_HINT_VECTOR_GRID_SIZE>(hintGrid)
                               : NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    ip.mode = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel = NV_OF_PERF_LEVEL_FAST;
    ip.enableExternalHints = hintGrid ? NV_OF_TRUE : NV_OF_FALSE;
    ip.enableOutputCost = NV_OF_TRUE;
    ip.hPrivData = nullptr;
    ip.enableRoi = NV_OF_FALSE;
    ip.predDirection = NV_OF_PRED_DIRECTION_BOTH;
    ip.enableGlobalFlow = NV_OF_FALSE;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;

    st = p.fn.nvOFInit(hOf, &ip);
    if (st != NV_OF_SUCCESS) {
        DumpLastError(p, hOf);
        if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hOf);
        return st;
    }
    *out = hOf;
    return NV_OF_SUCCESS;
}

// ------------------------------------------------------------------- caps dump

// The two-stage query the header documents: capsVal = NULL returns the count,
// then a second call fills the array. Several caps are LISTS, so never assume 1.
static bool QueryCap(Probe& p, NvOFHandle hOf, int cap, std::vector<uint32_t>* vals,
                     NV_OF_STATUS* stOut) {
    vals->clear();
    uint32_t size = 0;
    NV_OF_STATUS st = p.fn.nvOFGetCaps(hOf, static_cast<NV_OF_CAPS>(cap), nullptr, &size);
    *stOut = st;
    if (st != NV_OF_SUCCESS) return false;
    if (size == 0) return true;  // success with nothing to report
    if (size > 256) size = 256;  // paranoia: never trust a wild count
    vals->assign(size, 0u);
    st = p.fn.nvOFGetCaps(hOf, static_cast<NV_OF_CAPS>(cap), vals->data(), &size);
    *stOut = st;
    if (st != NV_OF_SUCCESS) {
        vals->clear();
        return false;
    }
    if (size < vals->size()) vals->resize(size);
    return true;
}

static void DumpAllCaps(Probe& p, NvOFHandle hOf, const char* when) {
    Line("");
    Line("--- NV_OF_CAPS dump (%s) ---", when);
    if (!p.fn.nvOFGetCaps) {
        Line("  nvOFGetCaps is NULL in the function list — no capability query available.");
        return;
    }
    bool any = false;
    // The header's enum stops at NV_OF_CAPS_SUPPORT_MAX; probe two past it in case
    // the driver knows caps this header does not.
    const int last = static_cast<int>(NV_OF_CAPS_SUPPORT_MAX) + 2;
    for (int cap = 0; cap < last; ++cap) {
        std::vector<uint32_t> vals;
        NV_OF_STATUS st = NV_OF_SUCCESS;
        const bool ok = QueryCap(p, hOf, cap, &vals, &st);
        if (!ok) {
            Line("  [%2d] %-40s -> %s", cap, CapName(cap), StatusName(st));
            continue;
        }
        any = true;
        std::string list;
        char buf[64];
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) list += ", ";
            snprintf(buf, sizeof(buf), "%u", vals[i]);
            list += buf;
        }
        if (vals.empty()) list = "<empty list>";
        Line("  [%2d] %-40s -> count %u : [%s]", cap, CapName(cap),
             static_cast<unsigned>(vals.size()), list.c_str());

        if (cap == NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES) {
            p.hintGridSizes = vals;
            p.hintGridSizesKnown = true;
            for (uint32_t v : vals) Line("         hint grid %u = %s", v, HintGridName(v));
        }
        if (cap == NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES) {
            for (uint32_t v : vals) Line("         output grid %u", v);
        }
        if (cap == NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE && !vals.empty()) {
            p.hintWithOfMode = vals[0];
            p.hintWithOfModeKnown = true;
        }
        if (cap == NV_OF_CAPS_SUPPORT_HINT_WITH_ST_MODE && !vals.empty()) {
            p.hintWithStMode = vals[0];
            p.hintWithStModeKnown = true;
        }
    }
    if (any) p.capsQueryWorked = true;
}

static void DumpSurfaceFormats(Probe& p, NvOFHandle hOf) {
    Line("");
    Line("--- DXGI surface formats per NV_OF_BUFFER_USAGE (NV_OF_MODE_OPTICALFLOW) ---");
    if (!p.fn.nvOFGetSurfaceFormatCountD3D11 || !p.fn.nvOFGetSurfaceFormatD3D11) {
        Line("  surface format query not available in this function list");
        return;
    }
    struct U { NV_OF_BUFFER_USAGE u; const char* name; };
    const U usages[] = {
        {NV_OF_BUFFER_USAGE_INPUT, "INPUT"},
        {NV_OF_BUFFER_USAGE_OUTPUT, "OUTPUT"},
        {NV_OF_BUFFER_USAGE_HINT, "HINT"},
        {NV_OF_BUFFER_USAGE_COST, "COST"},
        {NV_OF_BUFFER_USAGE_GLOBAL_FLOW, "GLOBAL_FLOW"},
    };
    for (const U& u : usages) {
        uint32_t count = 0;
        NV_OF_STATUS st =
            p.fn.nvOFGetSurfaceFormatCountD3D11(hOf, u.u, NV_OF_MODE_OPTICALFLOW, &count);
        if (st != NV_OF_SUCCESS || count == 0) {
            Line("  %-12s count -> %s (count %u)", u.name, StatusName(st), count);
            continue;
        }
        std::vector<DXGI_FORMAT> f(count, DXGI_FORMAT_UNKNOWN);
        st = p.fn.nvOFGetSurfaceFormatD3D11(hOf, u.u, NV_OF_MODE_OPTICALFLOW, f.data());
        if (st != NV_OF_SUCCESS) {
            Line("  %-12s list  -> %s", u.name, StatusName(st));
            continue;
        }
        std::string list;
        char buf[64];
        for (uint32_t i = 0; i < count; ++i) {
            if (i) list += ", ";
            list += DxgiFormatName(f[i]);
            snprintf(buf, sizeof(buf), "(%d)", static_cast<int>(f[i]));
            list += buf;
        }
        Line("  %-12s count %u : %s", u.name, count, list.c_str());
        if (u.u == NV_OF_BUFFER_USAGE_HINT) {
            p.hintFormats = f;
            p.hintFormatsKnown = true;
        }
    }
}

// --------------------------------------------------- hint-enabled session tests

static void TestHintSessions(Probe& p) {
    Line("");
    Line("--- nvOFInit with enableExternalHints = 1, one fresh session per hint grid size ---");
    Line("  (outGridSize = %u, surface %ux%u)", p.grid, p.alignW, p.alignH);
    const uint32_t grids[] = {1, 2, 4, 8};
    for (uint32_t g : grids) {
        NvOFHandle hOf = nullptr;
        NV_OF_STATUS createSt = NV_OF_SUCCESS;
        const NV_OF_STATUS st = MakeSession(p, g, &hOf, &createSt);
        p.initTried[g] = true;
        p.initStatus[g] = st;
        if (createSt != NV_OF_SUCCESS) {
            Line("  hintGridSize=%u : nvCreateOpticalFlowD3D11 -> %s  (NO SESSION, not a "
                 "capability answer)",
                 g, StatusName(createSt));
            continue;
        }
        if (st == NV_OF_SUCCESS) {
            p.sessionEverCreated = true;
            Line("  hintGridSize=%u : nvOFInit -> SUCCESS   hint buffer would be %u x %u vectors", g,
                 p.alignW / g, p.alignH / g);
            if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hOf);
        } else {
            Line("  hintGridSize=%u : nvOFInit -> %s", g, StatusName(st));
        }
    }
}

// ------------------------------------------------------------- optional execute

static UINT BytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R32_UINT: return 4;
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R8G8_UNORM: return 2;
        default: return 1;
    }
}

static bool MakeTex(Probe& p, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind,
                    const void* initial, ComPtr<ID3D11Texture2D>* out) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = initial;
    sd.SysMemPitch = w * BytesPerPixel(fmt);
    const HRESULT hr =
        p.dev->CreateTexture2D(&td, initial ? &sd : nullptr, out->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        Line("    CreateTexture2D(%ux%u %s) failed hr=0x%08lX", w, h, DxgiFormatName(fmt),
             static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

struct ExecResult {
    bool ran = false;
    NV_OF_STATUS st = NV_OF_SUCCESS;
    double medX = 0, medY = 0;   // pixels
    uint32_t exactHits = 0;      // vectors equal to ground truth
    uint32_t total = 0;
    std::vector<int16_t> field;  // the whole forward flow field, xy interleaved
};

// How two flow fields differ: number of vectors that are not identical, and the
// largest per-component gap in pixels.
static void CompareFields(const ExecResult& a, const ExecResult& b, uint32_t* differing,
                          double* maxGapPx) {
    *differing = 0;
    *maxGapPx = 0.0;
    const size_t n = (a.field.size() < b.field.size()) ? a.field.size() : b.field.size();
    for (size_t i = 0; i + 1 < n; i += 2) {
        const int dx = a.field[i] - b.field[i];
        const int dy = a.field[i + 1] - b.field[i + 1];
        if (dx || dy) {
            ++(*differing);
            const double g = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 32.0;
            if (g > *maxGapPx) *maxGapPx = g;
        }
    }
}

// Deterministic luma noise, and the same noise shifted by (dx, 0). Full-resolution
// noise is what verify_ofcontent.py feeds the OFA, so keep that precedent.
static void MakeContent(uint32_t w, uint32_t h, int dx, std::vector<uint8_t>* prev,
                        std::vector<uint8_t>* next) {
    prev->assign(static_cast<size_t>(w) * h, 0);
    next->assign(static_cast<size_t>(w) * h, 0);
    uint32_t s = 0x13579BDFu;
    for (size_t i = 0; i < prev->size(); ++i) {
        s = s * 1664525u + 1013904223u;
        (*prev)[i] = static_cast<uint8_t>(s >> 24);
    }
    // next(x) = prev(x - dx)  =>  forward flow from prev to next is +dx.
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sp = prev->data() + static_cast<size_t>(y) * w;
        uint8_t* dp = next->data() + static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = static_cast<int>(x) - dx;
            dp[x] = (sx >= 0 && sx < static_cast<int>(w)) ? sp[sx] : 0;
        }
    }
}

// One execute. `useHints` false runs the engine's current hints-OFF configuration;
// true enables hints at `hintGrid` and fills the hint buffer with (hintX, hintY)
// pixels, converted to the S10.5 fixed point NV_OF_FLOW_VECTOR uses.
static void RunExec(Probe& p, bool useHints, uint32_t hintGrid, int hintX, int hintY, int truthDx,
                    const char* label, ExecResult* res) {
    Line("  [%s]", label);
    NvOFHandle hOf = nullptr;
    NV_OF_STATUS createSt = NV_OF_SUCCESS;
    NV_OF_STATUS st = MakeSession(p, useHints ? hintGrid : 0, &hOf, &createSt);
    if (st != NV_OF_SUCCESS) {
        res->st = st;
        Line("    session bring-up -> create %s / init %s", StatusName(createSt), StatusName(st));
        return;
    }

    DXGI_FORMAT hintFmt = DXGI_FORMAT_R16G16_SINT;
    if (p.hintFormatsKnown)
        for (DXGI_FORMAT f : p.hintFormats)
            if (f == DXGI_FORMAT_R16G16_SINT) hintFmt = f;

    std::vector<uint8_t> prev, next;
    MakeContent(p.alignW, p.alignH, truthDx, &prev, &next);

    const uint32_t hw = p.alignW / hintGrid, hh = p.alignH / hintGrid;
    std::vector<int16_t> hintData(static_cast<size_t>(hw) * hh * 2, 0);
    for (size_t i = 0; i < hintData.size(); i += 2) {
        hintData[i] = static_cast<int16_t>(hintX * 32);      // S10.5
        hintData[i + 1] = static_cast<int16_t>(hintY * 32);
    }

    ComPtr<ID3D11Texture2D> inPrev, inNext, hint, flowF, flowB, costF, costB;
    const bool ok =
        MakeTex(p, p.alignW, p.alignH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                prev.data(), &inPrev) &&
        MakeTex(p, p.alignW, p.alignH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                next.data(), &inNext) &&
        MakeTex(p, hw, hh, hintFmt, D3D11_BIND_SHADER_RESOURCE, hintData.data(), &hint) &&
        MakeTex(p, p.gridW, p.gridH, DXGI_FORMAT_R16G16_SINT, D3D11_BIND_SHADER_RESOURCE, nullptr,
                &flowF) &&
        MakeTex(p, p.gridW, p.gridH, DXGI_FORMAT_R16G16_SINT, D3D11_BIND_SHADER_RESOURCE, nullptr,
                &flowB) &&
        MakeTex(p, p.gridW, p.gridH, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE, nullptr,
                &costF) &&
        MakeTex(p, p.gridW, p.gridH, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE, nullptr,
                &costB);
    if (!ok) {
        if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hOf);
        return;
    }

    NvOFGPUBufferHandle bPrev = nullptr, bNext = nullptr, bHint = nullptr, bF = nullptr,
                        bB = nullptr, bCF = nullptr, bCB = nullptr;
    struct R { ID3D11Texture2D* t; NvOFGPUBufferHandle* h; const char* n; bool need; };
    const R regs[] = {{inPrev.Get(), &bPrev, "input prev", true},
                      {inNext.Get(), &bNext, "input next", true},
                      {hint.Get(), &bHint, "hint", useHints},
                      {flowF.Get(), &bF, "flow fwd", true},
                      {flowB.Get(), &bB, "flow bwd", true},
                      {costF.Get(), &bCF, "cost fwd", true},
                      {costB.Get(), &bCB, "cost bwd", true}};
    for (const R& r : regs) {
        if (!r.need) continue;
        st = p.fn.nvOFRegisterResourceD3D11(hOf, r.t, r.h);
        if (st != NV_OF_SUCCESS) {
            Line("    nvOFRegisterResourceD3D11(%s) -> %s", r.n, StatusName(st));
            res->st = st;
            if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hOf);
            return;
        }
    }
    if (useHints)
        Line("    hint buffer: %u x %u %s, every vector = (%+d, %+d) px", hw, hh,
             DxgiFormatName(hintFmt), hintX, hintY);

    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = bPrev;
    in.referenceFrame = bNext;
    in.externalHints = useHints ? bHint : nullptr;
    in.disableTemporalHints = NV_OF_TRUE;
    in.hPrivData = nullptr;
    in.numRois = 0;
    in.roiData = nullptr;

    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = bF;
    out.outputCostBuffer = bCF;
    out.hPrivData = nullptr;
    out.bwdOutputBuffer = bB;
    out.bwdOutputCostBuffer = bCB;
    out.globalFlowBuffer = nullptr;

    st = p.fn.nvOFExecute(hOf, &in, &out);
    res->st = st;
    Line("    nvOFExecute -> %s", StatusName(st));
    if (st != NV_OF_SUCCESS) {
        DumpLastError(p, hOf);
    } else {
        D3D11_TEXTURE2D_DESC sd = {};
        flowF->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        if (SUCCEEDED(p.dev->CreateTexture2D(&sd, nullptr, &stage))) {
            p.ctx->CopyResource(stage.Get(), flowF.Get());
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(p.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<int> xs, ys;
                xs.reserve(static_cast<size_t>(p.gridW) * p.gridH);
                ys.reserve(static_cast<size_t>(p.gridW) * p.gridH);
                res->field.reserve(static_cast<size_t>(p.gridW) * p.gridH * 2);
                for (uint32_t y = 0; y < p.gridH; ++y) {
                    const int16_t* row = reinterpret_cast<const int16_t*>(
                        static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch);
                    for (uint32_t x = 0; x < p.gridW; ++x) {
                        const int16_t fx = row[2 * x], fy = row[2 * x + 1];
                        xs.push_back(fx);
                        ys.push_back(fy);
                        res->field.push_back(fx);
                        res->field.push_back(fy);
                        if (fx == truthDx * 32 && fy == 0) ++res->exactHits;
                    }
                }
                p.ctx->Unmap(stage.Get(), 0);
                res->total = static_cast<uint32_t>(xs.size());
                std::vector<int> sx = xs, sy = ys;
                std::sort(sx.begin(), sx.end());
                std::sort(sy.begin(), sy.end());
                res->medX = sx.empty() ? 0 : sx[sx.size() / 2] / 32.0;
                res->medY = sy.empty() ? 0 : sy[sy.size() / 2] / 32.0;
                res->ran = true;
                Line("    flow: median (%.2f, %.2f) px, %u/%u vectors exactly (%+d, 0)",
                     res->medX, res->medY, res->exactHits, res->total, truthDx);
            }
        }
    }

    if (p.fn.nvOFUnregisterResourceD3D11) {
        NvOFGPUBufferHandle hs[] = {bPrev, bNext, bHint, bF, bB, bCF, bCB};
        for (NvOFGPUBufferHandle h2 : hs)
            if (h2) p.fn.nvOFUnregisterResourceD3D11(h2);
    }
    if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hOf);
}

// Does the hardware merely ACCEPT a hint buffer, or does it actually READ it?
// Five executes on identical content whose true forward motion is +TRUTH_DX px:
//   A   hints off                (the engine's configuration today)
//   B   hints on, all zero       (a hint that says "no motion")
//   B2  hints on, all zero AGAIN — the DETERMINISM CONTROL. Without it, a small
//       B-vs-C difference proves nothing: it could just be run-to-run noise.
//   C   hints on, all +HINT_FAR  (a hint that is deliberately, grossly wrong)
//   D   hints on, all +TRUTH_DX  (a hint that is exactly right)
// The hint buffer is only demonstrably consumed if B-vs-C (or B-vs-D) differs by
// MORE than the B-vs-B2 noise floor.
static void TryExecuteWithHints(Probe& p, uint32_t hintGrid) {
    const int kTruthDx = 12;
    const int kHintFar = 96;
    Line("");
    Line("--- hinted nvOFExecute: accepted, or actually consumed? (hintGridSize=%u) ---", hintGrid);
    Line("  content: deterministic full-res luma noise, next = prev shifted +%d px in x",
         kTruthDx);
    p.execAttempted = 1;
    p.execHintGrid = hintGrid;

    ExecResult a, b, b2, c, d;
    RunExec(p, false, hintGrid, 0, 0, kTruthDx, "A: hints OFF (the engine's config today)", &a);
    RunExec(p, true, hintGrid, 0, 0, kTruthDx, "B: hints ON, all (0, 0)", &b);
    RunExec(p, true, hintGrid, 0, 0, kTruthDx, "B2: hints ON, all (0, 0) AGAIN (noise floor)", &b2);
    RunExec(p, true, hintGrid, kHintFar, 0, kTruthDx, "C: hints ON, all (+96, 0) - grossly wrong",
            &c);
    RunExec(p, true, hintGrid, kTruthDx, 0, kTruthDx, "D: hints ON, all (+12, 0) - exactly right",
            &d);

    p.execStatus = c.st != NV_OF_SUCCESS ? c.st : (b.st != NV_OF_SUCCESS ? b.st : a.st);
    if (!b.ran || !b2.ran || !c.ran || !d.ran) {
        p.execNote = "at least one hinted execute did not complete - inconclusive";
        Line("  INCONCLUSIVE: a run did not complete.");
        return;
    }
    p.execFlowLooksSane = a.ran;

    uint32_t nBB2 = 0, nBC = 0, nBD = 0, nAB = 0;
    double gBB2 = 0, gBC = 0, gBD = 0, gAB = 0;
    CompareFields(b, b2, &nBB2, &gBB2);
    CompareFields(b, c, &nBC, &gBC);
    CompareFields(b, d, &nBD, &gBD);
    if (a.ran) CompareFields(a, b, &nAB, &gAB);

    Line("");
    Line("  field differences (of %u vectors):", b.total);
    Line("    B vs B2  (SAME hints, repeat)   : %6u differ, max gap %.2f px   <- noise floor",
         nBB2, gBB2);
    Line("    B vs C   (zero vs +96 hints)    : %6u differ, max gap %.2f px", nBC, gBC);
    Line("    B vs D   (zero vs +12 hints)    : %6u differ, max gap %.2f px", nBD, gBD);
    if (a.ran)
        Line("    A vs B   (hints off vs on)      : %6u differ, max gap %.2f px", nAB, gAB);
    Line("  vectors exactly (+%d, 0): A %u, B %u, B2 %u, C %u, D %u  (of %u)", kTruthDx,
         a.exactHits, b.exactHits, b2.exactHits, c.exactHits, d.exactHits, b.total);

    const uint32_t floorN = nBB2;
    const bool cBeatsNoise = nBC > floorN * 4 + 64;
    const bool dBeatsNoise = nBD > floorN * 4 + 64;

    if (nBB2 != 0)
        Line("  NOTE: the OFA is NOT bit-deterministic — two identical calls already differ in %u "
             "vectors. Anything at or below that scale is noise, not hint influence.",
             nBB2);

    if (cBeatsNoise || dBeatsNoise) {
        p.execNote = "hints are CONSUMED: changing the hint buffer moved the flow field well "
                     "beyond the repeat-run noise floor";
        Line("  => The hardware READS the hint buffer. Changing the hints moved the field far "
             "beyond the noise floor.");
    } else if (nBC == 0 && nBD == 0) {
        p.execNote = "hints ACCEPTED BUT IGNORED on this content: grossly wrong and exactly "
                     "right hints both produced a byte-identical field";
        Line("  => The hint buffer is accepted and produced NO change at all on this content.");
    } else {
        p.execNote = "hint influence NOT DEMONSTRATED on this content: the change from swapping "
                     "hints is at the same scale as the repeat-run noise";
        Line("  => INCONCLUSIVE on this content. The change from swapping the hints (%u / %u "
             "vectors) is not distinguishable from the %u-vector noise floor.",
             nBC, nBD, nBB2);
        Line("     nvOFInit and nvOFExecute both accept hints; whether they steer the search");
        Line("     needs content where the unhinted answer is actually wrong.");
    }
}

// ------------------------------------------------------------------ the verdict

static void Verdict(Probe& p) {
    Line("");
    Line("================================ VERDICT ================================");

    if (!p.sessionEverCreated && !p.capsQueryWorked) {
        Line("INCONCLUSIVE — WE NEVER GOT A SESSION.");
        Line("  This is NOT 'the driver says hints are unsupported'. Nothing was answered.");
        Line("  Re-run when the GPU is idle.");
        Line("=========================================================================");
        return;
    }

    // 1 -------------------------------------------------------------------
    Line("1. NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE");
    if (!p.hintWithOfModeKnown) {
        Line("   UNKNOWN — the capability query did not return a value. Uncertain.");
    } else if (p.hintWithOfMode) {
        Line("   SUPPORTED. The driver reports %u for NV_OF_MODE_OPTICALFLOW on this GPU.",
             p.hintWithOfMode);
    } else {
        Line("   NOT SUPPORTED. The driver reports 0 for NV_OF_MODE_OPTICALFLOW on this GPU.");
        Line("   External hints are unavailable here. Seeding NVOFA with our own coarse");
        Line("   estimate is not a design direction that exists on this hardware.");
    }
    if (p.hintWithStModeKnown)
        Line("   (stereo mode, for reference: NV_OF_CAPS_SUPPORT_HINT_WITH_ST_MODE = %u)",
             p.hintWithStMode);

    // 2 -------------------------------------------------------------------
    Line("");
    Line("2. Supported NV_OF_HINT_VECTOR_GRID_SIZE values");
    if (!p.hintGridSizesKnown) {
        Line("   UNKNOWN — NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES did not return. Uncertain.");
    } else if (p.hintGridSizes.empty()) {
        Line("   The driver returned an EMPTY list — no hint grid size is offered.");
    } else {
        std::string s;
        char buf[32];
        for (size_t i = 0; i < p.hintGridSizes.size(); ++i) {
            if (i) s += ", ";
            snprintf(buf, sizeof(buf), "%u", p.hintGridSizes[i]);
            s += buf;
        }
        Line("   Driver-reported list: [%s]", s.c_str());
    }
    Line("   Cross-check by construction (fresh session, enableExternalHints=1):");
    for (uint32_t g : {1u, 2u, 4u, 8u}) {
        if (!p.initTried[g]) continue;
        Line("     hintGridSize=%u -> nvOFInit %s", g, StatusName(p.initStatus[g]));
    }

    // 3 -------------------------------------------------------------------
    Line("");
    Line("3. Legal hint grid sizes for THIS engine (outGridSize = 4)");
    Line("   The header rule is hintGridSize >= outGridSize, so of {1,2,4,8} only 4 and 8");
    Line("   can ever be legal for us; 1 and 2 are excluded by the rule regardless of caps.");
    {
        bool any = false;
        for (uint32_t g : {4u, 8u}) {
            const bool capOk = !p.hintGridSizesKnown ||
                               [&] {
                                   for (uint32_t v : p.hintGridSizes)
                                       if (v == g) return true;
                                   return false;
                               }();
            const bool initOk = p.initTried[g] && p.initStatus[g] == NV_OF_SUCCESS;
            if (capOk && initOk) {
                Line("     hintGridSize = %u : LEGAL (caps list %s, nvOFInit accepted it)", g,
                     p.hintGridSizesKnown ? "agrees" : "unavailable");
                any = true;
            } else {
                Line("     hintGridSize = %u : not usable (caps %s, nvOFInit %s)", g,
                     capOk ? "ok" : "absent",
                     p.initTried[g] ? StatusName(p.initStatus[g]) : "untested");
            }
        }
        if (!any) Line("     NONE. No hint grid size is usable at outGridSize = 4.");
    }

    // 4 -------------------------------------------------------------------
    Line("");
    Line("4. Hint buffer format and dimensions");
    Line("   Format per the header: NV_OF_MODE_OPTICALFLOW hints are NV_OF_FLOW_VECTOR,");
    Line("   i.e. two int16 in S10.5 fixed point (flowx, flowy) = NV_OF_BUFFER_FORMAT_SHORT2.");
    if (p.hintFormatsKnown && !p.hintFormats.empty()) {
        std::string s;
        for (size_t i = 0; i < p.hintFormats.size(); ++i) {
            if (i) s += ", ";
            s += DxgiFormatName(p.hintFormats[i]);
        }
        Line("   DXGI formats the driver actually offers for NV_OF_BUFFER_USAGE_HINT: %s",
             s.c_str());
    } else if (p.hintFormatsKnown) {
        Line("   The driver offered NO DXGI format for NV_OF_BUFFER_USAGE_HINT.");
    } else {
        Line("   DXGI hint format list: NOT QUERIED / query failed. Uncertain.");
    }
    Line("   Dimensions are in hint-grid units (NV_OF_BUFFER_DESCRIPTOR doc): the buffer is");
    Line("   width/hintGridSize by height/hintGridSize vectors, over the SAME aligned surface");
    Line("   passed to nvOFInit, not the raw picture.");
    Line("   For our %ux%u picture (aligned to %ux%u at outGridSize 4):", p.w, p.h, p.alignW,
         p.alignH);
    for (uint32_t g : {4u, 8u})
        Line("     hintGridSize %u -> %u x %u hint vectors (output is %u x %u)", g, p.alignW / g,
             p.alignH / g, p.gridW, p.gridH);

    if (p.execAttempted == 1) {
        Line("");
        Line("   Live execute check (hintGridSize=%u): nvOFExecute -> %s%s%s", p.execHintGrid,
             StatusName(p.execStatus), p.execNote.empty() ? "" : " — ", p.execNote.c_str());
    } else {
        Line("");
        Line("   Live execute check: NOT RUN (pass --exec). Everything above about the hint");
        Line("   buffer's shape is from the caps query, the header, and a successful nvOFInit;");
        Line("   it has not been confirmed by an actual hinted execute.");
    }
    Line("=========================================================================");
}

// ------------------------------------------------------------------------ main

int main(int argc, char** argv) {
    Probe p;
    bool doExec = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--exec") == 0) doExec = true;
        else if (i + 1 < argc && p.w == 2560) {
            p.w = static_cast<uint32_t>(atoi(argv[i]));
            p.h = static_cast<uint32_t>(atoi(argv[i + 1]));
            ++i;
        }
    }
    if (p.w == 0 || p.h == 0) { p.w = 2560; p.h = 1440; }

    p.alignW = AlignUp(p.w, 32 * p.grid);
    p.alignH = AlignUp(p.h, 8 * p.grid);
    p.gridW = p.alignW / p.grid;
    p.gridH = p.alignH / p.grid;

    Line("=== ofhints — NVOFA external-hint capability probe ===");
    Line("picture %ux%u -> aligned surface %ux%u, outGridSize %u, %ux%u output vectors", p.w, p.h,
         p.alignW, p.alignH, p.grid, p.gridW, p.gridH);

    Line("");
    Line("--- device ---");
    if (!MakeDevice(p)) {
        Line("FATAL: no D3D11 device. Nothing was answered.");
        return 2;
    }

    Line("");
    Line("--- nvofapi bring-up (same sequence as prototype/nsp_ofa.cpp) ---");
    if (!LoadApi(p)) {
        Line("FATAL: the NVOFA API did not come up. This is a BRING-UP failure, NOT a");
        Line("       statement about hint support.");
        return 2;
    }

    // A plain, hints-OFF session first — that is the configuration the engine runs
    // today, and it is the one that must succeed before any caps answer means
    // anything. Retry: the OFA may be busy with another process.
    Line("");
    Line("--- baseline session (hints OFF, exactly the engine's configuration) ---");
    NvOFHandle hBase = nullptr;
    NV_OF_STATUS baseCreate = NV_OF_SUCCESS, baseInit = NV_OF_SUCCESS;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        baseInit = MakeSession(p, 0, &hBase, &baseCreate);
        if (baseInit == NV_OF_SUCCESS) break;
        Line("  attempt %d/3: nvCreateOpticalFlowD3D11 %s, nvOFInit %s", attempt,
             StatusName(baseCreate), StatusName(baseInit));
        if (attempt < 3) {
            Line("  the OFA may be busy with another process — waiting 10 s and retrying");
            Sleep(10000);
        }
    }
    if (baseInit != NV_OF_SUCCESS) {
        Line("  COULD NOT GET A SESSION AT ALL (create %s, init %s).", StatusName(baseCreate),
             StatusName(baseInit));
        Line("  This is NOT a 'driver says unsupported' answer — nothing was measured.");
        Verdict(p);
        return 3;
    }
    p.sessionEverCreated = true;
    Line("  nvCreateOpticalFlowD3D11 + nvOFInit: ok (FAST, cost on, both directions)");

    DumpAllCaps(p, hBase, "on an initialised hints-OFF session");
    DumpSurfaceFormats(p, hBase);

    if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hBase);
    hBase = nullptr;

    // Caps on a created-but-NOT-initialised session: the header lists
    // NOT_INITIALIZED as a possible return, so record which shape actually works.
    Line("");
    Line("--- nvOFGetCaps before nvOFInit (does the query need an initialised session?) ---");
    {
        NvOFHandle hRaw = nullptr;
        const NV_OF_STATUS st = p.fn.nvCreateOpticalFlowD3D11(p.dev.Get(), p.ctx.Get(), &hRaw);
        if (st == NV_OF_SUCCESS && hRaw) {
            std::vector<uint32_t> v;
            NV_OF_STATUS qs = NV_OF_SUCCESS;
            const bool ok = QueryCap(p, hRaw, NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE, &v, &qs);
            Line("  SUPPORT_HINT_WITH_OF_MODE on an uninitialised session -> %s%s",
                 StatusName(qs), (ok && !v.empty()) ? "" : " (no value)");
            if (ok && !v.empty()) Line("    value %u", v[0]);
            if (p.fn.nvOFDestroy) p.fn.nvOFDestroy(hRaw);
        } else {
            Line("  nvCreateOpticalFlowD3D11 -> %s (skipped)", StatusName(st));
        }
    }

    TestHintSessions(p);

    if (doExec) {
        uint32_t g = 0;
        for (uint32_t cand : {4u, 8u})
            if (p.initTried[cand] && p.initStatus[cand] == NV_OF_SUCCESS) { g = cand; break; }
        if (g) TryExecuteWithHints(p, g);
        else Line("\n--- execute skipped: no hint grid size >= outGridSize initialised ---");
    }

    Verdict(p);
    return 0;
}
