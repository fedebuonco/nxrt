# Renderer refactor

Goal: split `runtime/nvn/renderer.m` into modules behind a narrow `renderer.h`, with a renderer context instead of
the largest globals, plus headless tests and CI. Keep behavior identical and the host suites green.

## Map (2,087 lines after compute dispatch)

| Lines (start) | Section | Target module |
|---|---|---|
| 64 | state / init / stats | `renderer_context` + `renderer_cache` (done) + `renderer_state` |
| 729 | textures with views | `renderer_textures` |
| 889 | render passes | `renderer_pass` |
| 1052 | clears | `renderer_pass` |
| 1230 | draw support (formats, vertex) | `renderer_draw` |
| 1430 | binary archive | `renderer_archive.m` (done) |
| 1431 | Metal 4 | `renderer_metal4.m` (done) |
| 1583 | Metal 4 bases/specialisation | `renderer_metal4.m` (done) |
| 1866 | prepared objects | `renderer_pipelines.m` (pipeline creation done) |
| 2510 | compute | `renderer_compute` |
| 3577 | texture copies | `renderer_textures` |
| 3868 | executor | `renderer_exec` |
| 4163 | shader preparation | `renderer_pipelines` |

`renderer.h` stays the only header metal.m uses; internal modules get `renderer_internal.h` (Prog, RtState,
RendererContext) rather than including renderer.m's statics.

## Done

- **Pipeline/Metal 4 modules**: `renderer_prog.m` (the `Prog` class), `renderer_metal4.m` (M4 compiler, bases,
specialisation), `renderer_pipelines.m` (archive render/compute pipelines, compiled libraries, `compile_program`).
  `renderer.m` calls `renderer_compile_program`, `renderer_archive_render_pipeline` and `renderer_metal4_*`.
- **Copies**: `renderer_copies.m/.h` owns buffer↔texture and texture↔texture copies (blit or full-screen pass).
- **Shader facts**: `renderer_facts.m` loads the per-game fact table once and exposes it to prepare/draw.
- **Programs**: `renderer_programs.m/.h` owns program translation, the `prog_tab` cache, content keys and lookup.
- **Samplers**: `renderer_samplers.m/.h` owns sampler-state conversion and slot assignment.
- **Formats**: `renderer_formats.m/.h` owns NVN→Metal vertex, blend, compare, stencil and render-target kind conversion.
- **Pass encoding**: `renderer_pass.m` owns `begin_pass`/`end_pass`, the `td_*` deferred-clear table, geometry and
  `renderer_texture_will_read`; the debug/capture/stats callbacks stay in renderer (`renderer_pass_debug_*`,
  `renderer_pass_gpu_label`, `renderer_stat_*`).
- **Clear path**: `renderer_clears.m/.h` owns the full-screen clear quads, recorded-clear entry and pipeline formats.
- **Snapshots**: `renderer_snapshots.m` owns snapshot validity/invalidation and reasons.
- **Texture binding**: `renderer_bind.m` now owns `bind_textures`; per-stage texture scale/fraction/size and `draw_watched`
  live in the context.
- **Feedback**: `renderer_bind.m` now owns the feedback copies, snapshot-copy table, current FS hash and
  `resolve_feedback`; the earlier reverted attempt is superseded.
- **Binding**: `renderer_bind.m` also owns `is_attachment`; feedback extraction was attempted and reverted (too coupled).
- **Present**: `backend_present`/`backend_represent`/`backend_present_now` already live in `metal.m`, not renderer.m;
  they stay there as the Metal/window boundary. `renderer.h` only exposes the small backend calls metal.m needs.
- **Compute dispatch**: `renderer_compute.m` now owns `dispatch_compute` as well as program lookup/compilation.
- **Compute programs**: `renderer_compute.m/.h` owns `CsProg`, compute translation/compilation and lookup; dispatch stays in
  renderer for now.
- **Prepared tables**: `renderer_prep.m/.h` owns the prepared program/pipeline/compute dictionaries and pending counter.
- **Texture resources (sampled)**: `renderer_textures.m` also owns `sampled_texture` (buffer textures, type views, swizzle).
- **Texture resources (helpers)**: `renderer_textures.m` also owns dummy/shadow textures, depth-comparable checks and
  swizzle views.
- **Resources**: `renderer_textures.m/.h` owns the texture-view cache, format-view compatibility and `view_texture`.
- **Pass geometry**: `renderer_pass.m/.h` owns `pass_size` and `apply_scissor`, reading the size/scissor state from the context.
- **Renderer state**: `st` (bound targets, programs, attributes, viewport/scissor) moved from renderer.m into
  `RendererContext`; the pass and draw state are no longer file globals.
- **Renderer context**: `renderer_context.h/.m` owns `dev`, the current command buffer/encoder, the clear pipelines and
the shared cache; `renderer.m` uses short macros while the modules are extracted. `renderer_context_release()` drops the
retained Metal objects for a headless test.
- **Cache module**: `renderer_cache.h/.m` owns program/pipeline/depth-stencil lookups and clearing; renderer.m no longer
touches the dictionary directly.
- **Archive module**: `runtime/nvn/renderer_archive.m/.h` (167 lines). It owns the archive, its tag, lock, save
  timer and counters; `renderer.m` calls `renderer_archive_open/get/add/save` and the counter getters. Verified:
  EoW saves 99 pipelines/9.4 MB and loads them next run; FE3H smoke passes.
- **Visual fix**: FE3H's Metal 4 menu had opaque black quads behind the text. Blending-enabled pipelines now fall
  back to Metal 3; the screenshot is clean and EoW still passes.
- **CI**: `.github/workflows/macos.yml` runs ASan/UBSan, TSan and the template build on macOS.
- **Port build**: `tools/build_port.sh fe3h|eow` builds a real port against this checkout; `SMOKE=1` also runs the
  screenshot check.
- **Smoke test**: `tools/smoke.py` runs FE3H/EoW to a screenshot and rejects crashes, GPU command-buffer errors and
  black frames; `tests/test_smoke.py` tests the checker. Both games pass.
- **Narrow renderer.h**: host-input declarations moved to `input/input_host.h`; `renderer.h` no longer pulls in SDL.
- **Headless GPU tests**: `tests/run-gpu.sh` runs `test_renderer_context` (context/cache lifecycle), `test_mtl4`,
  `test_mtl4_draw` (red-pixel readback) and `test_metal_archive` with no window.

## Fixed during the split

- Removing the old `@synchronized(cache)` compile-stat lock exposed a rendering regression: an
  `@synchronized([NSNull null])` lock in the async compile path left EoW dark. Replaced with a plain
  `pthread_mutex_t` in `renderer_note_compile`; EoW is back to the full Suthorn Ruins scene and archive works.
- Extracting the texture-view cache was tried and reverted (it did not fix the async issue; only the lock did).

## Tried and reverted

Extracting Metal 4 alone carried the archive-pipeline functions (`archive_render_pipeline`,
`archive_compute_pipeline`, `compile_program`) with it; it needs the shared context/`Prog`/`dev` first. Reverted
cleanly; only the archive slice is in.

## Next order

1. Move the prepared-object tables (`g_prep_*`) into `renderer_pipelines.m`.
2. Move compute/copies and present; `renderer.h` remains the narrow metal.m boundary.
3. Keep `renderer.h` as the metal.m boundary; add internals only as needed.
