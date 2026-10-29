# Steam Deck specialization

**What this is.** A strategy for a Deck-native product in the shape of nxnative: **one hardware target family** (LCD
and OLED are the same binary and the same target; their panel/refresh/bandwidth differences are runtime capabilities),
**all SteamOS 3.x that meet the feature floor**, games prepared ahead of time, a console-like launch path. It assumes
the runtime is **nxrt** (AOT recompilation to x86-64 + NVN→Vulkan), not nxnative itself.

**Working name:** `nxdeck` — the Deck host/product layer around `nxrt`. `nxrt` stays the engine and the library;
`nxdeck` is what a player installs, launches from Game Mode, and what carries the Deck profile.

**Current state:** the `steam_deck` branch is a target cut, not a working build. The first job is still
[`docs/HANDOFF.md`](HANDOFF.md) steps 2a–2d, not performance work. This document is the *after it builds* plan, and
the reasoning for what to specialize first.

---

## 1. The correction that determines the whole design

nxnative's core trick does not transport to Steam Deck. For the full comparison — execution model, graphics, host layer,
product shape, codebase size and what does or does not transfer — see
[`NXRT_VS_NXNATIVE.md`](NXRT_VS_NXNATIVE.md).

| | Apple Silicon (nxnative) | Steam Deck (this project) |
|---|---|---|
| Guest ISA | ARM64 | ARM64 |
| Host ISA | ARM64 | **x86-64** |
| How the game's code runs | The NSO is rewritten and **runs directly** on the same ISA | Must be **translated ahead of time** to C and compiled for x86-64 |
| Graphics | NVN → Metal | NVN → **Vulkan / RADV** |
| OS | iOS / macOS | **SteamOS / Linux**, immutable root, Game Mode |
| Power | Phone/desktop | **4–15 W APU**, 40/50 Wh battery, fan and thermal limits |

So "nxnative for Deck" cannot be a port of nxnative's loader. The only way to get the same class of result on Deck is
the thing nxrt already is:

> **Know the exact game build, translate it once, compile it for the exact CPU, translate every shader and build every
> pipeline before gameplay, and ship a host that treats the Deck as a fixed console.**

That is why **nxrt is the right backend**, and why the valuable part of nxnative to copy is not its ARM/Metal code but
its **precomputation and product system**.

### The one-sentence thesis

**Win on stutter, startup, frame-time consistency, latency, battery and integration — not on compatibility breadth.**
An emulator has to discover the game at runtime; you can know it at install time. The Deck's fixed hardware turns that
knowledge into constants, caches and per-title defaults.

---

## 2. Product shape: Mac frontend vs Deck frontend

If the Mac product is "your frontend + nxnative as backend", the Deck product is not a second frontend with a second
backend bolted on. It is:

| Layer | Mac product | Deck product |
|---|---|---|
| Shell | Your native macOS app | **Game Mode + Steam library entry** (a custom full-screen launcher is optional, and usually worse) |
| Session/compositor | macOS window server | **gamescope / SteamOS Game Mode** |
| Input | Keyboard, trackpad, controller | **Steam Input**, sticks, trackpads, gyro, back buttons, touch |
| Power | Plugged in / battery | **TDP 4–15 W**, thermal and battery budgets |
| Backend | nxnative host + NVN→Metal | **nxrt** + NVN→Vulkan |
| Hardware target | Apple Silicon family | **One APU family, one binary; LCD/OLED are capability profiles, not ports** |
| Game preparation | Build-time (Xcode, offline shaders) | **Install-time or first-run** (x86-64 code, SPIR-V, RADV pipelines) |

The shared layer between the two products should be the **game profile schema, the build/install pipeline, the
settings semantics and the compatibility matrix** — not the renderer or the OS host.

### Recommended architecture

```
nxdeck/                         product, packaging, Deck policy
├── frontend/                   Game Mode launch, import/setup, overlay, library entry
├── deck/                       SteamOS integration: gamescope, logind suspend, TDP, refresh, Steam Input
├── profile/                    per-game deck profile (settings, route, cache keys, tested version)
└── tools/                      build/install/bench/regress pipeline

nxrt/                           engine (already exists)
├── tools/                      gen.py, lifter.py, NSO/RomFS parsers
├── runtime/                    loader, kernel, HLE, NVN→Vulkan
├── platform/                   Linux/SteamOS host
├── input/ settings/ save/ ui/  the five layers
└── games/<id>/                 game-specific hooks and patches (NOT in this repo)

third_party/                    SDL3, FFmpeg, shaderc, ImGui, ...
```

**Keep the rule that already exists:** game policy lives in `games/<id>/`, never as `if (game)` in `runtime/`.
Deck policy lives in `nxdeck/deck/` and the per-game profile, never scattered through the renderer.

---

## 3. What to take from nxnative

This is the "best parts" list, translated to Deck mechanics. nxnative is MIT; code can be reused with its notice, but
most of this is architecture, not code you can lift line-for-line.

| nxnative mechanism | Why it wins | Deck / nxrt equivalent | Where it lands |
|---|---|---|---|
| Offline shader split + translation | No runtime shader compile, no first-visit stall | `shader_prep.c` index + offline SPIR-V translation; `NX_SHADER_LIST` + `NXRT_RECORD_KEYS` | `runtime/nvn/shader_prep.c`, `tools/` |
| Pipeline archive + prewarm + hybrid stand-ins | A never-seen pipeline does not stall the frame | RADV PSO archive, GPL (graphics pipeline library), prewarm list, stand-in pipeline while the real one builds | `runtime/nvn/vulkan/exec.c`, `device.c` |
| Per-game profile keyed by build ID | One code path, per-title constants and patches | `games/<id>/nx_game.info` + `patches.py` + new `deck.toml` | `games/<id>/`, `nxdeck/profile/` |
| Tested defaults + settings screen | Players get the known-good config first | Per-game settings defaults; Deck settings (TDP, refresh, cap, scale) | `settings/`, `ui/`, `nxdeck` |
| Build pipeline with content-addressed cache | Rebuild only what changed; repeatable installs | Stage cache keyed by game build, recompiler, translator, Mesa, profile | `tools/deck_build.py` |
| Benchmark routes + regression harness | Claims are measured; regressions caught | Scripted input route, frame-time TSV, screenshots, TDP log, suspend test | `tools/deck_bench.py` |
| Self-contained app packaging | One folder/app, no toolchain on the player's machine | Self-contained `nxdeck` dir + Steam shortcut; code built at install | `nxdeck/`, `runtime/gamebuild.c` |
| HLE host that stands in for Horizon | No firmware, direct game files, native FS/audio/input | Already nxrt's `runtime/hle/`, `services/`, `platform/` | `runtime/` |
| "One hardware target" discipline | Fewer drivers, one performance target, prebuilt caches | LCD + OLED only; one Mesa family; one SDL3/FFmpeg bundle | everywhere |

The macOS counterpart — where Metal makes most of these mechanisms directly portable — is
[`NXNATIVE_IMPORTS.md`](NXNATIVE_IMPORTS.md).

### What not to take

- **The ARM-native execution model.** It cannot work on x86-64.
- **Metal-specific renderer ideas.** The concepts (offline pipelines, present snapshots, encode thread) transfer; the
  code does not.
- **Game profiles wholesale.** They encode per-title facts for a different GPU/driver/OS. Re-derive on Deck.
- **The "compatibility breadth" goal.** nxnative supports a long list because it patches in place; nxrt's strength is
  depth on a few titles. Pick 3–5 launch titles.
- **Shader/pipeline archives as portable downloads.** A RADV pipeline cache is tied to GPU, driver, compiler and
  build; treat it as a local cache, not a universal asset.

---

## 4. What "ultra-specialized" means here

Three rings, in order of leverage:

### Ring 1 — Target constants (compile/install time)

If the target family is fixed, stop compiling generic code. Keep only the capability detection that genuinely varies
(60/90 Hz, panel, battery, storage); everything else is a constant.

- CPU: AMD Zen 2, 4 cores / 8 threads, AVX2 + FMA, no AVX-512.
- GPU: RDNA 2, 8 CUs, 1.6 GHz, RADV, Vulkan 1.3.
- Memory: 16 GB LPDDR5, unified; 88 GB/s (LCD, 5500 MT/s) or 102.4 GB/s (OLED, 6400 MT/s).
- Display: 1280×800; 60 Hz (LCD) or up to 90 Hz (OLED).
- Power: 4–15 W APU; 40 Wh (LCD) / 50 Wh (OLED).
- OS: SteamOS 3.x, immutable root, gamescope, Game Mode.

Concretely: `-march=znver2 -mtune=znver2`, LTO, a Deck build config, fixed guest-memory layout, Deck-specific
thread/core assignment, and a Deck profile that is selected at boot rather than a runtime feature hunt.

### Ring 2 — Precomputation (per game build)

Everything that can be known before the player sees a frame:

- recompiled game code, built once per game build and cached;
- every shader translated to SPIR-V (and optimized) ahead of time;
- every pipeline variant on the shipped route created and cached;
- a recorded and replayed route that proves coverage (`NXRT_SHADER_STRICT`);
- per-game tested settings and a benchmark route;
- a build/install cache keyed by game build + tools + driver + profile.

This is the ring that produces the **"no stutter, fast start"** feel. It is worth more than a 5% micro-optimization.

### Ring 3 — Runtime adaptation (per frame / scene / power state)

Only what genuinely changes at runtime:

- frame pacing and frame cap (60/90, 45, 40, 30) and the game's own 30 fps mode;
- dynamic resolution / FSR 1 when GPU-bound;
- TDP-aware quality (battery vs docked, thermal headroom);
- thread affinity and priorities under load;
- suspend/resume;
- input latency and haptics.

The design rule: **Ring 3 may only choose among states that Ring 2 has already prepared.** It must not trigger a
shader compile, a pipeline build or a game-code rebuild in the middle of gameplay.

---

## 5. One hardware target, all SteamOS 3.x

**Policy:** one binary, one target family, **LCD and OLED treated the same**. The panel differences are runtime
capabilities used for pacing, HDR and battery estimates — not separate builds, not separate code paths in the
renderer.

| | Steam Deck (both panels) |
|---|---|
| APU | AMD custom, Zen 2 4c/8t, 2.4–3.5 GHz; RDNA 2 8 CU, 1.6 GHz (7 nm LCD, 6 nm OLED — same ISA/features) |
| FP32 | ~448 GFlops CPU, ~1.6 TFlops GPU |
| RAM | 16 GB LPDDR5, unified; 88 GB/s (LCD, 5500 MT/s) / 102.4 GB/s (OLED, 6400 MT/s) |
| Display | 1280×800; 60 Hz (LCD) or up to 90 Hz (OLED); no VRR assumption |
| Battery | 40 Wh (LCD) / 50 Wh (OLED) |
| TDP | 4–15 W APU |
| Storage | 64 GB eMMC / 256 GB / 512 GB / 1 TB NVMe, plus microSD |
| OS | **all SteamOS 3.x** that meets the feature floor (see below) |

Do not assume VRR: Valve's spec lists 60 Hz and "up to 90 Hz", not variable refresh. Treat 90 Hz as a fixed-rate mode
unless the specific unit/SteamOS exposes VRR; make it a detected capability, not a design assumption.

### The only per-unit capabilities that matter

Read them once into a `DeckCapabilities` struct and pass them down; do not scatter `if (oled)` through the code.

| Capability | Why it matters | Not a reason to |
|---|---|---|
| `refresh_hz` (60 / 90) | frame-cap divisors, present pacing | build a separate binary |
| `panel` (LCD / OLED) | SDR/HDR, gamma, peak brightness | fork the renderer |
| `memory_bandwidth_class` | texture/cache budgets, render scale | change game logic |
| `battery_wh` | battery estimate, "battery" profile | gate features |
| `storage_class` (eMMC / NVMe / SD) | cache size, install strategy | refuse to run |
| `tdp_limit_w` (user setting) | quality adaptation | override the user |

### SteamOS support policy: "all versions" without building all versions

Literally supporting every past and future SteamOS build is impossible; **supporting the SteamOS 3.x ABI with runtime
capability detection** is not. The rule:

- **One binary.** No per-SteamOS builds, no distro-specific packages. Bundle the user-space libraries you control
  (SDL3, FFmpeg, shaderc); use the system's **Vulkan/RADV** for driver integration and performance.
- **Feature floor, not a version check.** Detect the Vulkan/physical-device features the renderer actually needs
  (dynamic rendering, timeline semaphores, the present modes, GPL, pipeline cache, memory budget). If a feature is
  missing, take the documented fallback; if the floor is missing, show a plain-language error.
- **Forward-compatible by construction.** Do not parse SteamOS/gamescope internals, do not pin a Mesa version, do
  not require a kernel module. Use public interfaces: SDL3, Vulkan, logind, XDG, hwmon.
- **Cache keys include the driver stack.** Mesa/RADV/LLVM version and GPU are part of every pipeline-cache key, so a
  SteamOS update invalidates cleanly instead of loading a stale cache.
- **Test matrix, not a promise.** CI and manual QA on current stable, beta, and the previous stable; older 3.x
  versions are best-effort. "All versions" means "all versions that meet the feature floor", and the app says so
  when it does not.
- **Log the stack.** The `[info]` line records SteamOS, kernel, Mesa/RADV, gamescope and the Deck capabilities, so a
  support bundle identifies the environment without guessing.

| SteamOS branch | Support | Tested |
|---|---|---|
| Current stable | full | yes, every release |
| Beta / preview | full | yes, smoke + one title |
| Previous stable | full | yes, before each release |
| Older 3.x above the floor | best effort | smoke, community reports |
| Below the feature floor | unsupported | clear error + log |

This is also what makes tour records portable: a record set is keyed by **game build + translator + record schema**,
not by the SteamOS or Mesa version. Every Deck compiles the pipelines locally for its own driver. See §11.

---

## 6. Specialization axes

Each row is an axis you can push independently. The "first experiment" is deliberately cheap: it tells you whether the
axis is worth a week before you spend the week.

### 6.1 Instruction set and code generation

**Deck fact:** Zen 2, AVX2/FMA, 4-wide decode, 32 KB L1D / 512 KB L2 per core, 4 MB L3 shared. The current build uses
`-O2`, `-march=x86-64-v3` only for generated code, no LTO, and a separate game `.so`.

**Levers:**
- `-O3 -march=znver2 -mtune=znver2` for the app and generated code; verify no AVX-512 assumptions.
- **LTO across runtime + game code.** The current split (`<id>game.so`) prevents cross-module inlining and
  dead-code elimination. A Deck `MONOLITHIC=1` build that compiles generated C into the executable can be a
  double-digit CPU win.
- **PGO**: instrument a recorded route on Deck, rebuild with the profile. The profile is per game and per build;
  acceptable for a Deck-only product, but doubles install time unless shipped/prebuilt.
- **BOLT** (or `-freorder-blocks-and-partition`) for hot/cold layout after PGO.
- **Dispatch/HLE fast paths**: direct calls instead of fake-address dispatch; inline the hot HLE functions; remove
  redundant guest-memory base arithmetic in generated code.
- **SIMD mapping**: NEON → AVX2 in `simd.h`/`simd_c.py`; check whether 128-bit NEON ops are already widened well.

**First experiment:** A/B `-O2` vs `-O3 -march=znver2` vs `-O3 -flto` on one CPU-heavy scene; record p50/p99 and
guest-function profile. Then measure the same with a monolithic link.

**Expected:** 5–20% CPU frame time; larger on dispatch-heavy titles. **Risk:** build complexity, PGO reproducibility,
code-size growth.

### 6.2 Threads, cores and scheduling

**Deck fact:** 4 physical cores / 8 SMT threads. The game, the render/encode thread, audio, shader preparation and
I/O all compete. `platform/platform.h` already has `plat_thread_core` / `plat_frame_work`, but they are no-ops on
Linux; `plat_thread_perf_core` is also a no-op.

**Levers:**
- Pin the game's main thread and the render thread to **different physical cores**; keep SMT siblings free for
  short-lived workers.
- Size the shader/prep worker pool to 2–3, not `nproc`; a 4C/8T part loses to oversubscription.
- `SCHED_BATCH` for background compile, `SCHED_FIFO`/RT for the audio callback only if measured safe; never starve
  the compositor.
- Reduce kernel-lock contention and context switches in guest synchronization (futex fast paths, spin-then-block).
- Move shader/pipeline preparation off the render thread and **before** gameplay; nxnative's "encode thread" is the
  same idea (about 4 ms/frame in its Breath of the Wild case).
- Keep a core for gamescope/Steam; the app is not the only process on the machine.

**First experiment:** `perf stat -e context-switches,cpu-migrations,cycles,instructions` on a 60 s route; then pin
main/render and compare. `perf record` for the top host functions and guest addresses.

**Expected:** fewer 1% low spikes, 5–15% on thread-heavy scenes. **Risk:** affinity fights SteamOS/gamescope; make it
opt-out and measured.

### 6.3 Memory and UMA

**Deck fact:** one 16 GB pool, no dedicated VRAM; the guest heap can be ~3 GB; the GPU and CPU share bandwidth. The
Vulkan path already tries a whole-heap dmabuf import and falls back to copies when the driver's system heap cannot
hold it (`exec.c` `guest_heap_dmabuf`).

**Levers:**
- **Zero-copy guest memory to GPU.** Import the guest heap once (`VK_EXT_external_memory_host` or dmabuf), make
  textures/buffers views into it; avoid per-frame or per-texture copies. This is the single biggest UMA lever.
- Huge pages: `madvise(MADV_HUGEPAGE)` on the guest arena and code; reduce TLB pressure.
- Allocator: one arena for guest memory; a fast host allocator (mimalloc/jemalloc) for runtime allocations; no
  `malloc` in the frame path.
- Budget the 16 GB: guest heap + shader/pipeline caches + texture pool + OS/gamescope. Evict caches under pressure;
  use `VK_EXT_memory_budget` (already used).
- Avoid readbacks and CPU-side copies of render targets; keep the present path on the GPU.

**First experiment:** log the import path chosen and count copies per frame; run with `NXRT_LOG_MEMORY`/VMA stats;
compare a zero-copy build on a texture-heavy scene.

**Expected:** large load-time and bandwidth wins; fewer GPU stalls. **Risk:** driver/extension support, cache
coherency, alignment.

### 6.4 GPU: RADV, RDNA 2 and the NVN translation

**Deck fact:** 8 CUs, 1.6 GHz, RADV, Vulkan 1.3. The renderer already has a pipeline cache keyed by app version,
translator, GPU and driver (`device.c`), dynamic rendering, FSR 1, SMAA, and a GPL probe.

**Levers:**
- **Pipeline archive + prewarm + stand-ins**: build every pipeline on the shipped route; for an unseen pair, draw with
  a compatible stand-in while the real pipeline builds. This is nxnative's strongest anti-stutter idea and maps
  directly to Vulkan/GPL.
- `VK_EXT_graphics_pipeline_library`: split vertex input / pre-rasterization / fragment / fragment output; cache and
  reuse libraries. The probe exists; make it a product path.
- `VK_EXT_pipeline_creation_cache_control`: avoid blocking creation where the cache already answers.
- **Texture path**: determine whether RADV exposes ASTC on this GPU. If yes, sample ASTC natively. If not, the CPU
  decode in `textures.c` is a major cost: transcode ASTC→BC7/BCn offline at install, or decode on a compute queue.
- **Compressed uploads**: BCn/ASTC directly; no RGBA8 expansion unless the format is genuinely unsupported.
- **Render-pass/barrier hygiene**: avoid redundant load/store, resolve, clear and ping-pong; keep DCC/CCS on
  (`lean_usage` already helps on integrated GPUs).
- **CPU-side draw cost**: descriptor sets, push constants, indirect draws, dynamic state, command buffer reuse;
  the current recorder/queue (`rqueue.c`) is already the right shape.
- **Async compute** for texture/shader preparation and post-process work where it overlaps.
- **FSR 1 / CAS** at 800p; dynamic resolution tied to frame budget. Do not chase FSR 2 without motion vectors.

**First experiment:** `NXRT_VK_GPL_PROBE=1`; `NXRT_LOG_SKIPS=1`; count pipeline builds and skips on a route; check
`format_supports` for ASTC/BCn and the `[vk] format ... not supported` log; GPU time per pass with the existing
timers.

**Expected:** the biggest perceived win (no hitches), plus 10–30% GPU on texture/post-heavy scenes. **Risk:** cache
invalidation on Mesa updates; GPL driver bugs; transcode quality/time.

### 6.5 Presentation, display and pacing

**Deck fact:** 800p; 60 Hz LCD, 90 Hz OLED. The game's own 30/60 mode and the current `pace.c` 60/30 hold were tuned
on macOS and a Deck at 1x (issue #40). Gamescope composites; present mode is currently MAILBOX by default, FIFO
optional.

**Levers:**
- A Deck pacing policy: target 90/60/45/40/30, choose the largest divisor that holds; keep the game's own frame
  step in sync; never double-cap with gamescope.
- Present snapshots / frame buffering: copy the presented image into a small ring so the game's swapchain image is
  free immediately; this is nxnative's `present_snap` and it removed GPU idle time on iPhone. On Deck, test whether
  it helps or just adds latency.
- `VK_KHR_present_wait` / `present_id` if RADV exposes it; otherwise fence-based pacing.
- Fullscreen 1280×800, integer scaling for 2D games, FSR for 3D; avoid compositor scaling when 1:1.
- 90 Hz only when the frame budget genuinely fits; a 45 fps cap on a 90 Hz panel is smoother than 60 on 90.
- Latency: sample input as late as possible before present; reduce queued frames; measure, don't guess.
- HDR/OLED: later; SDR first, then a proper HDR swapchain if the panel path supports it. Do not fake HDR.

**First experiment:** A/B `NXRT_VK_PRESENT=mailbox|fifo|immediate` at 60 and 90; measure frame-time p99 and
present-to-present jitter; try a 45 cap on a 90 Hz panel.

**Expected:** much better perceived smoothness and battery at the same average FPS. **Risk:** gamescope interaction;
latency regressions.

### 6.6 Power, thermal and battery

**Deck fact:** 4–15 W APU, 40/50 Wh battery, fan, thermal throttling. The player owns the TDP slider; the app should
not fight it.

**Levers:**
- Read `hwmon` (`power1_average`, `temp1_input`, `fan1_input`), battery state and the current TDP/refresh settings.
- TDP-aware quality: reduce render scale / cap to 30 / disable SMAA before the fan ramps; recover when cool.
- A "battery" profile (30 fps, 0.7–0.8 scale) and a "docked/performance" profile (60/90, 1.0).
- Avoid unnecessary wakeups: no polling loops; block on fences; batch shader prep.
- Startup/install work is the worst battery drain; require charging or warn, pause on low battery, resume.
- Thermal soak test: 30–60 min, not 2 min. A build that is fast cold and throttles is not fast.

**First experiment:** log W and temp at 60/30 for 20 min on one title; compare battery estimate to actual drain.

**Expected:** 20–40% battery life improvement from a 30 fps cap; better handheld feel. **Risk:** over-aggressive
throttling; user confusion; don't override SteamOS settings silently.

### 6.7 Storage, startup and install

**Deck fact:** 64 GB eMMC is slow and small; NVMe and microSD vary widely. The game code build is minutes on 4 cores;
the RomFS can be tens of GB; shader/pipeline caches add hundreds of MB.

**Levers:**
- One copy of game data; hardlink/reflink where possible; don't keep NSP + extracted + patched duplicates.
- Async/parallel reads; `posix_fadvise`/`io_uring` where it helps; overlap texture upload with loads.
- Build game code at install with all 4 cores, show progress, cache the result by `GAMELIB_ID`; don't rebuild on every
  launch.
- Precompute SPIR-V on a build machine (portable); build the RADV PSO cache on the Deck (not portable).
- Warm-start budget: < 5 s to first frame; cold install budget: define per title (e.g. < 10 min on NVMe).
- eMMC profile: smaller caches, lower-resolution textures optional, warn on space.

**First experiment:** time cold install, warm start, level load, shader prep on eMMC vs NVMe vs microSD.

**Expected:** the "console-like" feel; avoids 30-minute first-run surprises. **Risk:** install size, cache eviction.

### 6.8 OS, Game Mode, suspend/resume

**Deck fact:** immutable SteamOS, gamescope, Game Mode, logind suspend. A desktop app that assumes a normal Linux
session will feel wrong.

**Levers:**
- Launch from a Steam shortcut; fullscreen 1280×800; handle `SteamDeck=1`, `SteamAppId`, `SteamGameId`.
- Use SDL3's Wayland path under gamescope; verify focus, keyboard, OSK, screenshot and overlay behavior.
- **Suspend/resume**: subscribe to logind `PrepareForSleep`; inhibit suspend briefly to finish a save; on resume,
  recreate the swapchain, re-query display/TDP, restart audio, re-arm timers, and recover from `VK_ERROR_DEVICE_LOST`.
  Test 100 cycles, including suspend during a load and during pipeline creation.
- Don't require root; don't modify system files; keep all state under XDG dirs.
- Handle SteamOS updates: Mesa/driver changes invalidate caches; ship a migration path.
- OSK: test Steam's OSK first; keep nxrt's ImGui OSK as fallback.

**First experiment:** 20 suspend/resume cycles with a 60 s gameplay check after each; log device-lost, audio
underruns, and frame-time outliers.

**Expected:** the difference between "a Linux build" and "a Deck app". **Risk:** driver suspend bugs; gamescope
version drift.

### 6.9 Input, haptics and latency

**Deck fact:** Steam Input, 2 sticks, 2 trackpads, D-pad, ABXY, bumpers/triggers, 4 back buttons, gyro, capacitive
sticks, touchscreen, HD haptics. The current input layer is SDL3 with Switch-pad semantics.

**Levers:**
- Steam Input as the primary mapping; provide a recommended per-game layout. Back buttons and trackpads are only
  fully available through Steam Input (or raw hidraw, which is fragile).
- Button prompts: Deck users expect Steam/Xbox-style; default `PROMPTS_AUTO` but make it explicit.
- Gyro: enable per game, with sensitivity/activation; don't force it.
- Trackpads: mouse/radial menu for strategy/RPG titles; pressure click.
- Haptics: SDL3 rumble → Deck HD haptics; keep amplitudes sane; don't buzz constantly.
- Latency: sample at the last safe point, one frame of queued work at most; measure with a high-speed camera or
  LDAT if available.
- Touch: 800p UI; large hit targets; OSK.

**First experiment:** measure input-to-present latency for a 60 Hz and 90 Hz mode; test trackpad/gyro/back buttons
through a Steam shortcut and outside it.

**Expected:** native-feeling controls; fewer "why is this a PC port" complaints. **Risk:** Steam Input configuration
fragility; hidraw differences across SteamOS versions.

### 6.10 Audio

**Deck fact:** PipeWire, stereo speakers, headphone jack, DP/Bluetooth audio docked, suspend/resume.

**Levers:**
- SDL3 audio with a low-latency buffer; avoid underruns during shader/pipeline prep.
- Handle device changes (headphones, dock, Bluetooth) without restarting the game.
- Suspend/resume: drain and restart the stream; don't leave a stuck buffer.
- Audio thread priority and affinity; keep it off the render core.
- Measure underruns and latency; don't chase exotic DSP.

**Expected:** no crackle/pop; correct device switching. **Risk:** PipeWire version differences.

### 6.11 Build and install pipeline

This is nxnative's least glamorous and most valuable part.

**Levers:**
- One command: `nxdeck build <game-folder>` → extract, recompile, translate shaders, build PSO archive, pack,
  write a manifest.
- Stage cache keyed by inputs (game build ID, recompiler, translator, Mesa, profile, flags); atomic writes; reuse.
- Progress as machine-readable JSON (nxnative's `--progress json`) so the frontend can show a real progress bar.
- Derived profiles for untested game versions; warn which fixes don't apply.
- Build on Deck or on a Linux PC; transfer the portable parts (game code, SPIR-V), rebuild only the RADV PSO cache
  on the Deck.
- A manifest with hashes, versions, size, and rollback.

**First experiment:** build one title twice; confirm the second build reuses every stage; measure cold/warm.

**Expected:** reliable installs; faster iteration; less support burden. **Risk:** cache invalidation bugs; disk usage.

### 6.12 QA, telemetry and compatibility

**Deck fact:** one target, two panel capabilities; SteamOS stable/beta updates; Mesa updates; gamescope updates;
64 GB eMMC vs NVMe vs microSD.

**Levers:**
- A test matrix: game × panel capability (60/90, SDR/HDR) × SteamOS branch (stable, beta, previous stable, oldest
  supported) × storage × TDP.
- Scripted routes with screenshots and frame-time percentiles, not "it felt fine".
- Automated suspend/resume, install, update and cache-invalidation tests.
- A crash reporter with a redacted log; opt-in telemetry; no game data.
- A public compatibility table with tested version, known issues, recommended settings, and measured FPS/TDP.
- A "definition of done" per title: 30 min stable, zero shader/pipeline hitches, suspend ×20, no validation errors,
  battery target.

**First experiment:** one title, one route, one LCD and one OLED unit running the **same binary**, 10 runs; publish
the table.

**Expected:** claims you can defend; regressions caught before players. **Risk:** hardware cost/time; telemetry privacy.

### 6.13 UX and frontend

**Deck fact:** 10-foot UI, controller-only, no desk, small screen, suspend anytime.

**Levers:**
- One Steam library entry per game (best for Steam Input, playtime, per-game settings), or one `nxdeck` library app
  if you need a unified importer. Do not build a desktop-style launcher for Game Mode.
- First-run import can be painful in Game Mode (file picker, keyboard). Offer a desktop-mode setup tool and a
  "copy files to this folder" path; validate before launch.
- In-game overlay: settings, TDP/refresh hints, performance HUD, quit. Keep it optional.
- A real progress screen for build/prep, with ETA, cancel, and resume.
- Error messages in plain language: missing keys, wrong game version, unsupported GPU, low disk, failed build.
- Updates: check on launch, never block play; rollback.

**Expected:** the product feels like a console, not a dev build. **Risk:** Steam library/shortcut management is
fragile; file pickers in Game Mode are awkward.

---

## 7. Priority: what to do first

Ordered by **leverage per week**, not by how interesting the work is.

| Priority | Axis | Why first | Effort | Payoff |
|---|---|---|---|---|
| P0 | Make the branch build and run one title | Nothing else is measurable until it runs | 1–2 weeks | baseline |
| P0 | Instrumentation: frame times, skips, TDP, suspend | Without numbers, specialization is guesswork | 3–5 days | enables everything |
| P0 | Shader/pipeline coverage + strict gate **for Tier A titles** | Biggest perceived win; removes stutter where it is promised | 2–4 weeks | "no hitches" on launch titles |
| P1 | Deck pacing (60/90/45/30) + present mode | Cheap; immediate feel/battery | 1 week | smoothness, battery |
| P1 | CPU build config: `-O3 -march=znver2`, LTO | Cheap to try; broad win | 3–5 days | 5–15% CPU |
| P1 | Thread affinity + worker pool sizing | Cheap; 1% lows | 3–5 days | fewer spikes |
| P1 | Suspend/resume + Game Mode launch | Product-defining | 1–2 weeks | "Deck app" |
| P1 | **Missing-pipeline fallback (async build + stand-in)** | No universal warp; partial coverage must not stall or drop draws | 2–3 weeks | playable without 100% coverage |
| P1 | **Best-effort shader tour harness (record/replay + coverage)** | Best-effort coverage; turns per-title tours into data | 2–3 weeks | coverage without per-game tour code |
| P2 | Zero-copy guest heap / texture path | Large but risky; needs driver work | 3–6 weeks | load/bandwidth |
| P2 | PGO/BOLT + monolithic link | Large CPU win but build complexity | 2–4 weeks | 10–20% CPU |
| P2 | TDP-aware dynamic quality | Battery differentiator | 1–2 weeks | 20–40% battery |
| P3 | HDR/OLED polish, 90 Hz tuning | Nice differentiator, not core | 2–3 weeks | quality |
| P3 | Compatibility matrix/telemetry | Necessary for a product, later | ongoing | trust |

### The first ten commits

1. **`vk: add the Vulkan NxUiBackend`** — `runtime/nvn/vulkan/ui_backend.cpp` plus `vk_window()`; follow
   [`docs/HANDOFF.md`](HANDOFF.md) §2a.
2. **`vk: guard LAS-only hooks in exec.c`** — remove/guard `recolour`, `pot_*`, `texpatch_*`, `escape_bottle_*`
   (HANDOFF §2b). Do not put them back in `runtime/`.
3. **`build: add a deck config`** — `DECK=1` with `-O3 -march=znver2 -mtune=znver2`; keep the generic build for
   other x86-64 machines; detect and refuse unsupported CPUs at startup.
4. **`platform: Deck detection and system info`** — fill `PlatSysInfo` (LCD/OLED, refresh, RAM, APU), log it in the
   `[info]` line, expose it to settings.
5. **`platform: thread roles`** — implement `plat_thread_core` / `plat_frame_work` on Linux; pin game/render/audio;
   measure.
6. **`deck: per-game profile`** — start with `games/<id>/deck.toml` (frame cap, refresh, scale, TDP hint, shader
   list, benchmark route); no game names in `runtime/`.
7. **`shader: strict coverage`** — coverage report + `NXRT_SHADER_STRICT=1`; baseline one title.
8. **`vk: pipeline archive/prewarm`** — build the route's pipelines; tag by game build + translator + Mesa/GPU;
   never ship a stale cache as valid.
9. **`deck: suspend/resume`** — logind hook, swapchain recreate, audio restart, device-lost recovery; 20-cycle test.
10. **`tools: deck_build.py + deck_bench.py`** — stage cache, JSON progress, recorded tour route, frame/TDP report.

Then the expensive experiments: monolithic LTO, PGO, zero-copy heap, native compressed textures, encode thread,
present snapshots.

---

## 8. Measurement protocol

Every claim needs a before/after row. Minimum:

| Metric | How |
|---|---|
| Average FPS | frame timestamps |
| p50 / p95 / p99 frame time | frame timestamps (not average) |
| Hitch count | frames > 2× median, and > 50 ms |
| Shader first visits / skipped draws | `NXRT_SHADER_STRICT`, `NXRT_LOG_SKIPS`, `[shader]` log |
| Pipeline builds (count, ms) | `[vk]` stats, GPL probe |
| Startup to first frame | log timestamps |
| Level load | scene marker in the route |
| GPU time per pass | existing GPU timer / `NXRT_VK_PROF` |
| TDP / package power | `hwmon` `power1_average` |
| Temp / fan | `hwmon` `temp1_input`, `fan1_input` |
| Battery drain | `power_supply` energy counters over a fixed route |
| Suspend/resume | 20/100 cycles, time to playable, device-lost count |
| Input latency | high-speed camera / LDAT where available |
| Disk usage / install time | build manifest |

**Rules:**
- Same Deck, same SteamOS/Mesa, same scene, same save, warm cache vs cold cache stated.
- Compare variants in one session; thermal drift is real.
- 1% lows and hitch count matter more than average FPS.
- For emulator comparison: same Deck, same scene, same resolution, **warm** emulator shader cache; otherwise the
  comparison is dishonest.
- Keep a `results.md` per experiment; link it in the PR.

---

## 9. Non-goals

- **Compatibility breadth.** Do not try to run every Switch game. Pick 3–5 titles and make them excellent.
- **A generic emulator.** Do not add an interpreter/JIT fallback to chase unsupported games.
- **A universal warp.** No portable save-state, scene ID or GPU-state restore exists; do not design the product
  around it. Use recorded tours, save corpora and optional per-game hooks (§12).
- **4K/120 Hz.** The Deck is 800p, 60/90 Hz. Optimize there.
- **Online/amiibo/DRM.** Out of scope; don't pretend otherwise.
- **Windows/macOS parity in this product.** Separate targets; don't compromise the Deck build for them.
- **Shipping game data, keys, or generated game code.** Never.
- **A custom desktop launcher for Game Mode.** Steam is the launcher; your app is a game.

---

## 10. Risks and open decisions

**Risks**

- **Legal:** static recompilation of console games is legally contested; nxnative is MIT but ships no game data.
  Get counsel before distributing anything; keep the "user supplies their own files" model; no keys.
- **Driver churn:** SteamOS/Mesa updates invalidate PSO caches. Key them correctly and rebuild; never load a stale
  cache silently.
- **Hardware variance:** LCD vs OLED capabilities, eMMC vs NVMe, panel revisions. Detect capabilities, don't fork
  the build; don't assume 90 Hz or HDR.
- **Tour coverage:** a general harness cannot guarantee every scene; one-time cutscenes, online content, randomized
  areas and games without manual saves leave holes. Record real clips, keep a save corpus, add optional warp hooks,
  and report coverage instead of claiming 100%.
- **Suspend bugs:** Vulkan device loss, audio, timers. Test aggressively.
- **Build time:** on-device compile is slow; a Linux build machine is a better default for developers, but players
  need a local path.
- **Overfitting:** per-game hacks are fine behind profiles; per-game `if` in `runtime/` is not.
- **Scope:** "the most optimized Deck solution" can become an endless performance project. Define the launch bar
  and stop.

**Decisions to make now**

1. **One title or a small set?** Pick the launch title(s) before optimizing; every cache is per game.
2. **Build on Deck or on a PC?** Recommend: build game code/SPIR-V on a Linux PC, build the RADV PSO cache on the
   Deck; support on-Deck build as a fallback.
3. **One Steam entry per game or one `nxdeck` library?** Recommend one per game for Steam Input and playtime;
   `nxdeck` handles import/setup.
4. **Bundle Mesa or use the system's?** Recommend system RADV for driver integration; bundle SDL3/FFmpeg/shaderc.
5. **Is 90 Hz a first-class target or a bonus?** It is a capability, not a separate target: one binary, 90 Hz when
   the panel and frame budget allow, 60/45/30 otherwise. Recommend first-class on OLED for light titles.
6. **How much telemetry?** Recommend opt-in, redacted, no game data.
7. **SteamOS support window?** Recommend: one binary, feature floor, stable + beta + previous stable tested, older
   3.x best-effort, explicit error below the floor.
8. **How are tours produced?** Recommend human/QA **record and replay** clips plus a save corpus, with optional
   per-game warp hooks for hard scenes. No automatic bot is required, and none should be trusted to prove coverage.
9. **What guarantee do we ship?** Tier A (launch titles): zero first visits/skips, full tour. Tier B: no stall or
   skipped draw through the fallback, coverage reported. Tier C: runs, lazy compile, explicit "not precompiled".
   No universal 100% promise (§12).

---

## 11. A best-effort shader tour harness

The tour is the bottleneck in precomputation. nxrt can translate, compile and cache anything, but someone has to
make the game *build every variant once*. Today that knowledge lives in per-game code (`games/<id>/.../shader_tour.mm`,
warp tables, scene hooks). The question is whether the mechanism can be generic.

**Yes — if "general" means the harness, the capture format and the coverage report.** It cannot mean "automatically
reach every scene in every game"; that is game knowledge and, in some games, one-time content. The practical design is:

> **Record once, replay deterministically, capture what the route reaches, merge, validate, then prewarm and prove
> coverage for that route.**

A human (or QA, or the player) plays the game once with recording on. The tool replays the recording without a human,
captures the shader and pipeline variants that route builds, merges it with other clips, and emits the same
`shader_variants.txt` the hand-written tours produce today.

**It does not solve reachability.** There is no reliable warp in every game — see §12. The tool is a coverage
harness, not a completeness guarantee.

### 11.1 What is generic and what is not

| Part | Generic? | Where it lives |
|---|---|---|
| Input recording and replay | **Yes** | `input/`, `runtime/` |
| Save/clip management | **Yes** (uses the game's own saves) | `tools/tour.py`, per-game data |
| Shader/pipeline capture | **Yes** (`NXRT_RECORD_KEYS`) | `runtime/nvn/shader_prep.c` |
| Merge, validate, sort, report | **Yes** (`tools/shader_records.py`) | `tools/` |
| Strict coverage gate | **Yes** (`NXRT_SHADER_STRICT`) | `runtime/nvn/shader_prep.c`, Vulkan backend |
| The route itself | **No — data, not code** | `games/<id>/tours/*.jsonl`, `tour.toml` |
| Warp/teleport/scene-load hooks | **Optional per game** | `games/<id>/runtime/tour.c` behind a generic hook |
| A universal warp | **No** | impossible; not a goal (§12) |
| Automatic 100% coverage | **No** | report it; do not promise it |

The important shift: **routes become data.** Adding a new scene to a tour should be a recording or a save slot, not a
new `.mm` file and a rebuild.

### 11.2 Workflow

```
record ──► replay ──► capture ──► merge ──► validate ──► prewarm ──► strict gate
  │           │           │          │          │            │            │
human/QA   no human    records    dedupe    every hash   build on     zero first
plays      frame-      per clip   + sort    in index     the Deck     visits/skips
           exact                             + ranges
```

### 11.3 Record mode: capture input, not game knowledge

`NXRT_TOUR_RECORD=<file>` records a **frame-indexed input timeline** while the game runs normally:

- keyboard, gamepad buttons/axes, mouse, touch, text input — the raw events, not only the final `NxPad`;
- the presented-frame index for each event (not wall-clock, so replay is independent of machine speed);
- user/QA marks (`F8` or a CLI hotkey): `intro`, `menu`, `field`, `boss`, `shop`;
- optional screenshots at marks or every N seconds (for visual checkpoints and reports);
- the settings and save slot active at the start of the clip.

Why raw events: menus, the launcher and the game consume input differently. Recording only the final pad loses menu
navigation, and recording only game input misses the setup screens.

A **clip** is one recording plus its starting state:

```toml
# games/<id>/tours/main.toml
[[clip]]
name = "intro-and-menu"
save = "slot-0"                 # a copy of the game's own save folder
input = "intro-and-menu.jsonl"
settings = { frame_cap = 60, scale = 1.0 }

[[clip]]
name = "hyrule-field"
save = "slot-1"
input = "hyrule-field.jsonl"
```

No game-specific code is required to record. The clip data is per game, but it is data.

### 11.4 Replay mode: deterministic, fast, headless where possible

`NXRT_TOUR_REPLAY=<file>` injects the recorded events at the same frame indices. The replay run:

- starts from a **copy** of the same save, in a temporary data root;
- uses the same settings (frame cap, render scale, language) as the recording;
- fixes the random seed and disables wall-clock-dependent pacing (`NXRT_PACE=0`, audio off);
- runs as fast as the game allows; presentation can be skipped, but draws still execute so pipelines are still
  created;
- verifies marks with a screenshot/perceptual hash and warns if the replay diverged;
- writes records with `NXRT_RECORD_KEYS=<file>`.

Frame-indexed input makes replay independent of how fast the Deck is. If the game itself reads wall-clock time or an
online service, the replay may diverge; the mark check catches it, and the clip is re-recorded or starts from a later
save.

### 11.5 Capture and coverage: prove what the tour reached

The runtime already records every program, pipeline and compute variant the game builds (`NXRT_RECORD_KEYS`), and
already logs first visits and skipped draws. The tour tool adds the reporting layer:

- per clip and per mark: new shader hashes, new pipeline states, first visits, skipped draws, frames, time;
- total unique records vs the **static shader index** (every shader found in the game files), so you know how much of
  the program space exists before the tour;
- a plateau detector: when N minutes produce no new records, the clip is probably exhausted;
- `tour_report.json` plus a small HTML/Markdown report with screenshots and the missing counts.

The static index is the general part that does not need a tour: `shader_prep.c` can find every `BNSH` program in the
game files and translate it. The tour is only needed for **pipeline state combinations** and for shaders generated at
runtime (Unreal/Oodle, Unity, GLSL capture), which are game-specific realities.

### 11.6 Merge, validate, prewarm

1. `tools/shader_records.py` parses, validates, sorts and dedupes every clip's records.
2. Every shader hash must exist in the local index; every pipeline state must be in range.
3. The merged file becomes `games/<id>/runtime/nvn/shader_variants.txt` and is wired through `NX_SHADER_LIST`.
4. `renderer_prepare` builds the programs and pipelines; the Vulkan pipeline cache is written.
5. `NXRT_SHADER_STRICT=1` replays the full tour and fails on any first visit or avoidable skipped draw.
6. Re-run the tour on a game update, a translator change or a SteamOS/Mesa update that changes the cache key.

The records contain hashes and state, not shader code. They are safe to keep in the game folder and to ship with a
profile.

### 11.7 Optional: coverage-guided exploration

A recorded human tour is the reliable path. For the long tail, an `--explore` mode can supplement it:

- start from each save in a corpus;
- take random/structured actions (menus, movement, combat, pause/resume);
- reward **novelty**: a new shader hash, a new pipeline state, a new screen hash, a new save;
- keep the route prefix that led to novelty; restart from a save when stuck;
- stop on a time budget or a coverage plateau;
- write candidate clips for a human to review, never merge them blindly.

This is not a replacement for a QA playthrough. It is a way to find the menus, options and combat states a scripted
route misses. Treat its output as untrusted coverage data and validate it like any other clip.

### 11.8 Optional: per-game warp hooks

Some scenes are painful to reach: a one-time cutscene, a late boss, a randomized area. A small optional hook makes
the tour dramatically cheaper:

```c
// games/<id>/runtime/tour.c — optional, game-specific, behind a generic interface
bool nx_tour_warp(const char *scene);   // load a scene/save/checkpoint
void nx_tour_mark(const char *label);   // mark a coverage segment
```

The harness works without these hooks, and **no universal warp is possible** (§12). When hooks exist, a `tour.toml`
clip can say `warp = "boss-3"` instead of recording twenty minutes of traversal. Keep the hook in `games/<id>/`; never
put a game name in `runtime/`.

### 11.9 CLI surface

```bash
# record a playthrough (human/QA)
nxdeck tour record --game games/<id> --out games/<id>/tours/field.jsonl

# replay and capture records
nxdeck tour replay --game games/<id> --tour games/<id>/tours/field.jsonl \
                   --records build/tour/field.records --report build/tour/field.json

# merge every clip, validate against the index, write the shipped list
nxdeck tour merge --game games/<id> --tours games/<id>/tours/*.jsonl \
                  --out games/<id>/runtime/nvn/shader_variants.txt

# prove coverage
nxdeck tour verify --game games/<id> --strict
```

The same functions can be exposed as `tools/tour.py` for CI and as a menu entry in the app for QA builds.

### 11.10 MVP, in order

1. **Recorder/replayer**: raw SDL events + frame index, JSONL, one title, replay from a save copy. No coverage logic
   yet.
2. **Capture integration**: `NXRT_RECORD_KEYS`, per-mark counters, `tour_report.json`; run one full clip.
3. **Merge/validate**: extend `shader_records.py`; write `shader_variants.txt`; wire `NX_SHADER_LIST`.
4. **Strict gate**: `NXRT_SHADER_STRICT=1` replay; fix missing variants until zero.
5. **Clips and saves**: multiple saves/chapters; plateau detection; fast/headless replay.
6. **Optional explorer and warp hooks**: only after the human-recorded path is solid.

Acceptance for a **Tier A** title: a recorded tour replays without a human, produces a merged list, and a strict
replay reports zero first visits and zero avoidable skipped draws. For Tier B/C the bar is different: no stall or
missing draw, with stand-in use counted and bounded (§12.5).

### 11.11 Limits and risks

- **No universal warp, and coverage is not guaranteed.** One-time scenes, online content, randomized runs and games
  without manual saves can leave holes. Report coverage; use chapter select, save corpora and optional warps; do not
  claim 100%. §12 defines the tiers and the fallback.
- **Determinism is not free.** Wall-clock reads, RNG and online state can diverge. Use fixed frame input, fixed seed,
  save checkpoints and mark verification.
- **QA time is the real cost.** A 30-hour game needs a 30-hour recording, or several shorter clips. That is still much
  cheaper than writing per-game tour code for every scene.
- **Game updates invalidate records.** Key everything by build ID; re-run the affected clips.
- **Settings matter.** Pipeline state can depend on quality/resolution; record at the target Deck settings and, if
  needed, at the battery profile too.
- **Disk/time.** Screenshots and logs add up; cap them. Replays are GPU-bound even when fast.
- **Legal.** Input recordings and saves are user data. Do not ship game data; do not put saves in the repository.

### 11.12 What this buys

- Per-title tour work becomes **recording and QA**, not C++.
- Routes are reviewable data, not code that can break the runtime.
- The same harness works on LCD and OLED, and on every SteamOS/Mesa version, because it captures records and builds
  pipelines locally for the current driver.
- It makes precomputation practical for a game without hand-written tour code, within the coverage tier you choose —
  not a promise that every scene is reachable.

## 12. No universal warp: coverage tiers and graceful degradation

You are right: **a reliable warp in every game is impossible.** Any plan that depends on one is a per-game tool
wearing a general-tool costume. The plan must not depend on it.

### 12.1 Why universal warp is impossible

| Problem | Why there is no general solution |
|---|---|
| Save/checkpoint format | Every engine writes its own; there is no portable "chapter 7" file |
| Guest state | CPU registers, threads, kernel objects, RNG, physics, animation and online state are game-specific |
| Host GPU state | Vulkan devices, queues, images, pipelines and driver state cannot be serialized portably; a process-level checkpoint (CRIU) does not restore the GPU |
| Scene identity | No universal scene ID; addresses and object layouts differ per build and per run |
| Menus | UI flow differs per game; input automation cannot reliably reach a specific screen |
| Memory cheats | Game-specific, fragile, version-bound, and often blocked or unsafe |
| One-time content | Cutscenes, bosses and randomized events may not be replayable at all |

A **process-level save state** is the closest thing to a universal warp, but on a Vulkan/SteamOS target it means
snapshotting guest memory, host threads, driver objects and GPU state, then restoring them on a different driver
version. That is a research project, not a product feature, and still not reliable across Mesa updates.

So: **do not build the product around warp. Build it around reachable coverage and graceful degradation.**

### 12.2 Reachability sources, cheapest first

| Source | Reliability | Cost | Game-specific? |
|---|---|---|---|
| Full playthrough recording | High for what was played | 1× game length | No code; one recording |
| Save corpus at chapters/checkpoints | High | QA playthrough to create the saves | No code; save data |
| Chapter select / new game+ / debug menu | High when the game has it | Low | No code |
| Menu automation | Medium | Low | No code |
| Attract mode / idle scenes | Low–medium | Low | No code |
| Optional warp hook | High | Low per scene, but code per game | **Yes** |
| Coverage-guided explorer | Low–medium | High | No code, but unreliable |
| Memory editing / cheats | Low | High | **Yes** |

The practical default is **full playthrough recording + a save corpus**. Warp is an accelerator for launch titles,
not a requirement.

### 12.3 Coverage tiers

This is the product answer to "warp is impossible": stop pretending every game gets the same guarantee.

| Tier | Games | Precompute target | Gate |
|---|---|---|---|
| **A — Launch** | 3–5 titles | Full recorded tour + save corpus + optional warp; every route variant | Zero first visits, zero avoidable skipped draws, 30 min stable |
| **B — Supported** | Long tail | Recorded playthroughs/saves where available; otherwise lazy + cache | **No visible stall or missing draw**; first-use compile under budget; coverage reported |
| **C — Unverified** | Anything else | Static shader translation only; lazy pipelines | Game runs; first-use hitch allowed and disclosed; no precompute claim |

The "most optimized Deck solution" claim belongs to **Tier A**. Tier B is "plays well, improves as you play". Tier C is
"it runs, and it gets better after the first visit". Be explicit in the UI and the compatibility table.

### 12.4 The fallback that makes partial coverage acceptable

If warp is impossible and a tour is incomplete, the runtime must not turn a missing pipeline into a stutter, a
skipped draw or a pop. The fallback is the other half of the design:

1. **Static shader pretranslation.** Translate every shader found in the game files, independent of any tour. That
   removes the shader-compile half of first use for static shaders.
2. **Async pipeline build off the render thread.** Do not block the frame on `vkCreateGraphicsPipelines`.
3. **Stand-in pipeline per shader pair.** nxnative's "hybrid pipeline" idea: draw with a conservative, compatible
   pipeline while the real one builds, then swap when ready. It may look approximate for a frame; it does not stall or
   skip the draw.
4. **Never skip a draw because a pipeline is pending.** If a stand-in is impossible, build synchronously at a loading
   screen or present the previous frame; do not silently remove geometry.
5. **Local pipeline cache.** After the first encounter, the pipeline is cached for that game build + Mesa/GPU. The
   second run is smooth even for Tier B/C.
6. **Coverage report.** Show what is precompiled and what is lazy; do not call Tier B "fully precompiled".
7. **Strict gate only for Tier A.** Tier B/C gate on "no stall/skip", not on "zero first visits".

This is the honest answer to "warp is impossible": **you do not need 100% coverage if missing coverage degrades to a
brief stand-in instead of a hitch.** That is a runtime property, not a tour property.

### 12.5 Acceptance criteria per tier

- **Tier A:** recorded tour replays without a human; strict replay has zero first visits and zero avoidable skips;
  suspend ×20; 30 min stable.
- **Tier B:** a fresh save routes through the game without a skipped draw; any stand-in use is counted and bounded; a
  second run has no first-use compile on the same route; coverage is published.
- **Tier C:** the game boots and plays; shader/pipeline compilation is lazy and cached; the UI says "not precompiled";
  no crash or corrupted cache.
- **Never:** a pending pipeline silently removes a draw, or a stale cache is accepted after a driver change.

### 12.6 Product implication

- Optimize a small Tier A set; that is where "most optimized" is measurable.
- For the long tail, ship Tier B: record what QA can, use saves, accept partial coverage, and rely on the fallback.
- Do not build a universal tour bot. Build a **tour harness** plus a **fallback runtime**; together they cover the
  reachable and the unreachable.
- If a game has no warp and no usable save corpus, it stays Tier B/C. That is a product decision, not an engineering
  failure.

## 13. References

- nxnative (the project whose product system is the model): <https://github.com/seedds/nxnative>
- nxnative development notes (offline shaders, pipeline archive, profiles): <https://github.com/seedds/nxnative/blob/main/docs/development.md>
- nxnative per-game profiles: <https://github.com/seedds/nxnative/blob/main/dm/games/README.md>
- AppleGamingWiki nxnative guide: <https://www.applegamingwiki.com/wiki/Nxnative_Switch_to_Mac_conversion>
- Steam Deck LCD specs: <https://www.steamdeck.com/en/tech/deck>
- Steam Deck OLED specs: <https://www.steamdeck.com/en/tech/oled>
- nxrt branch status: [`STEAM_DECK.md`](../STEAM_DECK.md), [`docs/HANDOFF.md`](HANDOFF.md)
- nxrt shader precomputation plan: [`docs/SHADER_PRECOMP.md`](SHADER_PRECOMP.md)
- nxrt architecture: [`docs/ARCHITECTURE.md`](ARCHITECTURE.md)
