# Nova SilkPlay — agent rules

Windows 10/11 desktop app: real-time frame generation for video playing in browsers (Chrome first) and players (PotPlayer first), up to the monitor refresh rate (165 Hz). NVIDIA-native (RTX 50 / Blackwell first, dev GPU RTX 5060 Ti 8 GB), limited CPU fallback. Greenfield: no code yet, research phase.

## Commands
| Task | Command | cwd |
|---|---|---|
| build | `prototypeuild.cmd` -> `silkplay.exe` | `prototype/` |
| run | `silkplay.exe` (no args: finds Chrome itself, waits if absent) | `prototype/` |
| agy delegate | `bash ~/.claude/bin/agy-task.sh -d "$PWD" "<task>"` | repo root |

## Never
- Never extract, copy or vendor Lossless Scaling's shaders/models/binaries (`D:\SteamLibrary\steamapps\common\Lossless Scaling\game`). Studying its metadata/strings/behaviour is allowed; its code is proprietary.
- Never design around capturing DRM-protected video (Widevine L1 / PlayReady); such windows must be detected and skipped, not circumvented.
- Never state a performance number (fps, ms, VRAM) as fact unless measured on the RTX 5060 Ti or cited with hardware; otherwise mark `unverified`.
- Never propose game frame generation: the product is browsers + players only, and must pause when a game loads the GPU.
- Never let the user see the mechanism: no geometry shift or rescale of the picture (1:1 over the video rect), no captured/redrawn/resized cursor, no seam or flicker. These are Lossless Scaling's visible defects and they are release blockers here, not trade-offs (I12, I13, D22).

## Context
`ROADMAP.md` is status and plan — read it and `F:\Claude\Nova SilkPlay\rules.md` once before substantive work.
Deep detail lives in `F:\Claude\Nova SilkPlay\kb\`, indexed in `F:\Claude\Nova SilkPlay\INDEX.md`. Both are
maintained by the `roadmap` skill.
Knowledge base: `F:\Claude\Nova SilkPlay\` — `rules.md`, `map.md`; search `gotchas.md` / `negative.md` with `python ~/.claude/bin/kbq.py find <words>`.
