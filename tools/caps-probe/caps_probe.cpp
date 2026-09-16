
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <wrl/client.h>
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <comdef.h>

#if __has_include(<Presentation.h>)
#define HAS_PRESENTATION_H 1
#include <Presentation.h>
#endif

// The feature-guard names below are ENUMERATORS, not macros, so a plain `#ifdef` on
// them is always false and silently deletes the block. That is what emptied sections
// 4, 5 and 9 in the first revision. Self-referential defines make the guards evaluate
// true while leaving the token itself resolving to the enumerator (the preprocessor
// will not re-expand a macro inside its own expansion). Each one is wrapped in
// __has_include-independent compilation: if the SDK really lacks the enumerator, the
// build fails loudly here instead of the probe lying by omission.
#define D3D11_FEATURE_D3D11_OPTIONS2 D3D11_FEATURE_D3D11_OPTIONS2
#define D3D11_FEATURE_FORMAT_SUPPORT2 D3D11_FEATURE_FORMAT_SUPPORT2
#define D3D11_FEATURE_DISPLAYABLE D3D11_FEATURE_DISPLAYABLE
#define D3D12_FEATURE_D3D12_OPTIONS D3D12_FEATURE_D3D12_OPTIONS
#define D3D12_FEATURE_D3D12_OPTIONS1 D3D12_FEATURE_D3D12_OPTIONS1
#define D3D12_FEATURE_D3D12_OPTIONS2 D3D12_FEATURE_D3D12_OPTIONS2
#define D3D12_FEATURE_D3D12_OPTIONS3 D3D12_FEATURE_D3D12_OPTIONS3
#define D3D12_FEATURE_D3D12_OPTIONS4 D3D12_FEATURE_D3D12_OPTIONS4
#define D3D12_FEATURE_D3D12_OPTIONS5 D3D12_FEATURE_D3D12_OPTIONS5
#define D3D12_FEATURE_D3D12_OPTIONS6 D3D12_FEATURE_D3D12_OPTIONS6
#define D3D12_FEATURE_D3D12_OPTIONS7 D3D12_FEATURE_D3D12_OPTIONS7
#define D3D12_FEATURE_D3D12_OPTIONS8 D3D12_FEATURE_D3D12_OPTIONS8
#define D3D12_FEATURE_D3D12_OPTIONS9 D3D12_FEATURE_D3D12_OPTIONS9
#define D3D12_FEATURE_D3D12_OPTIONS10 D3D12_FEATURE_D3D12_OPTIONS10
#define D3D12_FEATURE_D3D12_OPTIONS11 D3D12_FEATURE_D3D12_OPTIONS11
#define D3D12_FEATURE_D3D12_OPTIONS12 D3D12_FEATURE_D3D12_OPTIONS12
#define D3D12_FEATURE_D3D12_OPTIONS13 D3D12_FEATURE_D3D12_OPTIONS13
#define D3D12_FEATURE_D3D12_OPTIONS14 D3D12_FEATURE_D3D12_OPTIONS14
#define D3D12_FEATURE_D3D12_OPTIONS15 D3D12_FEATURE_D3D12_OPTIONS15
#define D3D12_FEATURE_D3D12_OPTIONS16 D3D12_FEATURE_D3D12_OPTIONS16
#define D3D12_FEATURE_D3D12_OPTIONS17 D3D12_FEATURE_D3D12_OPTIONS17
#define D3D12_FEATURE_D3D12_OPTIONS18 D3D12_FEATURE_D3D12_OPTIONS18
#define D3D12_FEATURE_D3D12_OPTIONS19 D3D12_FEATURE_D3D12_OPTIONS19
#define D3D12_FEATURE_D3D12_OPTIONS20 D3D12_FEATURE_D3D12_OPTIONS20
#define D3D12_FEATURE_D3D12_OPTIONS21 D3D12_FEATURE_D3D12_OPTIONS21
#define D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR
#define D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE

using Microsoft::WRL::ComPtr;

void ProbeAdapterAndOutputs(ComPtr<IDXGIAdapter3>& outAdapter) {
    std::cout << "\n=== 1. ADAPTER ===\n";
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::cout << "UNSUPPORTED: Failed to create IDXGIFactory6\n";
        return;
    }

    ComPtr<IDXGIAdapter1> adapter1;
    HRESULT hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter1));
    if (FAILED(hr)) {
        std::cout << "UNSUPPORTED: 0x" << std::hex << hr << std::dec << " EnumAdapterByGpuPreference failed\n";
        return;
    }

    ComPtr<IDXGIAdapter3> adapter3;
    adapter1.As(&adapter3);
    if (!adapter3) {
        std::cout << "UNSUPPORTED: IDXGIAdapter3 not supported on chosen adapter\n";
        return;
    }
    outAdapter = adapter3;

    DXGI_ADAPTER_DESC1 desc;
    adapter3->GetDesc1(&desc);
    std::wcout << L"  Description: " << desc.Description << L"\n";
    std::cout << "  VendorId: 0x" << std::hex << desc.VendorId << std::dec << "\n";
    std::cout << "  DeviceId: 0x" << std::hex << desc.DeviceId << std::dec << "\n";
    std::cout << "  DedicatedVideoMemory: " << desc.DedicatedVideoMemory << "\n";
    std::cout << "  SharedSystemMemory: " << desc.SharedSystemMemory << "\n";

    DXGI_QUERY_VIDEO_MEMORY_INFO localMemInfo = {};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localMemInfo);
    std::cout << "  LOCAL Memory Info:\n";
    std::cout << "    Budget: " << localMemInfo.Budget << "\n";
    std::cout << "    CurrentUsage: " << localMemInfo.CurrentUsage << "\n";
    std::cout << "    AvailableForReservation: " << localMemInfo.AvailableForReservation << "\n";
    std::cout << "    CurrentReservation: " << localMemInfo.CurrentReservation << "\n";

    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMemInfo = {};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalMemInfo);
    std::cout << "  NON_LOCAL Memory Info:\n";
    std::cout << "    Budget: " << nonLocalMemInfo.Budget << "\n";
    std::cout << "    CurrentUsage: " << nonLocalMemInfo.CurrentUsage << "\n";
    std::cout << "    AvailableForReservation: " << nonLocalMemInfo.AvailableForReservation << "\n";
    std::cout << "    CurrentReservation: " << nonLocalMemInfo.CurrentReservation << "\n";

    std::cout << "\n=== 2. OUTPUTS ===\n";
    UINT i = 0;
    ComPtr<IDXGIOutput> output;
    while (adapter3->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC outDesc;
        output->GetDesc(&outDesc);
        std::wcout << L"  Output " << i << L" DeviceName: " << outDesc.DeviceName << L"\n";
        std::cout << "    DesktopCoordinates: " << outDesc.DesktopCoordinates.left << "," << outDesc.DesktopCoordinates.top 
                  << " to " << outDesc.DesktopCoordinates.right << "," << outDesc.DesktopCoordinates.bottom << "\n";
        std::cout << "    AttachedToDesktop: " << outDesc.AttachedToDesktop << "\n";
        std::cout << "    Rotation: " << outDesc.Rotation << "\n";

        ComPtr<IDXGIOutput6> output6;
        if (SUCCEEDED(output.As(&output6))) {
            DXGI_OUTPUT_DESC1 outDesc1;
            if (SUCCEEDED(output6->GetDesc1(&outDesc1))) {
                std::cout << "    BitsPerColor: " << outDesc1.BitsPerColor << "\n";
                std::cout << "    ColorSpace: " << outDesc1.ColorSpace << "\n";
                std::cout << "    RedPrimary: " << outDesc1.RedPrimary[0] << "," << outDesc1.RedPrimary[1] << "\n";
                std::cout << "    GreenPrimary: " << outDesc1.GreenPrimary[0] << "," << outDesc1.GreenPrimary[1] << "\n";
                std::cout << "    BluePrimary: " << outDesc1.BluePrimary[0] << "," << outDesc1.BluePrimary[1] << "\n";
                std::cout << "    WhitePoint: " << outDesc1.WhitePoint[0] << "," << outDesc1.WhitePoint[1] << "\n";
                std::cout << "    MinLuminance: " << outDesc1.MinLuminance << "\n";
                std::cout << "    MaxLuminance: " << outDesc1.MaxLuminance << "\n";
                std::cout << "    MaxFullFrameLuminance: " << outDesc1.MaxFullFrameLuminance << "\n";
            }

            UINT flags = 0;
            if (SUCCEEDED(output6->CheckHardwareCompositionSupport(&flags))) {
                std::cout << "    Hardware Composition Flags:\n";
                if (flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN) std::cout << "      FULLSCREEN\n";
                if (flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED) std::cout << "      WINDOWED\n";
                if (flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED) std::cout << "      CURSOR_STRETCHED\n";
            } else {
                std::cout << "    UNSUPPORTED: CheckHardwareCompositionSupport\n";
            }
        }
        i++;
    }
}

void ProbeD3D11(ComPtr<IDXGIAdapter> adapter, ComPtr<ID3D11Device>& outDevice) {
    std::cout << "\n=== 3. D3D11 DEVICE ===\n";
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(),
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        nullptr
    );
    if (FAILED(hr)) {
        std::cout << "UNSUPPORTED: 0x" << std::hex << hr << std::dec << " D3D11CreateDevice failed\n";
        return;
    }
    outDevice = device;
    std::cout << "  D3D11 Feature Level: 0x" << std::hex << featureLevel << std::dec << "\n";

    D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))) {
        std::cout << "  D3D11_FEATURE_D3D11_OPTIONS:\n";
        std::cout << "    OutputMergerLogicOp: " << (uint64_t)options.OutputMergerLogicOp << "\n";
        std::cout << "    UAVOnlyRenderingForcedSampleCount: " << (uint64_t)options.UAVOnlyRenderingForcedSampleCount << "\n";
        std::cout << "    DiscardAPIsSeenByDriver: " << (uint64_t)options.DiscardAPIsSeenByDriver << "\n";
        std::cout << "    FlagsForUpdateAndCopySeenByDriver: " << (uint64_t)options.FlagsForUpdateAndCopySeenByDriver << "\n";
        std::cout << "    ClearView: " << (uint64_t)options.ClearView << "\n";
        std::cout << "    CopyWithOverlap: " << (uint64_t)options.CopyWithOverlap << "\n";
        std::cout << "    ConstantBufferPartialUpdate: " << (uint64_t)options.ConstantBufferPartialUpdate << "\n";
        std::cout << "    ConstantBufferOffsetting: " << (uint64_t)options.ConstantBufferOffsetting << "\n";
        std::cout << "    MapNoOverwriteOnDynamicConstantBuffer: " << (uint64_t)options.MapNoOverwriteOnDynamicConstantBuffer << "\n";
        std::cout << "    MapNoOverwriteOnDynamicBufferSRV: " << (uint64_t)options.MapNoOverwriteOnDynamicBufferSRV << "\n";
        std::cout << "    MultisampleRTVWithForcedSampleCountOne: " << (uint64_t)options.MultisampleRTVWithForcedSampleCountOne << "\n";
        std::cout << "    SAD4ShaderInstructions: " << (uint64_t)options.SAD4ShaderInstructions << "\n";
        std::cout << "    ExtendedDoublesShaderInstructions: " << (uint64_t)options.ExtendedDoublesShaderInstructions << "\n";
        std::cout << "    ExtendedResourceSharing: " << (uint64_t)options.ExtendedResourceSharing << "\n";
    }

#if defined(D3D11_FEATURE_D3D11_OPTIONS2) || (D3D11_SDK_VERSION >= 7) // actually just ifdef D3D11_FEATURE_D3D11_OPTIONS2 should work if d3d11_3.h is included
    // But let's check definition properly.
#endif

    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2)))) {
        std::cout << "  D3D11_FEATURE_D3D11_OPTIONS2:\n";
        std::cout << "    PSSpecifiedStencilRefSupported: " << (uint64_t)options2.PSSpecifiedStencilRefSupported << "\n";
        std::cout << "    TypedUAVLoadAdditionalFormats: " << (uint64_t)options2.TypedUAVLoadAdditionalFormats << "\n";
        std::cout << "    ROVsSupported: " << (uint64_t)options2.ROVsSupported << "\n";
        std::cout << "    ConservativeRasterizationTier: " << (uint64_t)options2.ConservativeRasterizationTier << "\n";
        std::cout << "    TiledResourcesTier: " << (uint64_t)options2.TiledResourcesTier << "\n";
        std::cout << "    MapOnDefaultTextures: " << (uint64_t)options2.MapOnDefaultTextures << "\n";
        std::cout << "    StandardSwizzle: " << (uint64_t)options2.StandardSwizzle << "\n";
        std::cout << "    UnifiedMemoryArchitecture: " << (uint64_t)options2.UnifiedMemoryArchitecture << "\n";
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof(options3)))) {
        std::cout << "  D3D11_FEATURE_D3D11_OPTIONS3:\n";
        std::cout << "    VPAndRTArrayIndexFromAnyShaderFeedingRasterizer: " << (uint64_t)options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer << "\n";
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS4 options4 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS4, &options4, sizeof(options4)))) {
        std::cout << "  D3D11_FEATURE_D3D11_OPTIONS4:\n";
        std::cout << "    ExtendedNV12SharedTextureSupported: " << (uint64_t)options4.ExtendedNV12SharedTextureSupported << "\n";
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS5 options5 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5, &options5, sizeof(options5)))) {
        std::cout << "  D3D11_FEATURE_D3D11_OPTIONS5:\n";
        std::cout << "    SharedResourceTier: " << (uint64_t)options5.SharedResourceTier << "\n";
    }

#if defined(D3D11_FEATURE_DISPLAYABLE)
    std::cout << "  *** D3D11_FEATURE_DISPLAYABLE ***\n";
    D3D11_FEATURE_DATA_DISPLAYABLE displayable = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_DISPLAYABLE, &displayable, sizeof(displayable)))) {
        std::cout << "    DisplayableTexture: " << (uint64_t)displayable.DisplayableTexture << "\n";
        std::cout << "    SharedResourceTier: " << (uint64_t)displayable.SharedResourceTier << "\n";
    } else {
        std::cout << "    UNSUPPORTED: CheckFeatureSupport(D3D11_FEATURE_DISPLAYABLE)\n";
    }
    // The P1 presenter needs MISC_SHARED_DISPLAYABLE and BIND_UNORDERED_ACCESS on the SAME
    // texture, so the synthesis shader can write the buffer that is scanned out. Nothing
    // documents whether that combination is legal; create one and find out.
    {
        const DXGI_FORMAT dfmt[] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                     DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
        const char* dname[] = { "B8G8R8A8_UNORM", "R8G8B8A8_UNORM", "R10G10B10A2_UNORM", "R16G16B16A16_FLOAT" };
        for (int i = 0; i < 4; ++i) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = 2560; td.Height = 1440; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = dfmt[i]; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE | D3D11_RESOURCE_MISC_SHARED |
                           D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            ComPtr<ID3D11Texture2D> tex;
            HRESULT hrT = device->CreateTexture2D(&td, nullptr, &tex);
            std::cout << "    displayable+UAV 2560x1440 " << dname[i] << ": CreateTexture2D=0x"
                      << std::hex << hrT << std::dec;
            if (SUCCEEDED(hrT)) {
                ComPtr<ID3D11UnorderedAccessView> uav;
                std::cout << " CreateUAV=0x" << std::hex
                          << device->CreateUnorderedAccessView(tex.Get(), nullptr, &uav) << std::dec;
            }
            std::cout << "\n";
        }
    }
#endif

    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT minPrecision = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &minPrecision, sizeof(minPrecision)))) {
        std::cout << "  D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT:\n";
        std::cout << "    PixelShaderMinPrecision: " << (uint64_t)minPrecision.PixelShaderMinPrecision << "\n";
        std::cout << "    AllOtherShaderStagesMinPrecision: " << (uint64_t)minPrecision.AllOtherShaderStagesMinPrecision << "\n";
    }

    DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_NV12, DXGI_FORMAT_P010
    };
    const char* formatNames[] = {
        "B8G8R8A8_UNORM", "R8G8B8A8_UNORM", "R10G10B10A2_UNORM",
        "R16G16B16A16_FLOAT", "R11G11B10_FLOAT", "NV12", "P010"
    };

    for (int i = 0; i < _countof(formats); i++) {
        UINT support = 0;
        if (SUCCEEDED(device->CheckFormatSupport(formats[i], &support))) {
            std::cout << "  Format " << formatNames[i] << " support:\n";
            if (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) std::cout << "    TYPED_UNORDERED_ACCESS_VIEW\n";
            if (support & D3D11_FORMAT_SUPPORT_DISPLAY) std::cout << "    DISPLAY\n";
            if (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) std::cout << "    RENDER_TARGET\n";
            if (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) std::cout << "    SHADER_SAMPLE\n";
        }
#ifdef D3D11_FEATURE_FORMAT_SUPPORT2
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2 = { formats[i], 0 };
        HRESULT hr2 = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2));
        std::cout << "    FORMAT_SUPPORT2 hr=0x" << std::hex << hr2
                  << " bits=0x" << support2.OutFormatSupport2 << std::dec << "\n";
        if (SUCCEEDED(hr2)) {
            if (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) std::cout << "    UAV_TYPED_STORE\n";
            if (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) std::cout << "    UAV_TYPED_LOAD\n";
            if (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD) std::cout << "    UAV_ATOMIC_ADD\n";
        }
#endif
    }
}

void ProbeD3D12(ComPtr<IDXGIAdapter> adapter, ComPtr<ID3D12Device>& outDevice) {
    std::cout << "\n=== 4. D3D12 DEVICE ===\n";
    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::cout << "UNSUPPORTED: 0x" << std::hex << hr << std::dec << " D3D12CreateDevice failed\n";
        return;
    }
    outDevice = device;

    // Feature level
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
    fl.NumFeatureLevels = _countof(levels);
    fl.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl)))) {
        std::cout << "  Highest D3D12 Feature Level: 0x" << std::hex << fl.MaxSupportedFeatureLevel << std::dec << "\n";
    }

    // Shader model
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_9 };
    // Probe downwards
    for (int v = 9; v >= 0; v--) {
        sm.HighestShaderModel = (D3D_SHADER_MODEL)(0x60 + v);
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            std::cout << "  Highest Shader Model: 0x" << std::hex << sm.HighestShaderModel << std::dec << "\n";
            break;
        }
    }


#ifdef D3D12_FEATURE_D3D12_OPTIONS
    D3D12_FEATURE_DATA_D3D12_OPTIONS d3d12_feature_data_d3d12_options = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &d3d12_feature_data_d3d12_options, sizeof(d3d12_feature_data_d3d12_options)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS:\n";
        std::cout << "    DoublePrecisionFloatShaderOps: " << (uint64_t)d3d12_feature_data_d3d12_options.DoublePrecisionFloatShaderOps << "\n";
        std::cout << "    OutputMergerLogicOp: " << (uint64_t)d3d12_feature_data_d3d12_options.OutputMergerLogicOp << "\n";
        std::cout << "    MinPrecisionSupport: " << (uint64_t)d3d12_feature_data_d3d12_options.MinPrecisionSupport << "\n";
        std::cout << "    TiledResourcesTier: " << (uint64_t)d3d12_feature_data_d3d12_options.TiledResourcesTier << "\n";
        std::cout << "    ResourceBindingTier: " << (uint64_t)d3d12_feature_data_d3d12_options.ResourceBindingTier << "\n";
        std::cout << "    PSSpecifiedStencilRefSupported: " << (uint64_t)d3d12_feature_data_d3d12_options.PSSpecifiedStencilRefSupported << "\n";
        std::cout << "    TypedUAVLoadAdditionalFormats: " << (uint64_t)d3d12_feature_data_d3d12_options.TypedUAVLoadAdditionalFormats << "\n";
        std::cout << "    ROVsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options.ROVsSupported << "\n";
        std::cout << "    ConservativeRasterizationTier: " << (uint64_t)d3d12_feature_data_d3d12_options.ConservativeRasterizationTier << "\n";
        std::cout << "    MaxGPUVirtualAddressBitsPerResource: " << (uint64_t)d3d12_feature_data_d3d12_options.MaxGPUVirtualAddressBitsPerResource << "\n";
        std::cout << "    StandardSwizzle64KBSupported: " << (uint64_t)d3d12_feature_data_d3d12_options.StandardSwizzle64KBSupported << "\n";
        std::cout << "    CrossNodeSharingTier: " << (uint64_t)d3d12_feature_data_d3d12_options.CrossNodeSharingTier << "\n";
        std::cout << "    CrossAdapterRowMajorTextureSupported: " << (uint64_t)d3d12_feature_data_d3d12_options.CrossAdapterRowMajorTextureSupported << "\n";
        std::cout << "    VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation: " << (uint64_t)d3d12_feature_data_d3d12_options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation << "\n";
        std::cout << "    ResourceHeapTier: " << (uint64_t)d3d12_feature_data_d3d12_options.ResourceHeapTier << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS1
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 d3d12_feature_data_d3d12_options1 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &d3d12_feature_data_d3d12_options1, sizeof(d3d12_feature_data_d3d12_options1)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS1:\n";
        std::cout << "    WaveOps: " << (uint64_t)d3d12_feature_data_d3d12_options1.WaveOps << "\n";
        std::cout << "    WaveLaneCountMin: " << (uint64_t)d3d12_feature_data_d3d12_options1.WaveLaneCountMin << "\n";
        std::cout << "    WaveLaneCountMax: " << (uint64_t)d3d12_feature_data_d3d12_options1.WaveLaneCountMax << "\n";
        std::cout << "    TotalLaneCount: " << (uint64_t)d3d12_feature_data_d3d12_options1.TotalLaneCount << "\n";
        std::cout << "    ExpandedComputeResourceStates: " << (uint64_t)d3d12_feature_data_d3d12_options1.ExpandedComputeResourceStates << "\n";
        std::cout << "    Int64ShaderOps: " << (uint64_t)d3d12_feature_data_d3d12_options1.Int64ShaderOps << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS2
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 d3d12_feature_data_d3d12_options2 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &d3d12_feature_data_d3d12_options2, sizeof(d3d12_feature_data_d3d12_options2)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS2:\n";
        std::cout << "    DepthBoundsTestSupported: " << (uint64_t)d3d12_feature_data_d3d12_options2.DepthBoundsTestSupported << "\n";
        std::cout << "    ProgrammableSamplePositionsTier: " << (uint64_t)d3d12_feature_data_d3d12_options2.ProgrammableSamplePositionsTier << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS3
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 d3d12_feature_data_d3d12_options3 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &d3d12_feature_data_d3d12_options3, sizeof(d3d12_feature_data_d3d12_options3)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS3:\n";
        std::cout << "    CopyQueueTimestampQueriesSupported: " << (uint64_t)d3d12_feature_data_d3d12_options3.CopyQueueTimestampQueriesSupported << "\n";
        std::cout << "    CastingFullyTypedFormatSupported: " << (uint64_t)d3d12_feature_data_d3d12_options3.CastingFullyTypedFormatSupported << "\n";
        std::cout << "    WriteBufferImmediateSupportFlags: " << (uint64_t)d3d12_feature_data_d3d12_options3.WriteBufferImmediateSupportFlags << "\n";
        std::cout << "    ViewInstancingTier: " << (uint64_t)d3d12_feature_data_d3d12_options3.ViewInstancingTier << "\n";
        std::cout << "    BarycentricsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options3.BarycentricsSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS4
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 d3d12_feature_data_d3d12_options4 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &d3d12_feature_data_d3d12_options4, sizeof(d3d12_feature_data_d3d12_options4)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS4:\n";
        std::cout << "    MSAA64KBAlignedTextureSupported: " << (uint64_t)d3d12_feature_data_d3d12_options4.MSAA64KBAlignedTextureSupported << "\n";
        std::cout << "    SharedResourceCompatibilityTier: " << (uint64_t)d3d12_feature_data_d3d12_options4.SharedResourceCompatibilityTier << "\n";
        std::cout << "    Native16BitShaderOpsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options4.Native16BitShaderOpsSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS5
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 d3d12_feature_data_d3d12_options5 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &d3d12_feature_data_d3d12_options5, sizeof(d3d12_feature_data_d3d12_options5)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS5:\n";
        std::cout << "    SRVOnlyTiledResourceTier3: " << (uint64_t)d3d12_feature_data_d3d12_options5.SRVOnlyTiledResourceTier3 << "\n";
        std::cout << "    RenderPassesTier: " << (uint64_t)d3d12_feature_data_d3d12_options5.RenderPassesTier << "\n";
        std::cout << "    RaytracingTier: " << (uint64_t)d3d12_feature_data_d3d12_options5.RaytracingTier << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS6
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 d3d12_feature_data_d3d12_options6 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &d3d12_feature_data_d3d12_options6, sizeof(d3d12_feature_data_d3d12_options6)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS6:\n";
        std::cout << "    AdditionalShadingRatesSupported: " << (uint64_t)d3d12_feature_data_d3d12_options6.AdditionalShadingRatesSupported << "\n";
        std::cout << "    PerPrimitiveShadingRateSupportedWithViewportIndexing: " << (uint64_t)d3d12_feature_data_d3d12_options6.PerPrimitiveShadingRateSupportedWithViewportIndexing << "\n";
        std::cout << "    VariableShadingRateTier: " << (uint64_t)d3d12_feature_data_d3d12_options6.VariableShadingRateTier << "\n";
        std::cout << "    ShadingRateImageTileSize: " << (uint64_t)d3d12_feature_data_d3d12_options6.ShadingRateImageTileSize << "\n";
        std::cout << "    BackgroundProcessingSupported: " << (uint64_t)d3d12_feature_data_d3d12_options6.BackgroundProcessingSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS7
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 d3d12_feature_data_d3d12_options7 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &d3d12_feature_data_d3d12_options7, sizeof(d3d12_feature_data_d3d12_options7)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS7:\n";
        std::cout << "    MeshShaderTier: " << (uint64_t)d3d12_feature_data_d3d12_options7.MeshShaderTier << "\n";
        std::cout << "    SamplerFeedbackTier: " << (uint64_t)d3d12_feature_data_d3d12_options7.SamplerFeedbackTier << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS8
    D3D12_FEATURE_DATA_D3D12_OPTIONS8 d3d12_feature_data_d3d12_options8 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &d3d12_feature_data_d3d12_options8, sizeof(d3d12_feature_data_d3d12_options8)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS8:\n";
        std::cout << "    UnalignedBlockTexturesSupported: " << (uint64_t)d3d12_feature_data_d3d12_options8.UnalignedBlockTexturesSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS9
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 d3d12_feature_data_d3d12_options9 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &d3d12_feature_data_d3d12_options9, sizeof(d3d12_feature_data_d3d12_options9)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS9:\n";
        std::cout << "    MeshShaderPipelineStatsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options9.MeshShaderPipelineStatsSupported << "\n";
        std::cout << "    MeshShaderSupportsFullRangeRenderTargetArrayIndex: " << (uint64_t)d3d12_feature_data_d3d12_options9.MeshShaderSupportsFullRangeRenderTargetArrayIndex << "\n";
        std::cout << "    AtomicInt64OnTypedResourceSupported: " << (uint64_t)d3d12_feature_data_d3d12_options9.AtomicInt64OnTypedResourceSupported << "\n";
        std::cout << "    AtomicInt64OnGroupSharedSupported: " << (uint64_t)d3d12_feature_data_d3d12_options9.AtomicInt64OnGroupSharedSupported << "\n";
        std::cout << "    DerivativesInMeshAndAmplificationShadersSupported: " << (uint64_t)d3d12_feature_data_d3d12_options9.DerivativesInMeshAndAmplificationShadersSupported << "\n";
        std::cout << "    WaveMMATier: " << (uint64_t)d3d12_feature_data_d3d12_options9.WaveMMATier << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS10
    D3D12_FEATURE_DATA_D3D12_OPTIONS10 d3d12_feature_data_d3d12_options10 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS10, &d3d12_feature_data_d3d12_options10, sizeof(d3d12_feature_data_d3d12_options10)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS10:\n";
        std::cout << "    VariableRateShadingSumCombinerSupported: " << (uint64_t)d3d12_feature_data_d3d12_options10.VariableRateShadingSumCombinerSupported << "\n";
        std::cout << "    MeshShaderPerPrimitiveShadingRateSupported: " << (uint64_t)d3d12_feature_data_d3d12_options10.MeshShaderPerPrimitiveShadingRateSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS11
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 d3d12_feature_data_d3d12_options11 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &d3d12_feature_data_d3d12_options11, sizeof(d3d12_feature_data_d3d12_options11)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS11:\n";
        std::cout << "    AtomicInt64OnDescriptorHeapResourceSupported: " << (uint64_t)d3d12_feature_data_d3d12_options11.AtomicInt64OnDescriptorHeapResourceSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS12
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 d3d12_feature_data_d3d12_options12 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &d3d12_feature_data_d3d12_options12, sizeof(d3d12_feature_data_d3d12_options12)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS12:\n";
        std::cout << "    MSPrimitivesPipelineStatisticIncludesCulledPrimitives: " << (uint64_t)d3d12_feature_data_d3d12_options12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives << "\n";
        std::cout << "    EnhancedBarriersSupported: " << (uint64_t)d3d12_feature_data_d3d12_options12.EnhancedBarriersSupported << "\n";
        std::cout << "    RelaxedFormatCastingSupported: " << (uint64_t)d3d12_feature_data_d3d12_options12.RelaxedFormatCastingSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS13
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 d3d12_feature_data_d3d12_options13 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &d3d12_feature_data_d3d12_options13, sizeof(d3d12_feature_data_d3d12_options13)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS13:\n";
        std::cout << "    UnrestrictedBufferTextureCopyPitchSupported: " << (uint64_t)d3d12_feature_data_d3d12_options13.UnrestrictedBufferTextureCopyPitchSupported << "\n";
        std::cout << "    UnrestrictedVertexElementAlignmentSupported: " << (uint64_t)d3d12_feature_data_d3d12_options13.UnrestrictedVertexElementAlignmentSupported << "\n";
        std::cout << "    InvertedViewportHeightFlipsYSupported: " << (uint64_t)d3d12_feature_data_d3d12_options13.InvertedViewportHeightFlipsYSupported << "\n";
        std::cout << "    InvertedViewportDepthFlipsZSupported: " << (uint64_t)d3d12_feature_data_d3d12_options13.InvertedViewportDepthFlipsZSupported << "\n";
        std::cout << "    TextureCopyBetweenDimensionsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options13.TextureCopyBetweenDimensionsSupported << "\n";
        std::cout << "    AlphaBlendFactorSupported: " << (uint64_t)d3d12_feature_data_d3d12_options13.AlphaBlendFactorSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS14
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 d3d12_feature_data_d3d12_options14 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &d3d12_feature_data_d3d12_options14, sizeof(d3d12_feature_data_d3d12_options14)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS14:\n";
        std::cout << "    AdvancedTextureOpsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options14.AdvancedTextureOpsSupported << "\n";
        std::cout << "    WriteableMSAATexturesSupported: " << (uint64_t)d3d12_feature_data_d3d12_options14.WriteableMSAATexturesSupported << "\n";
        std::cout << "    IndependentFrontAndBackStencilRefMaskSupported: " << (uint64_t)d3d12_feature_data_d3d12_options14.IndependentFrontAndBackStencilRefMaskSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS15
    D3D12_FEATURE_DATA_D3D12_OPTIONS15 d3d12_feature_data_d3d12_options15 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS15, &d3d12_feature_data_d3d12_options15, sizeof(d3d12_feature_data_d3d12_options15)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS15:\n";
        std::cout << "    TriangleFanSupported: " << (uint64_t)d3d12_feature_data_d3d12_options15.TriangleFanSupported << "\n";
        std::cout << "    DynamicIndexBufferStripCutSupported: " << (uint64_t)d3d12_feature_data_d3d12_options15.DynamicIndexBufferStripCutSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS16
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 d3d12_feature_data_d3d12_options16 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &d3d12_feature_data_d3d12_options16, sizeof(d3d12_feature_data_d3d12_options16)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS16:\n";
        std::cout << "    DynamicDepthBiasSupported: " << (uint64_t)d3d12_feature_data_d3d12_options16.DynamicDepthBiasSupported << "\n";
        std::cout << "    GPUUploadHeapSupported: " << (uint64_t)d3d12_feature_data_d3d12_options16.GPUUploadHeapSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS17
    D3D12_FEATURE_DATA_D3D12_OPTIONS17 d3d12_feature_data_d3d12_options17 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS17, &d3d12_feature_data_d3d12_options17, sizeof(d3d12_feature_data_d3d12_options17)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS17:\n";
        std::cout << "    NonNormalizedCoordinateSamplersSupported: " << (uint64_t)d3d12_feature_data_d3d12_options17.NonNormalizedCoordinateSamplersSupported << "\n";
        std::cout << "    ManualWriteTrackingResourceSupported: " << (uint64_t)d3d12_feature_data_d3d12_options17.ManualWriteTrackingResourceSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS18
    D3D12_FEATURE_DATA_D3D12_OPTIONS18 d3d12_feature_data_d3d12_options18 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS18, &d3d12_feature_data_d3d12_options18, sizeof(d3d12_feature_data_d3d12_options18)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS18:\n";
        std::cout << "    RenderPassesValid: " << (uint64_t)d3d12_feature_data_d3d12_options18.RenderPassesValid << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS19
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 d3d12_feature_data_d3d12_options19 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &d3d12_feature_data_d3d12_options19, sizeof(d3d12_feature_data_d3d12_options19)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS19:\n";
        std::cout << "    MismatchingOutputDimensionsSupported: " << (uint64_t)d3d12_feature_data_d3d12_options19.MismatchingOutputDimensionsSupported << "\n";
        std::cout << "    SupportedSampleCountsWithNoOutputs: " << (uint64_t)d3d12_feature_data_d3d12_options19.SupportedSampleCountsWithNoOutputs << "\n";
        std::cout << "    PointSamplingAddressesNeverRoundUp: " << (uint64_t)d3d12_feature_data_d3d12_options19.PointSamplingAddressesNeverRoundUp << "\n";
        std::cout << "    RasterizerDesc2Supported: " << (uint64_t)d3d12_feature_data_d3d12_options19.RasterizerDesc2Supported << "\n";
        std::cout << "    NarrowQuadrilateralLinesSupported: " << (uint64_t)d3d12_feature_data_d3d12_options19.NarrowQuadrilateralLinesSupported << "\n";
        std::cout << "    AnisoFilterWithPointMipSupported: " << (uint64_t)d3d12_feature_data_d3d12_options19.AnisoFilterWithPointMipSupported << "\n";
        std::cout << "    MaxSamplerDescriptorHeapSize: " << (uint64_t)d3d12_feature_data_d3d12_options19.MaxSamplerDescriptorHeapSize << "\n";
        std::cout << "    MaxSamplerDescriptorHeapSizeWithStaticSamplers: " << (uint64_t)d3d12_feature_data_d3d12_options19.MaxSamplerDescriptorHeapSizeWithStaticSamplers << "\n";
        std::cout << "    MaxViewDescriptorHeapSize: " << (uint64_t)d3d12_feature_data_d3d12_options19.MaxViewDescriptorHeapSize << "\n";
        std::cout << "    ComputeOnlyCustomHeapSupported: " << (uint64_t)d3d12_feature_data_d3d12_options19.ComputeOnlyCustomHeapSupported << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS20
    D3D12_FEATURE_DATA_D3D12_OPTIONS20 d3d12_feature_data_d3d12_options20 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS20, &d3d12_feature_data_d3d12_options20, sizeof(d3d12_feature_data_d3d12_options20)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS20:\n";
        std::cout << "    ComputeOnlyWriteWatchSupported: " << (uint64_t)d3d12_feature_data_d3d12_options20.ComputeOnlyWriteWatchSupported << "\n";
        std::cout << "    RecreateAtTier: " << (uint64_t)d3d12_feature_data_d3d12_options20.RecreateAtTier << "\n";
    }
#endif

#ifdef D3D12_FEATURE_D3D12_OPTIONS21
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 d3d12_feature_data_d3d12_options21 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &d3d12_feature_data_d3d12_options21, sizeof(d3d12_feature_data_d3d12_options21)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS21:\n";
        std::cout << "    WorkGraphsTier: " << (uint64_t)d3d12_feature_data_d3d12_options21.WorkGraphsTier << "\n";
        std::cout << "    ExecuteIndirectTier: " << (uint64_t)d3d12_feature_data_d3d12_options21.ExecuteIndirectTier << "\n";
        std::cout << "    SampleCmpGradientAndBiasSupported: " << (uint64_t)d3d12_feature_data_d3d12_options21.SampleCmpGradientAndBiasSupported << "\n";
        std::cout << "    ExtendedCommandInfoSupported: " << (uint64_t)d3d12_feature_data_d3d12_options21.ExtendedCommandInfoSupported << "\n";
    }
#endif

    D3D12_FEATURE_DATA_ARCHITECTURE1 arch1 = { 0 };
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch1, sizeof(arch1)))) {
        std::cout << "  D3D12_FEATURE_ARCHITECTURE1:\n";
        std::cout << "    NodeIndex: " << (uint64_t)arch1.NodeIndex << "\n";
        std::cout << "    TileBasedRenderer: " << (uint64_t)arch1.TileBasedRenderer << "\n";
        std::cout << "    UMA: " << (uint64_t)arch1.UMA << "\n";
        std::cout << "    CacheCoherentUMA: " << (uint64_t)arch1.CacheCoherentUMA << "\n";
        std::cout << "    IsolatedMMU: " << (uint64_t)arch1.IsolatedMMU << "\n";
    }

#ifdef D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL
    D3D12_FEATURE_DATA_D3D12_OPTIONS_EXPERIMENTAL exp = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL, &exp, sizeof(exp)))) {
        std::cout << "  D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL supported.\n";
    }
#endif
}

void ProbeD3D12Video(ComPtr<ID3D12Device> device) {
    std::cout << "\n=== 5. D3D12 VIDEO ===\n";
    ComPtr<ID3D12VideoDevice> videoDevice;
    HRESULT hrVD = device.As(&videoDevice);
    std::cout << "  QueryInterface ID3D12VideoDevice:  0x" << std::hex << hrVD << std::dec << "\n";
    ComPtr<ID3D12VideoDevice1> videoDevice1;
    std::cout << "  QueryInterface ID3D12VideoDevice1: 0x" << std::hex << device.As(&videoDevice1) << std::dec << "\n";
    ComPtr<ID3D12VideoDevice2> videoDevice2;
    std::cout << "  QueryInterface ID3D12VideoDevice2: 0x" << std::hex << device.As(&videoDevice2) << std::dec << "\n";
    if (FAILED(hrVD)) { std::cout << "  -> no video device, section ends here\n"; return; }

    D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT area = {};
    HRESULT hrArea = videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT, &area, sizeof(area));
    std::cout << "  VIDEO_FEATURE_AREA_SUPPORT: 0x" << std::hex << hrArea << std::dec << "\n";
    if (SUCCEEDED(hrArea)) {
        std::cout << "    VideoDecodeSupport:  " << area.VideoDecodeSupport << "\n";
        std::cout << "    VideoProcessSupport: " << area.VideoProcessSupport << "\n";
        std::cout << "    VideoEncodeSupport:  " << area.VideoEncodeSupport << "\n";
    }

    // The point of this whole section: is there a D3D12-native hardware motion source?
    const DXGI_FORMAT meFormats[] = { DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM };
    const char* meFormatNames[] = { "NV12", "R8_UNORM" };
    D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAGS blockFlagsNV12 = D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_NONE;
    D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAGS precFlagsNV12 = D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_NONE;
    for (int i = 0; i < 2; ++i) {
        D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR me = {};
        me.NodeIndex = 0;
        me.InputFormat = meFormats[i];
        HRESULT hr = videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, &me, sizeof(me));
        std::cout << "  MOTION_ESTIMATOR (" << meFormatNames[i] << "): 0x" << std::hex << hr << std::dec << "\n";
        if (FAILED(hr)) continue;
        std::cout << "    BlockSizeFlags: 0x" << std::hex << me.BlockSizeFlags << std::dec;
        if (me.BlockSizeFlags & D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_8X8)   std::cout << " 8X8";
        if (me.BlockSizeFlags & D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_16X16) std::cout << " 16X16";
        if (me.BlockSizeFlags == D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_NONE) std::cout << " NONE";
        std::cout << "\n    PrecisionFlags: 0x" << std::hex << me.PrecisionFlags << std::dec;
        if (me.PrecisionFlags & D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_QUARTER_PEL) std::cout << " QUARTER_PEL";
        if (me.PrecisionFlags == D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_NONE)       std::cout << " NONE";
        std::cout << "\n    SizeRange: " << me.SizeRange.MinWidth << "x" << me.SizeRange.MinHeight
                  << " .. " << me.SizeRange.MaxWidth << "x" << me.SizeRange.MaxHeight << "\n";
        if (i == 0) { blockFlagsNV12 = me.BlockSizeFlags; precFlagsNV12 = me.PrecisionFlags; }
    }

    const UINT sizes[][2] = { {1920, 1080}, {2560, 1440} };
    for (int s = 0; s < 2; ++s) {
        D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_SIZE ms = {};
        ms.NodeIndex = 0;
        ms.InputFormat = DXGI_FORMAT_NV12;
        ms.BlockSize = D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16;
        ms.Precision = D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL;
        ms.SizeRange.MinWidth = 640; ms.SizeRange.MinHeight = 360;
        ms.SizeRange.MaxWidth = sizes[s][0]; ms.SizeRange.MaxHeight = sizes[s][1];
        HRESULT hr = videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE, &ms, sizeof(ms));
        std::cout << "  MOTION_ESTIMATOR_SIZE (" << sizes[s][0] << "x" << sizes[s][1] << "): 0x"
                  << std::hex << hr << std::dec << "\n";
        if (FAILED(hr)) continue;
        std::cout << "    Protected: " << ms.Protected
                  << "\n    MotionVectorHeapMemoryPoolL0Size: " << ms.MotionVectorHeapMemoryPoolL0Size
                  << "\n    MotionVectorHeapMemoryPoolL1Size: " << ms.MotionVectorHeapMemoryPoolL1Size
                  << "\n    MotionEstimatorMemoryPoolL0Size:  " << ms.MotionEstimatorMemoryPoolL0Size
                  << "\n    MotionEstimatorMemoryPoolL1Size:  " << ms.MotionEstimatorMemoryPoolL1Size << "\n";
    }

    // Creation is the real answer: CheckFeatureSupport can report a tier the driver will not honour.
    if (videoDevice1 && blockFlagsNV12 != D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_NONE) {
        D3D12_VIDEO_MOTION_ESTIMATOR_DESC desc = {};
        desc.NodeMask = 0;
        desc.InputFormat = DXGI_FORMAT_NV12;
        desc.BlockSize = (blockFlagsNV12 & D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_16X16)
                       ? D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16
                       : D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8;
        desc.Precision = D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL;
        desc.SizeRange.MinWidth = 640; desc.SizeRange.MinHeight = 360;
        desc.SizeRange.MaxWidth = 2560; desc.SizeRange.MaxHeight = 1440;
        ComPtr<ID3D12VideoMotionEstimator> estimator;
        std::cout << "  CreateVideoMotionEstimator: 0x" << std::hex
                  << videoDevice1->CreateVideoMotionEstimator(&desc, nullptr, IID_PPV_ARGS(&estimator))
                  << std::dec << "\n";
        D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC heapDesc = {};
        heapDesc.NodeMask = 0;
        heapDesc.InputFormat = desc.InputFormat;
        heapDesc.BlockSize = desc.BlockSize;
        heapDesc.Precision = desc.Precision;
        heapDesc.SizeRange = desc.SizeRange;
        ComPtr<ID3D12VideoMotionVectorHeap> mvHeap;
        std::cout << "  CreateVideoMotionVectorHeap: 0x" << std::hex
                  << videoDevice1->CreateVideoMotionVectorHeap(&heapDesc, nullptr, IID_PPV_ARGS(&mvHeap))
                  << std::dec << "\n";
    } else {
        std::cout << "  Creation skipped (no ID3D12VideoDevice1 or no supported block size)\n";
    }
}

void ProbePresentation(ComPtr<ID3D11Device> d3d11, ComPtr<ID3D12Device> d3d12) {
    std::cout << "\n=== 6. PRESENTATION API ===\n";
    HMODULE dcomp = LoadLibraryA("dcomp.dll");
    if (!dcomp) {
        std::cout << "UNSUPPORTED: dcomp.dll not found\n";
        return;
    }
    auto pCreatePresentationFactory = (void*)GetProcAddress(dcomp, "CreatePresentationFactory");
    std::cout << "  CreatePresentationFactory export: " << (pCreatePresentationFactory ? "FOUND" : "NOT FOUND") << "\n";

    auto pWait = (void*)GetProcAddress(dcomp, "DCompositionWaitForCompositorClock");
    auto pStats = (void*)GetProcAddress(dcomp, "DCompositionGetStatistics");
    auto pTargetStats = (void*)GetProcAddress(dcomp, "DCompositionGetTargetStatistics");
    auto pFrameId = (void*)GetProcAddress(dcomp, "DCompositionGetFrameId");
    auto pBoost = (void*)GetProcAddress(dcomp, "DCompositionBoostCompositorClock");

    std::cout << "  DCompositionWaitForCompositorClock: " << (pWait ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  DCompositionGetStatistics: " << (pStats ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  DCompositionGetTargetStatistics: " << (pTargetStats ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  DCompositionGetFrameId: " << (pFrameId ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  DCompositionBoostCompositorClock: " << (pBoost ? "FOUND" : "NOT FOUND") << "\n";

#ifdef HAS_PRESENTATION_H
    if (pCreatePresentationFactory && d3d11) {
        auto createFactory = (decltype(&CreatePresentationFactory))pCreatePresentationFactory;
        ComPtr<IPresentationFactory> factory;
        HRESULT hr = createFactory(d3d11.Get(), IID_PPV_ARGS(&factory));
        std::cout << "  D3D11 CreatePresentationFactory: 0x" << std::hex << hr << std::dec << "\n";
        if (SUCCEEDED(hr) && factory) {
            std::cout << "    IsPresentationSupported: " << (uint64_t)factory->IsPresentationSupported() << "\n";
            std::cout << "    IsPresentationSupportedWithIndependentFlip: " << (uint64_t)factory->IsPresentationSupportedWithIndependentFlip() << "\n";
        }
    }

    std::cout << "\n  *** D3D12 PRESENTATION PROBE ***\n";
    if (pCreatePresentationFactory && d3d12) {
        auto createFactory = (decltype(&CreatePresentationFactory))pCreatePresentationFactory;
        
        ComPtr<IPresentationFactory> factoryDev;
        HRESULT hrDev = createFactory(d3d12.Get(), IID_PPV_ARGS(&factoryDev));
        std::cout << "  D3D12 CreatePresentationFactory (Device): 0x" << std::hex << hrDev << std::dec << "\n";
        if (SUCCEEDED(hrDev) && factoryDev) {
            std::cout << "    IsPresentationSupported: " << (uint64_t)factoryDev->IsPresentationSupported() << "\n";
            std::cout << "    IsPresentationSupportedWithIndependentFlip: " << (uint64_t)factoryDev->IsPresentationSupportedWithIndependentFlip() << "\n";
        }

        D3D12_COMMAND_QUEUE_DESC qDesc = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(d3d12->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue)))) {
            ComPtr<IPresentationFactory> factoryQ;
            HRESULT hrQ = createFactory(queue.Get(), IID_PPV_ARGS(&factoryQ));
            std::cout << "  D3D12 CreatePresentationFactory (Queue): 0x" << std::hex << hrQ << std::dec << "\n";
            if (SUCCEEDED(hrQ) && factoryQ) {
                std::cout << "    IsPresentationSupported: " << (uint64_t)factoryQ->IsPresentationSupported() << "\n";
                std::cout << "    IsPresentationSupportedWithIndependentFlip: " << (uint64_t)factoryQ->IsPresentationSupportedWithIndependentFlip() << "\n";
            }
        }
    }
#else
    std::cout << "  UNSUPPORTED: Presentation.h not available during compile\n";
#endif
}

extern "C" typedef uint32_t NVOF_STATUS;
extern "C" typedef NVOF_STATUS(__stdcall* PFN_NvOFGetMaxSupportedApiVersion)(uint32_t* version);

void ProbeNVOFA() {
    std::cout << "\n=== 7. NVOFA ===\n";
    HMODULE nvofa = LoadLibraryA("nvofapi64.dll");
    if (!nvofa) {
        std::cout << "UNSUPPORTED: nvofapi64.dll not found\n";
        return;
    }
    
    auto p11 = GetProcAddress(nvofa, "NvOFAPICreateInstanceD3D11");
    auto p12 = GetProcAddress(nvofa, "NvOFAPICreateInstanceD3D12");
    auto pcuda = GetProcAddress(nvofa, "NvOFAPICreateInstanceCuda");
    auto pvk = GetProcAddress(nvofa, "NvOFAPICreateInstanceVk");
    auto pmax = GetProcAddress(nvofa, "NvOFGetMaxSupportedApiVersion");

    std::cout << "  NvOFAPICreateInstanceD3D11: " << (p11 ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  NvOFAPICreateInstanceD3D12: " << (p12 ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  NvOFAPICreateInstanceCuda: " << (pcuda ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  NvOFAPICreateInstanceVk: " << (pvk ? "FOUND" : "NOT FOUND") << "\n";
    std::cout << "  NvOFGetMaxSupportedApiVersion: " << (pmax ? "FOUND" : "NOT FOUND") << "\n";

    if (pmax) {
        std::cout << "  WARNING: Calling NvOFGetMaxSupportedApiVersion with undocumented signature. Might crash.\n";
        __try {
            auto func = (PFN_NvOFGetMaxSupportedApiVersion)pmax;
            uint32_t version = 0;
            NVOF_STATUS status = func(&version);
            std::cout << "  NvOFGetMaxSupportedApiVersion returned status 0x" << std::hex << status << ", version 0x" << version << std::dec << "\n";
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cout << "  UNSUPPORTED: NvOFGetMaxSupportedApiVersion crashed (Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << ")\n";
        }
    }
}

void ProbeTiming(ComPtr<ID3D12Device> d3d12) {
    std::cout << "\n=== 8. TIMING ===\n";
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    std::cout << "  QueryPerformanceFrequency: " << freq.QuadPart << "\n";

    if (d3d12) {
        D3D12_COMMAND_QUEUE_DESC qDescDirect = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
        ComPtr<ID3D12CommandQueue> queueDirect;
        if (SUCCEEDED(d3d12->CreateCommandQueue(&qDescDirect, IID_PPV_ARGS(&queueDirect)))) {
            UINT64 tsFreq = 0;
            if (SUCCEEDED(queueDirect->GetTimestampFrequency(&tsFreq))) {
                std::cout << "  D3D12 Direct Queue TimestampFrequency: " << tsFreq << "\n";
            }
            UINT64 gpuTS = 0, cpuTS = 0;
            HRESULT hr = queueDirect->GetClockCalibration(&gpuTS, &cpuTS);
            if (SUCCEEDED(hr)) {
                std::cout << "  D3D12 Direct Queue ClockCalibration: GPU=" << gpuTS << ", CPU=" << cpuTS << "\n";
            } else {
                std::cout << "  D3D12 Direct Queue ClockCalibration: UNSUPPORTED (0x" << std::hex << hr << std::dec << ")\n";
            }
        }
        
        D3D12_COMMAND_QUEUE_DESC qDescCompute = { D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
        ComPtr<ID3D12CommandQueue> queueCompute;
        if (SUCCEEDED(d3d12->CreateCommandQueue(&qDescCompute, IID_PPV_ARGS(&queueCompute)))) {
            UINT64 tsFreq = 0;
            if (SUCCEEDED(queueCompute->GetTimestampFrequency(&tsFreq))) {
                std::cout << "  D3D12 Compute Queue TimestampFrequency: " << tsFreq << "\n";
            }
            UINT64 gpuTS = 0, cpuTS = 0;
            HRESULT hr = queueCompute->GetClockCalibration(&gpuTS, &cpuTS);
            if (SUCCEEDED(hr)) {
                std::cout << "  D3D12 Compute Queue ClockCalibration: GPU=" << gpuTS << ", CPU=" << cpuTS << "\n";
            } else {
                std::cout << "  D3D12 Compute Queue ClockCalibration: UNSUPPORTED (0x" << std::hex << hr << std::dec << ")\n";
            }
        }
    }
}

int main() {
    std::cout << "Compiled against SDK version: 0x" << std::hex << NTDDI_VERSION << std::dec << "\n";

    ComPtr<IDXGIAdapter3> adapter;
    ProbeAdapterAndOutputs(adapter);
    
    ComPtr<ID3D11Device> d3d11;
    ProbeD3D11(adapter, d3d11);
    
    ComPtr<ID3D12Device> d3d12;
    ProbeD3D12(adapter, d3d12);
    
    if (d3d12) {
        ProbeD3D12Video(d3d12);
    }
    
    ProbePresentation(d3d11, d3d12);
    ProbeNVOFA();
    ProbeTiming(d3d12);

    return 0;
}
