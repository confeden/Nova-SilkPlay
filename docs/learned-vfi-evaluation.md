# Learned video interpolation evaluation

## Scope

Measured on **NVIDIA GeForce RTX 5060 Ti 8 GB**, Windows, PyTorch 2.11.0+cu128, TensorRT-RTX 1.7.1.116 and nvidia-vfx 0.2.0.0. This is an offline evaluation and working research prototype, **not a new Chrome engine**. No new binary or SDK was bundled in the installer.

R3 BBB is 1280x720, R2 Meridian 1920x1080, R4 Spring 960x540. Each real-clip midpoint score uses 31 pairs at stride 2; withheld frames provide actual ground truth. Clips are never pooled. RIFE/NeuFlow inference uses 768x448; FrameFusion uses 1024x576; VFG uses the source resolution. RGB output resizing remains in RGB-arm scores. Field arms warp the original source RGB.

GPU timing uses warmed CUDA events with resident inputs, and includes a full model call plus the reference synthesis for one output. It excludes capture, CPU-to-GPU image upload, PNG encoding, scoring, D3D11/CUDA interop and display. It is **not end-to-end FPS**; native 1440p165 remains unverified. Times vary with runtime/cache state and shared-machine load. The PyTorch allocator metric does not include all proprietary SDK/TensorRT allocations.

## BBB: same 31 source pairs

| Arm | PSNR dB | Large-error pixels % | False-edge mean | GPU ms per tested output | Notes |
|---|---:|---:|---:|---:|---|

| Current Nova engine | 25.916 | 2.9773 | 0.03951 | not timed here | shipping configuration |

| RIFE 4.26 PyTorch FP16 RGB | 32.845 | 0.7972 | 0.00413 | 17.898 | resident RGB, resized output |

| RIFE / TensorRT-RTX FP32 RGB | 32.844 | 0.7972 | 0.00413 | 6.262 | resident RGB, resized output |

| RIFE / TensorRT-RTX BF16 + FP32 deconvolutions | 32.838 | 0.7957 | 0.00413 | 3.337 | resident RGB, resized output |

| RIFE exact-phase fields, BF16, original RGB | 32.823 | 0.8036 | 0.00656 | 4.299 | per-phase inference + full-resolution warp |

| RIFE midpoint-reuse fields, FP32 | 32.817 | 0.8057 | 0.00656 | 8.175 | once-per-pair candidate; rejected as universal |

| VFG Medium, default auto-cut | 26.280 | 2.0352 | 0.00511 | 4.250 | false holds on motion |

| VFG Medium, auto-cut off | 30.995 | 1.2715 | 0.00500 | 3.859 | SDK float32 RGB API |

| VFG Low, auto-cut off | 28.995 | 1.9578 | 0.00600 | 0.901 | SDK float32 RGB API |

| VFG High, auto-cut off | 31.032 | 1.2490 | 0.00564 | 25.185 | SDK float32 RGB API |

| NeuFlow v2 + cycle-weighted forward splat | 27.899 | 2.6880 | 0.06039 | 43.698 | two directions in one batch |

| FrameFusion reference | 28.280 | 2.2132 | 0.00611 | 85.366 | predictor + renderer, unoptimized PyTorch FP32 |


The exact-phase field prototype cuts BBB large-error pixels from 2.9773% to 0.8036% (**73.0% relative reduction**) and raises PSNR from 25.92 to 32.82 dB. This metric is not a claim that all perceived artifacts fall by 73%.

## Cross-clip checks

| Clip | Current PSNR / large-error % | Exact-phase BF16 fields PSNR / large-error % | VFG Medium, no auto-cut PSNR / large-error % |
|---|---|---|

| R2_meridian | 44.685 / 0.00057 | 43.242 / 0.00111 | 44.421 / 0.00038 |

| R4_spring245 | 32.425 / 0.95660 | 34.922 / 0.26765 | 34.295 / 0.29002 |


R4 includes labelled cut pairs 10 and 30. Those are discontinuities, not valid continuous-motion interpolation targets. Excluding those two pairs leaves 29 motion pairs:

| Arm | R4 non-cut PSNR dB | Large-error pixels % |
|---|---:|---:|

| Current | 38.011 | 0.10622 |

| Exact-phase fields | 39.998 | 0.05205 |

| VFG Medium, no auto-cut | 39.788 | 0.05580 |


Meridian regresses by about 1.43 dB with the field prototype, although its absolute large-error share remains very small. A universal quality win is not established. The existing P23 cut gate must remain in front of a neural renderer; disabling the SDK detector does not authorize interpolation across a cut.

## Arbitrary-time checks

R3 stride 4 (15 pairs): the network inputs are four true frames apart.

| Phase | Direct RIFE FP32 RGB dB | Midpoint reuse/reprojection dB | Exact-phase BF16 fields dB |
|---|---:|---:|---:|

| 0.25 | 24.909 | 25.201 | 24.886 |

| 0.75 | 24.404 | 22.922 | 24.395 |


Midpoint reuse is **rejected as a universal replacement**: it loses 1.48 dB at t=0.75. A good t=0.5 score alone is not sufficient. Exact-phase inference follows the direct reference closely, but estimates motion per output; this remains an instrument outside the shipping engine I6 contract. No invariant was silently relaxed.

## Runtime findings

- NVIDIA VFG runs from the public [official Python wheel index](https://pypi.nvidia.com/nvidia-vfx/); no NGC credentials were needed. Installed wheel: `nvidia_vfx-0.2.0.0-cp312-abi3-win_amd64.whl`, SHA256 `cebaa12411f7e22b39293205c5c4d07b8073d14ca1e0e7727ee4375207a36445`.
- Default VFG auto-cut copied source A exactly on fast BBB motion pairs 6 and 7 (High also on pair 8). Disabling it raises Medium from 26.28 to 31.00 dB. Use independently verified shot handling. The current Python wrapper was measured on stream 0; changing stream after load failed initialization.
- Naive TensorRT-RTX FP16 produced non-finite output. ONNX weights and Constant nodes were finite; that variant remains rejected.
- Pure BF16 failed on ConvTranspose. Explicit FP32 deconvolution islands, BF16 input binding and dtype/shape checks produced finite, near-FP32 output. The tested graph still contains FP32 sampling operations. It is not an all-BF16 claim.
- Full reduced-resolution RGB RIFE scores only 34.78 dB on Meridian; preserving original RGB through predicted fields gives 43.24 dB. Do not blur the source to obtain faster inference.
- FrameFusion code is Apache-2.0 but its provided weights are CC BY-NC 4.0. It is an evaluation reference only. Its public renderer returns BHWC uint8 and counts intermediate frames: index 0, count 1 is t=0.5. RIFE code/author weights are MIT; NeuFlow code/weights are Apache-2.0; NVIDIA runtime is proprietary. Redistribution terms are not inferred from a successful download.
- Older VFG metrics JSON files carry the unused CLI precision flag (`fp16`); the actual wrapper always used float32 RGB inputs and SDK-selected internal precision. Current probe metadata distinguishes these. Scores/timings are unaffected.

## Validation and remaining work

Three CUDA geometry tests cover pair-renderer endpoints, static-frame preservation, translation at t=0.25/0.5/0.75 and direct phase-conditioned field direction. The field arms keep source RGB in FP32; only the estimator uses reduced precision. All four focused field-matrix cases reproduced the first warmed pair over three repeats with maximum pixel error 0. This is not a full live-soak/determinism certification. The 13-case general matrix and four-case focused matrix completed with no failed rows; rejected precision experiments remain recorded separately.

Next implementation target is a native learned-motion/visibility provider rendering original RGB, with independently retained P23 cut handling. Required before promotion: resolve the I6/amortization trade-off without the failed midpoint shortcut, measure GPU interop and scheduling on real Chrome, test game/DRM pause and recovery, then compare cadence and pictures against LS on the secondary monitor. No Magpie/LS superiority or artifact-free 165 Hz playback is claimed.

## Reproduce

From the repository root, with the local checkpoints described in the project KB:

```powershell
.\.venv-torch\Scripts\python.exe harness/learned_probe.py --model rife-fields --backend trt-rtx --precision bf16 --corpus harness/corpus/R3_bbb60_720crop --out harness/runs/learned/rife-fields-bf16-r3 --max-side 768 --count 32
.\.venv-torch\Scripts\python.exe harness/learned_matrix.py
.\.venv-torch\Scripts\python.exe harness/learned_matrix.py --rife-fields --out harness/runs/learned/fields-matrix-v1
.\.venv-torch\Scripts\python.exe harness/test_learned_geometry.py
```

Evidence: `harness/runs/learned/*/metrics.json`, both matrix manifests, per-pair PNGs and run logs (ignored local artifacts). Timing-only resized fixtures are not quality gates.

Primary references: [NVIDIA VFG API](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html), [TensorRT-RTX](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/), [Practical-RIFE](https://github.com/hzwer/Practical-RIFE), [NeuFlow v2](https://github.com/neufieldrobotics/NeuFlow_v2), [FrameFusion](https://github.com/BurguerJohn/FrameFusion-Model).

Installation used the project venv only: `tensorrt-rtx==1.7.1.116`, `onnx==1.23.2` and the hash-pinned official NVIDIA wheel linked above. Source colors in final field-arm artifacts are FP32; each focused case records that dtype and its repeat-check result.
