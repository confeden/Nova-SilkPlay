# NVOFA probes — hardware optical flow on the dev GPU

Pure-`ctypes` Vulkan probes against `C:\Windows\System32\vulkan-1.dll`. **No SDK, no headers, no developer login, no build step** — they run on the stock NVIDIA driver with the system Python. Results and their interpretation live in `.claude/kb/research/gaps-closed-2026-09.md#1`; the machine-fact summary is in `.claude/kb/dev-environment.md`.

Re-run all of these after any driver bump. They are the evidence behind invariant I6.

| Script | Usage | What it answers |
|---|---|---|
| `ofbench.py` | `python ofbench.py` | Does the OFA exist, is there a dedicated queue family, what are its features, grids, limits and supported image formats |
| `ofsingle.py` | `python ofsingle.py <perf 1=SLOW\|2=MEDIUM\|3=FAST> <grid 1\|2\|4> <hintGrid> <execFlags> <iters> [W H] [sessionFlags]` | Cost of one flow execute, timed with GPU timestamps on the OF queue |
| `ofbidir.py` | same arguments; `sessionFlags` bit 0x10 = BOTH_DIRECTIONS, 0x2 = ENABLE_COST | Cost of the configuration the engine actually needs |
| `verify_ofcaps.py` | `python verify_ofcaps.py` | Independent re-read of the capability structs |
| `verify_ofcontent.py` | `python verify_ofcontent.py <perf> <grid> <iters> [dx dy]` | **The adversarial one.** Feeds noise plus a known shifted copy and checks the returned vectors against ground truth, and re-times on real content instead of uninitialised VRAM |
| `verify_ofsubmits.py` | `python verify_ofsubmits.py` | Reproduces the executes-per-command-buffer device-loss ceiling |

One probe here is **not** Vulkan/ctypes — it goes through the NVOFA D3D11 API with the vendored headers, the same bring-up as `prototype/nsp_ofa.cpp`. Build it with `build.cmd` (vcvars64 + one `cl.exe`), then run the exe.

| Probe | Usage | What it answers |
|---|---|---|
| `ofhints.cpp` → `ofhints.exe` | `build.cmd` then `ofhints.exe [--exec] [W H]` | Dumps every `NV_OF_CAPS` the driver reports, then answers whether **external hints** are supported, at which `hintGridSize`, in what buffer format and shape. `--exec` adds five live executes that test whether the hardware merely accepts the hint buffer or actually reads it — with a repeat-run control so a small difference cannot be mistaken for hint influence |

The design point, measured at 2560×1440:

```bash
python tools/ofa-probe/ofbidir.py 3 4 0 0 16 2560 1440 18
```

FAST, 4×4 output grid, bidirectional + cost buffer → **1.72 ms median, p95 1.79 ms**.

Two traps, both a full TDR (`VK_ERROR_DEVICE_LOST` on every subsequent call): a non-zero `hintGridSize` while `ENABLE_HINT` is clear, and more than ~16–18 executes queued in one command buffer.
