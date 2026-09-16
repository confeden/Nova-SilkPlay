# Capability Probe

A self-contained Windows C++ probe for retrieving system graphics and compute capabilities.

## How to build

Run `build.cmd` from a command prompt. It will set up the MSVC environment using `vcvars64.bat` and compile `caps_probe.cpp` into `caps_probe.exe`.

## How to run

Run `caps_probe.exe` from the command prompt.

## Sections

- **ADAPTER**: Decides which GPU to use and its memory budget.
- **OUTPUTS**: Decides monitor formats, HDR capabilities, and MPO/hardware composition support.
- **D3D11 DEVICE**: Decides D3D11 feature level, typed UAV formats, and specific hardware feature support (like Displayable textures).
- **D3D12 DEVICE**: Decides D3D12 feature level, wave ops, cooperative capabilities, barrier types, and other modern features.
- **D3D12 VIDEO**: Decides whether hardware-accelerated motion estimation (optical flow) is natively supported via D3D12.
- **PRESENTATION API**: Decides if the modern Composition Presentation API (`CreatePresentationFactory`) and independent flip are supported for D3D11/D3D12.
- **NVOFA**: Decides if NVIDIA Optical Flow API is available via `nvofapi64.dll` and its max API version.
- **TIMING**: Decides the GPU and CPU timestamp frequencies and whether clock calibration is supported.
