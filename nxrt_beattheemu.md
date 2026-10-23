# Beating the emulator

**The strategy behind nxrt's performance work.** This is not "an emulator, but faster." An emulator must discover and
translate the game at runtime; nxrt knows the exact build ahead of time. The performance program is to exploit that
knowledge, not to out-genericize an emulator.

> You will not win on compatibility, accuracy or breadth. You can win on **stutter, startup, CPU frame time, latency
> and integration** — the things a player actually feels.

## The thesis

An emulator pays for things nxrt can move off the critical path:

| Work | Emulator | nxrt |
|---|---|---|
| CPU translation | JIT / interpreter at runtime | AOT, at setup or build time |
| Shader translation | first use, cached per title | setup, cached, shipped/prepared |
| Pipeline creation | first use | setup, cached, archive/prewarm |
| Firmware/OS boot | modelled | not needed |
| Game files | virtual FS over NAND/keys | direct RomFS folder or package |
| Whole-program optimisation | not across guest code | possible (LTO, direct calls, patches) |
| Native platform features | abstracted | Vulkan, native input, mods, saves |

The advantage is **precomputation and integration**, not a magic faster interpreter.

## What "better" means

Measure these separately. A single average-FPS number hides the actual win.

| Metric | Where nxrt can win | Notes |
|---|---|---|
| First-visit / traversal stutter | **Decisively** | precompile shaders and pipelines; never skip a draw for a pending pipeline |
| Startup / load times | **Decisively** | no firmware/VFS; cached build, shader index and pipeline archive |
| CPU-bound frame time | Often | native C, LTO, direct HLE, syscall fast paths |
| 1% lows | Often | no JIT warmup, no compile hitch, preallocation |
| GPU-bound frame time | Sometimes | native Vulkan passes, compressed textures, fewer copies |
| Input latency / presentation | Often | native input and present path, no extra composition |
| Compatibility breadth | **No** | emulator wins |
| Accuracy / hardware timing | **No** | emulator models more hardware |

If the game is GPU-bound at 4K and the emulator already holds 60 fps, do not promise "much faster." Promise fewer
hitches, faster loads, better latency and native features — and prove it.

## The five levers

### 1. Precompute shaders and pipelines (the biggest perceived win)

Both emulators and nxrt stall when a shader/pipeline is met for the first time. nxrt can move that work to setup or a
loading screen:

- pre-translate every enumerable shader variant (`runtime/nvn/shader_prep.c`, `shader_seen.c`);
- pre-build Vulkan pipelines and save the pipeline cache;
- keep first-use compiles off the render thread and out of gameplay;
- do **not** silently skip a draw because its pipeline is pending — prewarm it, or queue the work;
- measure with `NXRT_SLOW_FRAME_MS=50` and the compile counters in the `[metal]`/`[vk]` stats line.

Code: `runtime/nvn/shader_prep.c`, `runtime/nvn/shader_seen.c`, `runtime/nvn/vulkan/exec.c`, `runtime/nvn/rqueue.c`.
The step-by-step path to complete coverage is in [docs/SHADER_PRECOMP.md](docs/SHADER_PRECOMP.md).

### 2. Remove CPU overhead the emulator must pay

The game is already native C. Make the glue native too:

- **Direct HLE binding.** Imports currently go through fake addresses and dispatch (`runtime/dispatch.c`,
  `hle_register`). Emit direct calls to the host implementation where possible; keep the hot ones on a fast path.
- **Extend HLE** for hot SDK functions: libc already does `memcpy`/`strlen` (`runtime/hle/libc.c`); profile for the
  rest (math, string, file, heap).
- **LTO / whole-program optimisation**: `-O3`, `-flto`, visibility, direct calls. An emulator cannot LTO guest code
  into its runtime; nxrt can.
- **Syscall fast paths**: common mutex/event/wait traffic should not take the full kernel lock/dispatch path
  (`runtime/kernel.c`).
- **Generated-code quality**: `tools/gen.py` output is the hot path; inspect for redundant loads/flag work and
  per-call dispatch. Optimise the generator, not just the C flags.

### 3. Use native GPU features an emulator abstracts away

On Steam Deck this is Vulkan-specific:

- **Compressed textures on the GPU.** If ASTC/BCn is decoded on the CPU and uploaded as RGBA
  (`runtime/nvn/astc.c`, the Vulkan texture path), that is a removable cost. RDNA2 samples ASTC natively.
- **Vulkan descriptor/indirect command buffers** to cut per-draw CPU cost.
- **Render-pass locality** and avoiding unnecessary load/store on RDNA2.
- **Fewer passes and copies**: merge clears/loads, avoid render-target ping-pong and readbacks.
- **Parallel encoding / triple buffering** so the CPU never waits on the GPU (`rqueue.c`, `pace.c`).
- **FSR** and a sane internal resolution instead of native 4K (`output_scale.c`).

An emulator can implement these eventually; its generic translation layer makes it harder, and it must work for every
game. nxrt can specialise for the handful it ships.

### 4. Win on startup, loading and I/O

- no firmware, NAND, keys or sysmodule boot;
- direct RomFS serving (`runtime/services/fs.c`), no virtual filesystem stack;
- cached game-code build, shader index and pipeline archive;
- async/parallel file reads and texture uploads during loads.

This is the difference between an app and an emulator before gameplay even starts.

### 5. Win on latency and integration

- native input, no emulator input layer;
- native present path, vsync/VRR pacing, no extra compositing (`pace.c`, `output_scale.c`);
- mods, widescreen, randomizer, saves, menus, texture packs — features an emulator does not naturally provide.

## What not to do

- **Do not chase compatibility.** "Run every Switch game" is an emulator's job and an emulator will win.
- **Do not put `if (game)` in `runtime/`.** Game policy belongs in `games/<id>/` behind `nx_game` hooks or patches.
- **Do not make a game-specific optimisation the default policy.** Hidden `NXRT_*` flags multiply; prefer a documented
  per-game flag when one is genuinely needed.
- **Do not optimise by guesswork.** Profile first; every change needs a before/after number.

## Concrete first experiments

1. **EoW missing compute instruction.** Implement `eb20138600f70018` in
   `runtime/nvn/maxwell/translate.c` (the `0xebf` reduction is the closest reference), then check the
   `translate failed`/skipped counters and the `[metal]` stats line.
2. **Slow-frame audit.** Run with `NXRT_SLOW_FRAME_MS=50` and collect pipeline/program/shader compile time, texture
   upload, GPU wait and game CPU. The biggest bucket decides the next lever.
3. **Texture-path audit.** Check whether ASTC/BCn textures are CPU-decoded on upload. If yes, try native compressed
   upload and measure load time and GPU bandwidth.
4. **CPU experiment.** Profile the top guest functions and dispatch/HLE overhead; try `-O3 -flto` on the game dylib
   and compare a CPU-heavy scene.
5. **Cold-shader test.** Force cold compiles with `NXRT_SHADER_SALT` and verify that precompile/prewarm removes the
   hitch instead of moving it.
6. **Emulator comparison.** Same Mac, same scene, same resolution and settings. Measure frame-time percentiles and
   hitch counts, not only average FPS.

## Benchmark protocol

Use this for every claimed win. Fill one row per run.

| Field | Value |
|---|---|
| Build / commit | |
| Game / version / scene | |
| Deck / GPU / RAM / SteamOS | |
| Resolution / scale / vsync | |
| Renderer settings | |
| Cold or warm shader cache | |
| Duration / frames | |
| Average FPS | |
| Frame time p50 / p95 / p99 (ms) | |
| Frames over 2x median (hitch count) | |
| Pipeline/program compiles (count, ms) | |
| Texture uploads (MB, ms) | |
| GPU wait (ms/frame) | |
| Startup to first frame (s) | |
| Load time, scene to scene (s) | |
| Input latency (if measurable) | |
| Emulator comparison (same scene) | |

Recommended environment variables: `NXRT_SLOW_FRAME_MS`, `NXRT_LOG_PACE`, `NXRT_SHADER_PREP`, `NXRT_SHADER_SALT`,
`NXRT_WINDOW_SHOT`, `NXRT_DUMP_SHADERS`, `NXRT_SAVE_MSL`.

## Milestones

- **M0 — baseline:** one game, one scene, emulator comparison, frame-time percentiles recorded.
- **M1 — no first-visit stalls:** all shaders/pipelines precompiled or prewarmed; zero draws skipped for a pending
  pipeline; hitch count measured.
- **M2 — CPU win:** top guest/HLE hot spots optimised; LTO/direct binding evaluated; CPU-bound scene at or above
  emulator.
- **M3 — startup/load win:** no firmware boot; cached build/index/pipelines; measured seconds saved.
- **M4 — GPU win:** compressed textures, fewer passes/copies, parallel encode; GPU-bound scene measured.
- **M5 — ship:** one polished port that is demonstrably better than the emulator on the metrics above.

## The genericity contract

The runtime stays generic; the game stays in `games/<id>/`.

- no game names, paths or ids in `runtime/`;
- game-specific behaviour behind `nx_game` hooks (`runtime/nxgame.h`) or per-instruction patches
  (`games/<id>/tools/patches.py`, with `expect()` build checks);
- shared performance work must benefit every port;
- if a flag exists, it is documented and owned by a game or a setting, not a hidden environment variable.

The goal is not "no game-specific code." It is **game-specific code only where the game, not the platform, is the
reason.**
