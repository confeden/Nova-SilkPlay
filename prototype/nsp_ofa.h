// nsp_ofa.h — NVIDIA hardware optical flow (NVOFA) through its D3D11 entry point.
//
// This is the motion source the engine is meant to ship with (D11/I6). It runs
// on OUR device and registers OUR textures: `nvCreateOpticalFlowD3D11` takes an
// ID3D11Device and `nvOFRegisterResourceD3D11` takes an ID3D11Resource, so
// there is no CUDA context, no Vulkan instance and no cross-API sharing here.
//
// One Execute with predDirection = BOTH returns the forward field, the backward
// field and a per-vector cost for each — the bidirectional evidence a proper
// occlusion mask needs, which the block matcher could only approximate.
//
// Hazard: NVOFA device-loss traps are full TDRs (gotcha G13). The one that
// applies here is hintGridSize — it must stay UNDEFINED while external hints
// are disabled.
#pragma once

#include "nsp_common.h"

#include <d3d11_1.h>

#include <memory>
#include <string>

namespace nsp {

class OfaFlow {
public:
    OfaFlow();
    ~OfaFlow();
    OfaFlow(const OfaFlow&) = delete;
    OfaFlow& operator=(const OfaFlow&) = delete;

    // `gridSize` is the output vector grid in pixels: 4, 2 or 1. Allocates and
    // registers the two input luma surfaces and the flow/cost outputs.
    // `wantHints` enables the EXTERNAL HINT input: the hardware search is
    // seeded with a field we supply instead of starting cold. Probed on GB206
    // (G32): NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE is 1, and with outGridSize 4
    // only hintGridSize 4 and 8 initialise — the capability list claims 1 and 2
    // as well and the driver rejects them.
    //
    // TWO TRAPS, both measured, both fatal if ignored. hintGridSize must stay
    // UNDEFINED whenever hints are off or execute TDRs (G13). And an all-zero
    // hint buffer is WORSE than no hints at all, because zero is not "no
    // opinion", it is the assertion "nothing moved" — so a caller that enables
    // hints owes a real field on EVERY execute.
    bool Create(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h, UINT gridSize,
                std::string* err, bool wantHints = false);
    void Destroy();
    bool Ready() const;

    UINT GridSize() const;
    UINT GridW() const;   // vectors across the aligned surface, not the picture
    UINT GridH() const;
    // The hardware is fed an aligned surface; the picture sits in its top-left.
    UINT SurfaceW() const;
    UINT SurfaceH() const;

    // Fill these with 8-bit luma of the previous and the newest source frame,
    // then call Execute(). They are plain R8_UNORM textures with a UAV.
    ID3D11Texture2D* InputPrev() const;
    ID3D11Texture2D* InputNext() const;
    ID3D11UnorderedAccessView* InputPrevUav() const;
    ID3D11UnorderedAccessView* InputNextUav() const;

    bool Execute();

    // The hint surface, R16G16_SINT in S10.5 fixed point like the flow output,
    // sized GridW() x GridH(). Null unless Create() was asked for hints. Fill it
    // before every Execute; see the zero-buffer trap above.
    ID3D11UnorderedAccessView* HintUav() const;
    bool HintsEnabled() const;

    // Flow is R16G16_SINT in S10.5 fixed point: divide by 32 for pixels.
    // Forward is prev->next; backward is next->prev.
    ID3D11ShaderResourceView* FlowFwdSrv() const;
    ID3D11ShaderResourceView* FlowBwdSrv() const;
    ID3D11ShaderResourceView* CostFwdSrv() const;
    ID3D11ShaderResourceView* CostBwdSrv() const;

    // Everything creation learned: driver API version, the surface formats the
    // hardware offered for each usage, what was picked and every HRESULT.
    const std::string& Report() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
