# Plan: finishing the beta_5 NVN port

Companion to [HANDOFF_NVN.md](HANDOFF_NVN.md). That document says where the work stands; this one says what
happens next, in what order, and what proves each step.

Two corrections to the handoff, found by reading `nvn-renderer-wip` (commit `33c13c0`):

- **The render queue is not started by `main.c`.** `rq_on()` starts the render thread lazily (`pthread_once`), and
  `backend_init()` is called from `nvnDeviceInitialize`. "Nobody drains the queue" is an unverified guess; reproduce
  the hang before restructuring the init path.
- **The game interface is not fully back.** `draw_tint`, `instance_tint` and `instances_tinted` are never called:
  the tint buffer is zero-filled and the per-instance tint runs were dropped. LAS implements both hooks
  (`games/las/runtime/las_game.c`), so a build passes and a screenshot can look mostly right while pots/items lose
  their tint. The window title is still `"LAS Recomp"` in both backends, and `ui_window_init` no longer calls
  `nxui_metal_setup` (the start-screen overlay wiring).

Headers did land with the import (`ShaderKey`/`ShaderInfo` incl. `ssbo_read`), so handoff step 1 is mostly done.
WIP's `shader.h` lacks the `alpha_func`/`dual_source` fields that `beta5-merge`'s Vulkan path uses; the translator
choice (beta_5's vs nxrt's, `b89db46`) is decided by a screenshot on the new renderer, not by argument.

## Phases

### 1. Reproduce and explain the hang

Build WIP beside the working `beta5-merge` build, without touching it:

```zsh
git switch nvn-renderer-wip
cd games/las && export SDKROOT=$(xcrun --show-sdk-path)
make -j10 B=build-wip CLANG=/usr/bin/clang
cp build/lasgame.dylib build/lasgame.dylib.id build-wip/   # game code unchanged: no 10-15 min rebuild
NXRT_HANG_DUMP=8 NXRT_LOG_PACE=1 NXRT_DATA_ROOT=/tmp/d ./build-wip/lasrecomp
```

`/tmp/d/port.ini` points at the read-only LAS folder (`game_dir`, `game_in_place = 1`). Capture the log and
`sample $(pgrep lasrecomp) 5`; self-test the hang dump first with `NXRT_RQ_STALL_AT` so silence is not read as "no
hang". Candidates: `ui_ready` (device init waits for the window), `renderer_init`'s cache load, Metal library
compilation, the first submit's `rq_wait_item`, the perf-core/QoS helpers.

Gate: a stack sample naming the blocked thread, not a theory.

### 2. Metal: first frame

Smallest fix that presents. Gate: `NXRT_SHOT_PATH` screenshot of the game's first scene, then LAS boot-to-menu.
"Builds" is not a gate.

### 3. Interface completeness (Metal)

Re-add `draw_tint` and the instance-tint run splitting to beta_5's draw path (port from `beta5-merge`'s
`runtime/nvn/renderer.m`), verify `tex_identify`/`tex_generation`/`tex_apply`/`tex_replace` call counts, add a
temporary `NXRT_LOG_HOOKS` counter so "no hooks" cannot pass silently, set the window title from `nx_game.info.title`,
restore `nxui_metal_setup`. Gate: LAS with tinted pots/items and the Hint Sheet icon, no LAS strings in `runtime/nvn/`.

### 4. Vulkan

Same interface audit in `vulkan/exec.c` and `record.c`/`rqueue` interaction. Cannot be verified on this Mac
(macOS is Metal only): needs a Linux/Windows run, or compile-only here plus a tester.

### 5. The rest of beta_5

kernel.c (gate: EoW boots, see `e481c2a`), then `tex_hash`, `shader_prep`/`seen` (with the UI wiring), `pace`,
`slowframe`. One commit each, each with a named game as gate.

### 6. Sky's stall (independent)

`svc_wait_process_wide_key` / `svc_signal_process_wide_key` in `runtime/kernel.c`: the waiter is registered and never
woken. Gate: Sky passes 32 frames and keeps presenting.

### 7. Docs and release

Update both handoff docs; tag `v0.2.0` only after the owner runs all three ports.

## Rules

- Game folders are read-only; test through `/tmp` data roots with `game_in_place`.
- App-only rebuilds (~1 min) first; game libraries (10-15 min) only when generated code changes.
- Keep `beta5-merge` runnable: build WIP into separate build dirs (`B=build-wip`).
- No game files, keys, generated C, dylibs, ids or saves in commits.

## Order

Phase 1 and Phase 6 are independent and can run in parallel; Phase 2 blocks 3 and 4; Phase 5 last. Estimated:
1-2 days for 1-3, a day for 4, 2-3 days for 5, a day for 6.
