# Restructuring plan: a macOS port basis

**Goal.** nxrt is the common starting point for porting a Switch game to macOS. It holds exactly what every port needs
to boot, draw and look like a proper app. Everything else is either opt-in (extras) or the game's own work, which lives
outside the library.

**Status (branch `macos-core`):** A1 done (the other backends removed); B0's build profile done; B1 done (the
generator split into stages, byte-identical output); B2 done for LAS and EoW (EoW's compile timing pending); B3 done (stable files); B4 done (the lifter tested against the CPU); B5 done (inline caches).
Everything else is still planned. File paths are `master`'s.

**Left over from A1:** the shader translator still has its GLSL output path and Vulkan-only parts (GMEM chunk bindings
in `maxwell/shader.h`; `tools/test_translate.sh`'s GLSL mode), and the upscaler setting still lists FSR 1 and NIS,
which only the removed backends drew. These go with the extras move (`upscalers`) and a translator clean-up after B1.

**Decided (2026-10-24):**
- **macOS only for now.** The Vulkan renderer and the Windows / Linux platform code are removed, not moved. Kept: the
  backend-agnostic seams (`renderer.h`, `NxUiBackend`, `platform.h`), so another backend can come back later without
  touching core. `steam_deck` keeps its own Linux/Vulkan copy and isn't reconciled.
- **One generic `draw` hook with draw actions** replaces LAS's eight drawing hooks.
- **The recompiler is rewritten** (part B): restructured into clear stages, then made faster to build and faster to run.

- **Apple Silicon only.** Intel Macs aren't supported. The recompiler's portable-C SIMD path (`simd_c.py`, the non-asm
  half of `simd.h` / `simd_fp.h`, `--simd c`) and the Android `--shift` option (`GUEST_SHIFT` in `cpu.h`) are removed;
  generated code always uses the ARM64 inline-asm SIMD.

The work has two parts that can run side by side: **A, the library** and **B, the recompiler**.

---

## Part A: the library

### Where it stands

`master` is about 54k lines, not counting `third_party/`. The games have already left the library (`games/` is
ignored, and each game builds against nxrt).

| Part | Lines | After |
|---|---|---|
| Recompiler (`tools/gen.py`, `lifter.py`, `analyze.py`, `nso.py`, `passes.py`, `simd_c.py`) | ~2.9k | core, rewritten (part B); `simd_c.py` removed |
| Kernel, IPC, loader, dispatch, services, HLE | ~7.5k | core |
| NVN core: objects, command buffers, Maxwell translator, ASTC, Tegra layouts, render queue, pacing | ~5.8k | core |
| Metal renderer (`renderer.m`, `metal.m`) | ~5.3k | core |
| Layers (platform, settings, input, save, ui) | ~7.4k | core, minus Windows/Linux |
| Vulkan renderer (`runtime/nvn/vulkan/`) | ~10.7k | **removed** |
| Windows + Linux platform | ~1.5k | **removed** |
| Opt-in features (mods, texture packs, LiveSplit, upscalers, shader cache…) | ~3.3k | extras |
| Tests and test tools | ~4.7k | dev |

**Hooks born from one game.** `nxgame.h` has about 55 hooks. Which games set them today:

| Used by | Hooks |
|---|---|
| **LAS only** | `draw_shader`, `draw_textures`, `draw_tint`, `draw_mirror`, `draw_effect_stretch`, `instances_tinted`, `instance_tint`, `box_depth_draws`, `scene_fresh_targets`, `pad_mirrored`, `code_variant`, `profiles_*`, `present_wide` / `present_crop` / `present_shape` / `present_frame_end`, `content_dir`, `romfs_override`, `save_read` / `save_written`, `game_tick`, `in_play`, `scene_name`, `pad_script`, `menu_opened` |
| **Sky only** | `inner_entries`, `input_event`, `mouse_wanted`, `stick_deadzone`, `keyboard_prompts`, `language_code`, `max_frame_rate` |
| **LAS and Sky** | `tex_identify` / `tex_generation` / `tex_apply`, `pad_filter`, `pad_final`, `present_tick`, `movie_skippable`, `modules_loaded`, `settings_*`, `menu_pages`, `overlay_*` |
| **EoW, Tunic** | `late_module_init` (plus identity) |
| **Dread** | identity only (and the `romfs_files` field, shared with LAS) |
| **No game** | `romfs_read_filter`, `romfs_file_opened`, `tex_replace` |

The expensive ones are LAS's drawing hooks. Mirror mode and the gold tints are threaded through the shader translator
and both renderers, in about 85 places.

### Target layout

```
nxrt/
  core/                      the macOS port basis (~30k lines)
    platform/                macOS + shared parts (paths, app identity, logging, crash dialog); platform.h is the seam
    settings/ input/ save/   as today (input without rumble / light, which move to extras)
    ui/                      start screen + setup, menu (system tabs + the game's tab), on-screen keyboard, option rows;
                             ui/backends/metal.mm behind NxUiBackend
    runtime/                 loader, dispatch, kernel, IPC, services, HLE, main, game-files setup (gamebuild, romfs_build,
                             nxpkg), diagnostics (crash_explain, slowframe, fnprof)
    runtime/nvn/             objects, cmdbuf, formats, tegra, astc, rqueue, pace, maxwell/; renderer.m + metal.m behind
                             renderer.h
    recompiler/              the rewritten generator (part B)
  extras/<feature>/          opt-in features, registering through core's extension points
  dev/                       tests/, tools/test_*, log_triage.py, format.sh, tidy.sh, run_ports.sh, gen_compile_commands.py
  examples/                  game-template, ui-host
```

A game's Makefile chooses its features:

```make
NXRT_BACKEND  := metal                                 # the only one for now: the seam stays visible
NXRT_FEATURES := mods texpacks upscalers shader-cache  # empty = the bare core
include $(NXRT)/nxrt.mk
```

### What moves where

**Stays in core:**

| Area | Files (`master`) |
|---|---|
| Recompiler | `tools/*` generator files → `core/recompiler/` (rewritten in part B), `gamelib_entry.c`, `sym.py` (made module-aware) |
| Code runtime | `runtime/cpu.h`, `simd.h`, `simd_fp.h`, `loader.c`, `dispatch.c`, `gamelib.*`, `kernel.*`, `ipc.*`, `hle.h`, `rt.h`, `main.c`, `nxgame.h`, `lz4_block.*` |
| OS emulation | `runtime/hle/*` (framepace, heaps, hid, libc, movie, nvn_boot, pl, swkbd, vi), `runtime/services/*` |
| Graphics | `runtime/nvn/objects.c`, `cmdbuf.c`, `formats.c`, `tegra.*`, `astc.*`, `rqueue.*`, `pace.*`, `tex_hash.h`, `maxwell/*`, `renderer.*`, `metal.m`, `smaa_data.c` (SMAA is the default antialiasing) |
| Game setup | `runtime/gamebuild.*`, `romfs_build.*`, `nxpkg.*`, `rt_ui.mm` (minus the extras' rows), `rt_settings.c`, `settings.h`, `frame_rate.*` |
| Diagnostics | `runtime/crash_explain.c`, `nvn/slowframe.*`, `fnprof.c` |
| Layers | `platform/app.c`, `paths.*`, `platform.h`, `posix_log.h`, `macos.c`, `nx_app.h`; `settings/*`; `input/*`; `save/*`; `ui/*` with `ui/backends/metal.mm` |

**Moves to extras:**

| Feature | From | Needs from core |
|---|---|---|
| `mods` | `runtime/mods.*`, `rt_mods_ui.mm`, the LayeredFS part of `services/fs.c` | a RomFS overlay registry (`rt_fs_add_overlay`) |
| `texpacks` | `runtime/nvn/custom_tex.*` | a texture-replacement provider (today's `tex_replace` path) |
| `upscalers` | `runtime/nvn/output_scale.*` (MetalFX, FSR, NIS), the upscaler rows in Video | a present-pass stage |
| `shader-cache` | `runtime/nvn/shader_prep.*`, `shader_seen.c`, `build_cache.*`, `cache_sha256.h`, the start-screen shader bar | a pipeline-cache provider, a start-screen task slot |
| `livesplit` | `runtime/livesplit.*`, `run_timer.*`, the Controls knobs | a frame / state callback for extras |
| `screen-filter` | `runtime/nvn/screen_filter.h` and its use in `metal.m` (from LAS's Cosmetics tab) | the present-pass stage |
| `pad-extras` | rumble and controller light (`input/input.c`, `services/hid_set.c`, the renderer's vibration path) | an output hook in nx-input |
| `update-check` | the release check in `ui/launcher.mm` | the start-screen info slot (`NxUiStartInfo`) |
| `boot-profiles` | `profiles_*`, `code_variant` (LAS's randomizer seeds; usable for mod sets too) | the generator's `VARIANTS` option (part B keeps it) |
| `draw-tint` | the tint binding in translated shaders | the draw actions' tint fields |

Each extra registers like a game does: settings sections (`nx_settings_register`), menu tabs and rows (`nxui_*`), and the
new extension points. Core never `#include`s an extra. With `NXRT_FEATURES` empty, none is compiled.

**Removed:**
- `runtime/nvn/vulkan/*`, the ui Vulkan backend, every non-Apple renderer path.
- `platform/windows.c`, `windows_threads.h`, `win_compat.h`, `windows.rc`, `windows.manifest`, `tools/build_deps_windows.sh`,
  `tools/pe_no_cfg.py`; `platform/linux.c`, `linux_compat.h`, `cpu_check.h`.
- The Windows-only features: ReShade, the OneDrive warning.
- The portable-C SIMD path: `tools/simd_c.py` (~1.2k lines), the non-asm half of `runtime/simd.h` / `simd_fp.h`, and
  `--simd c`; the Android guest shift (`--shift`, `GUEST_SHIFT` / `GUEST()` in `cpu.h`, `rt.h`, `kernel.h`, `fnprof.c`).
- **Kept:** `tools/fetch_zig.sh` and `zig_slim.sh` (macOS ships zig to build the game code on players' Macs), without
  their Windows / Linux branches. All of this stays in git history and on `steam_deck`.

**Moves to dev:** `tests/`, `tools/test_*`, `tools/log_triage.py`, `format.sh`, `tidy.sh`, `run_ports.sh`,
`gen_compile_commands.py`. These still run in CI, but a port doesn't build them.

### Game-born mechanisms leave the library

| Mechanism | Today | After |
|---|---|---|
| Mirror mode | `draw_mirror`, `pad_mirrored`, `flip_x` in the shader keys | Draw action "flip"; LAS flips the pad in its own `pad_filter` |
| Gold tints | `draw_tint`, `instances_tinted`, `instance_tint`; a tint binding in every translated shader | Draw actions "tint this draw / these instances"; the binding is compiled in only with `draw-tint` |
| LAS widescreen pieces | `box_depth_draws`, `draw_effect_stretch` | Draw actions "box to 16:9", "stretch" |
| Phone-box scene | `scene_fresh_targets` | A `scene_changed` hook returning flags ("clear targets") |
| LAS test hooks | `NXRT_DOORJUMP_TEST`, `NXRT_PLACE` in `input/input.c` | Gone; LAS scripts input through `pad_script` |
| LAS comments | "rupee" in `metal.m`, `option_rows.h` | Reworded |
| Unused hooks | `romfs_read_filter`, `romfs_file_opened`, `tex_replace` | Dropped; `tex_replace` becomes the texpacks extension point |

**Draw actions:**

```c
typedef struct NxDrawActions {
    bool skip;            // leave the draw out
    bool flip_x;          // mirror it horizontally
    bool box_16_9;        // keep it inside the 16:9 area (widescreen)
    float stretch_x;      // 1 = none
    bool tinted; float tint[8];  // rgba multiply, rgba add (needs NXRT_FEATURES += draw-tint)
    bool (*instance_tint)(const float *model, float tint[8]);  // per instance (ssbo0 matrices), NULL = all alike
} NxDrawActions;
bool (*draw)(const NxDraw *d, NxDrawActions *a);  // render thread; d->stage: program known / textures bound
```

**The hook set after:** about 35 instead of 55, with no hook that exists for only one feature of one game.
- **Identity:** `nx_game.info`.
- **Boot:** `modules_loaded`, `inner_entries`, `late_module_init`.
- **Frame:** `game_tick`, `present_tick`, `in_play`, `scene_name`, `scene_changed`, `movie_skippable`.
- **Input:** `input_event`, `mouse_wanted`, `stick_deadzone`, `keyboard_prompts`, `pad_script`, `pad_filter`, `pad_final`.
- **Files and saves:** `romfs_override`, `content_dir`, `save_read`, `save_written`.
- **Options:** `language_code`, `max_frame_rate`, `settings_*`, `menu_pages`, `menu_opened`, `overlay_*`.
- **Graphics:** `tex_identify` / `tex_generation` / `tex_apply`, `draw`, `present_wide` / `present_crop` /
  `present_shape` / `present_frame_end`.

---

## Part B: the recompiler rewrite

### What it is today

`gen.py` and its helpers (~2.9k lines of Python) turn each game module into C:
- `nso.py` reads the module;
- `analyze.py` finds the functions (symbols, unwind data, call targets, address references, in rounds);
- `gen.py` finds control flow and jump tables, then writes the C;
- `lifter.py` turns each instruction into C;
- (`passes.py` isn't part of it: it summarises a renderer debug log, and moves to dev.)

clang then compiles the C into the game library.

**Measured on LAS (B0, 2026-10-24, M-series Mac, 8 cores, 16 GB; branch `macos-core`):**

| | main | sdk | total |
|---|---|---|---|
| Functions | 86,669 | 34,982 | 121,651 |
| Jump tables | 448 | 489 | 937 |
| Indirect jumps resolved at run time (`br_dispatch`) | 4,640 | 9,940 | 14,580 |
| Generation (Python) | 21 s | 5 s | 26 s |
| Generated C | | | 451 MB in 719 files |

- **Compiling:** 1,260 CPU-seconds, **163 s wall-clock** with 8 jobs. The app's own build on the portable SSD took
  324 s; the difference is generation, the game drive's I/O and the build cache's receipts.
- **The fallback is the cost:**
  - **What:** 817 functions (0.67%) use the all-instruction fallback. They hold 9% of the C (40 MB, 307,075 switch
    cases).
  - **How much slower:** they compile at **21 s per MB of C, against 1.7 s per MB for files without one: 12× slower**.
  - **Share of the time:** files that are mostly fallback take 538 s of the 1,260 (43%).
  - **The worst offenders:** the 12 slowest files are each a single fallback function. The slowest, `f_70007d88a0`
    (254 KB of C), takes **61 s alone**, which is also the build's critical path.
  - **No optimisation at all:** 6 functions are compiled `optnone` (2.9 MB).
- **Estimate:** compiling the fallback C at the normal rate would bring the compile from ~1,260 to ~760 CPU-seconds
  (−40%), and the wall-clock from 163 s towards ~95 s, since the 61 s tail goes. Real table recovery won't prove every
  table, so this is an upper bound.
- **Rebuilds are already partly incremental.** The app's build cache (`runtime/build_cache.*`) hashes each generated file
  and reuses its object when the file is unchanged, and removes the generated C after a successful build. Two things are
  still missing:
  - **Files shift:** functions are packed into files by size, so one changed function moves the boundaries of every file
    after it, and most objects are rebuilt anyway.
  - **Disk space:** the 720 kept objects are the 1.8 GB left in the game folder.
- **Not measured yet:** run-time speed (frame time in a fixed scene). That needs a scripted in-game scene and comes with
  B2.

**What holds it back:**
1. **The all-instruction fallback.** Any function with a jump table makes every instruction a possible jump target: one
   giant `switch`. This is a safety net for table cases that were missed. clang can't optimise these functions well, and
   above 3,000 instructions they are compiled with no optimisation at all (two LAS functions took over an hour each).
2. **Guest registers live in memory** (`c->x[...]`). They are written back at every call, dispatch and kernel call, so
   the generated code does far more loads and stores than the original.
3. **Every indirect jump is a table lookup** (`rt_dispatch`): virtual methods, function pointers, calls between modules.
4. **Rebuilds mostly start over.** Objects are reused per file, but files are packed by size, so one changed function
   shifts every file after it. A recompiler change rebuilds nearly everything.
5. **The stages are tangled.** Discovery, control flow, patching and emission share state inside `gen.py`, which makes
   each improvement risky and hard to test on its own.

### The new shape

It stays in Python (generation isn't the bottleneck), as a pipeline with one job per stage and data passed between them:

```
load ──► discover ──► cfg ──► lift ──► optimise ──► emit ──► plan
(nso)    (functions)  (blocks,  (instr →  (passes)    (C)       (files, hashes,
                      tables)   IR/C)                           what to rebuild)
```

| Stage | Job | New |
|---|---|---|
| `load` | Read the NSO, relocations, symbols | Unchanged (`nso.py`) |
| `discover` | Function starts | Takes `inner_entries`-style hints from the game, and reports what it couldn't classify |
| `cfg` | Basic blocks, branch targets, jump tables | **Real jump-table recovery** (bounds from the compare before the table, emulation as a check), per table; the all-instruction fallback only for tables it can't prove |
| `lift` | Instructions → C | Unchanged semantics at first; the target of differential testing |
| `optimise` | Passes over the lifted code | Register caching (step B5) lives here |
| `emit` | C text | One function per stable unit; game patches and `VARIANTS` applied here |
| `plan` | Which files to compile | **Stable file assignment and a hash per function**, so a rebuild recompiles only what changed |

Plus a `report` at the end: functions, tables (proven / fallback), indirect jumps, `optnone` functions, compile units.
That makes every improvement measurable per game.

### B1 result (2026-10-24)

- **The split:** `tools/gen.py` (690 lines) became a 70-line driver over `tools/recomp/`: `module.py` (load),
  `discover.py`, `cfg.py`, `emit.py`, `plan.py`, `report.py`. `lifter.py` is the lift stage unchanged.
- **The hidden state is gone:** the game's patches are a `Game` object passed in instead of globals, and
  `emit_function` returns its references instead of leaving them on the module.
- **Verified:** all 16 modules of LAS, Sky, EoW, Tunic and Dread (3.3 GB of C) are **byte-identical** to the
  single-file generator's output. The statistics lines and the progress lines the start screen reads are identical
  too.
- **Not verified:** `tools/test_lift.py` can't run. Its C harness (`tools/test_harness.c`) is missing on `master`
  too, an older breakage. B4 rebuilds that test.

### B2 result (2026-10-24)

- **How a table is proven:** a table is emulated twice, with every register other than the index holding different
  values. If both runs give the same targets, the targets depend only on the index and the module's own data, and
  the table counts as proven.
- **What a proven table becomes:** a plain `switch` over its targets; its default (which the bounds check rules
  out) goes to `rt_dispatch`, which names the address if a case was ever missed. Only a `br` that isn't a proven table
  makes its function take the all-instruction fallback. `optnone` applies only to those.
- **Proof failures:** few. Between 0 and 6 tables per module failed the second run; they keep the fallback.

| Game | Functions with the fallback | Compiled unoptimised | Game library |
|---|---|---|---|
| LAS | 817 → **107** | 6 → 1 | 91 → 80 MB |
| Sky | 2,324 → **341** | 95 → 24 | (not built) |
| EoW | 2,304 → **413** | 43 → 18 | 220 → 195 MB |
| Tunic | 2,183 → **316** | 30 → 11 | (not built) |
| Dread | 1,071 → **143** | 9 → 1 | (not built) |

- **LAS compile** (back to back, same conditions, source on the portable SSD): **wall-clock 210 → 141 s, CPU 1,651 →
  1,101 s: −33% both.** B0's estimate was an upper bound of −40%. The slowest file is now 39 s (was 47 s), and after
  the top two the files drop to 18 s and below.
- **EoW compile:** not timed yet. The run was stopped to build the libraries for play-testing.
- **Play-tested:**
  - **LAS:** gets into gameplay (Tarin's house) with no faults.
  - **EoW:** plays. Its occasional freezes (every guest thread waiting on a condition variable) were there before B2
    too.
  - **Sky, Tunic, Dread:** generated only, not built or run.
- **Found while testing (fixed separately):** the start screen waited forever on shader preparation for games set up
  from a package (no `romfs/` folder). Older than B2, and on `master` too.

### B3 result (2026-10-24)

- **The change:** chunk file `<module>_NNN.c` now holds exactly the functions whose entry is in the NNN-th 64 KiB of
  the module's text. Every range gets its file, even an empty one, so an index never moves. The names stay
  `<module>_NNN.c`, because the app's builder compiles files by index while generation runs. `--span` replaces
  `--chunk` / `--chunk-bytes` / `--big-fn`. Big functions no longer get a file of their own.
- **Same code:** every function's C is identical to B2's, in all six modules of LAS and EoW (LAS: 86,669 + 34,982;
  EoW: 295,093 functions), and so are the address tables. Only the grouping changed.
- **What a change rebuilds** (LAS `main`):

  | Change | B2 (packed by size / count) | B3 (address ranges) |
  |---|---|---|
  | One function's C changes (a patch) | 1 of 499 files | 1 of 325 files |
  | A new function is discovered | 6 of 499 files | 3 of 325 files |

  B2's packing was already more stable than this plan assumed: its files are mostly size-limited, so a shift settles
  again within a few files. B3 is strictly local, but the rebuild-time gain is modest.
- **Full compile** (LAS, back to back, source on the SSD): **wall-clock 122 → 114 s, CPU 949 → 853 s**. That's
  −7% / −10%, from fewer files (650 → 411), each paying a fixed cost. File counts: LAS 324 + 85, EoW 637 + 95 + 105 + 53.
- **Not done:** an option to drop the kept objects (the 1.8 GB in the game folder) after a successful build.

### B4 result (2026-10-24)

- **How:** `tools/test_lift.py` (rewritten) with `tools/test_lift_harness.c` (new; the old Unicorn harness had gone
  missing). Each case runs one instruction twice from the same random state: as its lifted C, and as itself on the
  Mac's CPU, in a stub made at run time (`MAP_JIT`). Compared: x0-x30, sp, nzcv, v0-v31 and a 256 KiB memory buffer.
  Faults in either run skip the case. `make test-lift` runs it on every module of a game's own files.
- **Does it catch bugs?** Yes: three planted bugs (a register, a flag, a vector register) were caught in all 9 of
  their cases.
- **Result:** **no mismatch** in 30,000 distinct instructions (5,000 from each module of LAS and EoW), 90,000 cases.
  One case was skipped (a native fault).
- **Not covered:**
  - **Position-dependent instructions:** branches, ADR / ADRP, literal loads (the native copy runs elsewhere).
  - **System instructions.**
  - **Instructions naming register 18:** reserved on macOS; about 3% of the distinct words.
  - **More threads at once:** each case runs single-threaded. The lifted exclusive pairs (LDXR / STXR, emulated
    with compare-and-swap) and the atomics are checked one step at a time, not under contention. That matters for
    the EoW deadlock (a lost wakeup), which this test can't rule out.
- **Not done:** running it in CI. It needs a game's files, which CI doesn't have.

### B5 result (2026-10-24)

- **Measured first:** LAS gameplay (a scripted walk on a copy of a save, the system sampler, 15 s). Only ~0.4 of a core
  is busy. Dispatch (`rt_dispatch`, its dynamic-link stub, the macOS TLS lookup, misses) was **6.2–6.9% of the busy
  CPU**, about a fifth of the time spent in game code.
- **The change:**
  - **Every site:** each indirect call (`blr`), indirect jump (`br`, which includes every call between modules
    through the import stubs) and jump out of the module has an **inline cache**: a `{key, fn}` pair, read with one
    `ldp` and written with one `stp`, single-copy atomic on Apple Silicon (FEAT_LSE2). A hit is a load, a compare
    and a call.
  - **A miss** goes to `rt_dispatch_ic`, which looks up as before and fills the site's cache.
  - **Reboots:** `key = address ^ g_ic_epoch`, and a reboot changes the epoch, emptying every cache.
  - **Not cached:** HLE targets, so their calls keep the reboot safe point.
  - **No pre-resolution:** calls between modules needed no separate step. Their import-stub `br` is cached after the
    first call.
- **Result** (LAS, same scripted walk, back to back): **dispatch 359 → 84 samples (6.2% → 1.6% of busy CPU, −77%)**;
  total busy CPU −7%; the same scene, no faults. Cost: the LAS game library grows **80 → 92 MB** (about 30,000 sites),
  and EoW's **195 → 228 MB**.
- **EoW:** runs with no faults. Over 3 + 3 runs it froze in both builds, B2 after 927 / 1,587 frames (one run didn't
  freeze), B5 after 61 / 182 / 2,787. Every freeze has the same signature: the main thread waits at identical return
  addresses, with the same idle workers. That's EoW's existing deadlock, not a new one. Whether B5's timing makes it
  come sooner, three runs can't tell.

### B6 (parked, 2026-10-25)

- **Step 1 is in** (`3d1516b`): FP/SIMD runs without memory accesses name their exact operands instead of clobbering
  all memory. Runs of up to 12 V registers use register operands, bigger ones memory operands (up to 16 fields), and
  everything else keeps the old form.
- **Checked:** the CPU test (90,000 cases) matches. LAS and EoW build (the 16-field cap fixed the two files that
  failed register allocation). LAS played 98 s with no faults at ~59 fps. SIMD-heavy files: up to −27% instructions
  and −44% loads/stores; ordinary files within about ±3%.
- **Not done:** LAS's CPU use vs B5; an EoW run; step 2 (guest registers in C locals across functions).
- **Parked** to work on frame rate (the GPU / render side) first.

### Steps

Each step keeps the games working. The library's id covers the recompiler (see ARCHITECTURE.md), so each step that
changes the output makes players rebuild once. Bundle those steps into as few releases as possible.

| Step | What | How it's checked | Players rebuild? |
|---|---|---|---|
| **B0. Baseline** ✅ build side done (above); run-time benchmark pending | Profile a full LAS build: time per compile unit, the slowest functions, how many go through the fallback; a run-time benchmark (fixed scene, frame time) | Numbers recorded in this doc | No |
| **B1. Restructure** ✅ | Split into the stages above, **with byte-identical C output** for LAS, Sky, EoW, Tunic and Dread (`--simd asm`, no shift). Drop `--simd c` and `--shift` here: they don't change the output of an Apple Silicon build | `diff` of the generated C against today's | The library id changes (new files are hashed), but the C is identical, so the app's build cache reuses every object: a relink, not a rebuild |
| **B2. Jump tables** ✅ (LAS, EoW) | Real table recovery; the fallback only where unproven | Report: fallback functions down; games boot and play their known scenes; build time vs B0 | Yes |
| **B3. Incremental builds** ✅ (stable files; dropping kept objects not done) | Builds on the existing build cache: **stable file assignment** (by address range, not packing order), so a changed function rebuilds only its own file; an option to drop the kept objects (1.8 GB) after a successful build | A one-instruction lifter change rebuilds only the affected files | Once (new layout) |
| **B4. Differential testing** ✅ (CI not set up) | Each lifted instruction run against the Mac's own ARM64 CPU, with random inputs; extends `test_lift.py` | Runs in CI; every mismatch is a bug found before a game shows it | No |
| **B5. Faster dispatch** ✅ (inline caches; no separate pre-resolution needed) | Calls between modules resolved ahead of time (imports are known after relocation), inline caches at indirect call sites | Run-time benchmark vs B0 | Yes |
| **B6. Register caching** ⏸ (step 1 in, parked) | Hot guest registers kept in C locals, written back only where a callee or the runtime can see them | Run-time benchmark vs B0; differential tests; games boot and play | Yes |
| **B7. Profile-guided builds** | `NXRT_FNPROF` profiles feed clang: hot code at higher optimisation, cold code at `-O1` for faster builds | Build time and frame time vs B0 | Yes |

B1 comes first because it carries no risk (identical output) and makes all the others easier. B2 and B3 give the
quickest wins for every game. B6 is the big performance project, and should only start once B4 can catch its mistakes.

---

## What each game has to change

| Game | Part A | Part B |
|---|---|---|
| **LAS** | The most work. Eight drawing hooks become one `draw` hook (mirror, tints, boxing, stretch); its pad flip moves into `pad_filter`; `scene_fresh_targets` becomes `scene_changed`; `NXRT_FEATURES += boot-profiles draw-tint screen-filter` and what it ships today; test scripts move to `pad_script`. Re-verify mirror mode, gold pots and bushes, widescreen boxing, the phone box, the randomizer | Nothing in its code. `patches.py` and `VARIANTS` keep their format. One rebuild per output-changing step |
| **Sky** | Small: hooks unchanged; `NXRT_FEATURES := mods texpacks upscalers` | Nothing. Its `inner_entries` may become unnecessary after B2; keep them until it's verified |
| **EoW, Tunic, Dread** | Makefile only (`NXRT_FEATURES` if they want extras) | Nothing |
| **New ports** | Start from `examples/game-template` with `NXRT_FEATURES` empty | Get the new report for free: which tables and functions need attention |

Players notice nothing in features, as long as each game lists today's features in `NXRT_FEATURES`. They notice a
one-time rebuild per recompiler release, and later shorter builds and faster games.

## Order of work

Each step builds every game and checks LAS (the most hooks) before the next. A and B run side by side; B never waits
for A.

| # | Part A (library) | Part B (recompiler) |
|---|---|---|
| 1 | **Remove Vulkan, Windows, Linux** and the x86 / Android paths; keep the seams | **B0** baseline |
| 2 | **Draw actions** in Metal; port LAS; remove the eight old hooks | **B1** restructure, byte-identical |
| 3 | **Leftovers:** test variables, `pad_mirrored`, `scene_changed`, unused hooks, comments | **B2** jump tables |
| 4 | **Extension points:** fs overlay, texture provider, present stage, pipeline cache, start-screen task slot, input output hook | **B3** incremental builds |
| 5 | **Move the extras** behind `NXRT_FEATURES`, one at a time | **B4** differential testing |
| 6 | **Move dev tooling**; update the docs to the new layout | **B5** dispatch, then **B6** registers, then **B7** profiles |

## Risks

- **Draw actions touch the shader translator.** The shader cache key must change with them, or old cached pipelines
  would be reused wrongly. Bump the cache salt.
- **Removing Vulkan** drops Windows, Linux and the Steam Deck from `master`. That's intended for now; `steam_deck` keeps
  its copy.
- **Recompiler changes force rebuilds** for every player of every game: bundle B2, B3, B5, B6 and B7 into as few releases
  as possible.
- **Jump-table recovery and register caching can silently miscompile.** Keep the fallback for unproven tables, and don't
  start B6 before B4's differential tests run in CI.
- **Extension points are new API.** Keep them few, and document them in ARCHITECTURE.md, because extras and games will
  depend on them.
