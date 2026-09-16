# NVIDIA Optical Flow SDK headers (vendored)

Two headers, unmodified, needed to call the hardware optical flow engine through
its **D3D11** entry point — which takes our own `ID3D11Device` and registers our
own `ID3D11Texture2D` resources, so there is no CUDA context, no Vulkan
instance and no cross-API sharing anywhere in the engine.

| File | What it declares |
|---|---|
| `nvOpticalFlowCommon.h` | every enum and struct: `NV_OF_INIT_PARAMS`, `NV_OF_EXECUTE_INPUT/OUTPUT_PARAMS`, `NV_OF_BUFFER_DESCRIPTOR`, grid sizes, perf levels, status codes |
| `nvOpticalFlowD3D11.h` | `NV_OF_D3D11_API_FUNCTION_LIST` and `NvOFAPICreateInstanceD3D11()`; the D3D11-specific calls are `nvCreateOpticalFlowD3D11`, `nvOFRegisterResourceD3D11`, `nvOFGetSurfaceFormatD3D11` |

The runtime itself is `C:\Windows\System32\nvofapi64.dll`, shipped with the
driver. It exports `NvOFAPICreateInstanceCuda / D3D11 / D3D12 / Vk` and
`NvOFGetMaxSupportedApiVersion` — nothing else; everything is reached through
the function list these headers describe.

## Licence

**MIT**, stated in the header of each file: *"Copyright (c) 2018-2023 NVIDIA
Corporation … Permission is hereby granted, free of charge, to any person
obtaining a copy of this software … without restriction"*, with the usual
requirement to keep the notice. The files are therefore vendored verbatim,
notices intact, and must stay that way.

This is unrelated to the project rule about Lossless Scaling: that rule forbids
reusing a *proprietary competitor's* code. These are the vendor's own
MIT-licensed interface headers for hardware we are entitled to program.

## Provenance and why these bytes are trusted

`NV_OF_API_MAJOR_VERSION 5 / MINOR 0` — matching what `nvofapi64.dll` reports on
this machine (API 5.0, `kb/research/gaps-closed-2026-09.md#1`).

NVIDIA's own public repo, `github.com/NVIDIA/NVIDIAOpticalFlowSDK`, carries only
**SDK 2.0** and has no D3D11 header at all — the 5.0 headers ship inside the
SDK archive behind a developer login. Rather than trust one random copy of a
header whose struct layout, if wrong, is a full TDR (gotcha **G13**), the bytes
here were cross-checked:

* `nvOpticalFlowCommon.h` — **byte-identical** in `mbucchia/Optical-Flow-SDK` and
  `finaltwinsen/VipleStream` (moonlight-qt's vendored copy), and identical in
  code (comments aside) in `shikhindahikar/Nvidia-optical-flow-sample`. Three
  independent copies, one answer.
* `nvOpticalFlowD3D11.h` — code identical in `mbucchia/Optical-Flow-SDK` and
  `jp7677/dxvk-nvapi`; only the licence banner differs (classic MIT text vs the
  SPDX form).

One copy was **rejected**: `jp7677/dxvk-nvapi`'s `nvOpticalFlowCommon.h` is
missing `NV_OF_CAPS_SUPPORT_STEREO` from the `NV_OF_CAPS` enum, which silently
renumbers every capability after it. That project reimplements the API rather
than consuming it, so its headers are edited for its own purposes. This is
exactly the failure a single-source download would have walked into.

Re-verify after any SDK bump: check `NV_OF_API_MAJOR_VERSION` against what
`NvOFGetMaxSupportedApiVersion` returns on the target driver.
