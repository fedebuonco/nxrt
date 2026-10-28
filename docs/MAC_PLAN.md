# macOS plan: what nxrt takes from nxnative

**Status:** active. **Line:** `macos-core` (worktree `build/nxrt-macos`). **Baseline:** host tests 16/16 pass
(`tests/run.sh`), 2026-10-27.

This is the working plan for the items in [`NXNATIVE_IMPORTS.md`](NXNATIVE_IMPORTS.md). It is ordered by what can be
built and verified **without the user**: no game files, no GUI runs, no visual checks, no device measurements. Anything
that needs those is implemented only as far as it can be compiled and unit-tested, then marked `blocked: user` with the
exact step that is missing.

## Rules

- Every change builds on the Mac line and keeps `tests/run.sh` green (ASan/UBSan).
- New host-side logic gets a unit test in `tests/`; a change that cannot be tested without a game is not "done".
- No game files, saves, keys or ports are added to the repository.
- A step that needs the user is written down here, not guessed at.

## Status

| ID | Item | Phase | Verification | Status |
|---|---|---|---|---|
| P0 | Baseline: build + host tests | 0 | `tests/run.sh` 16/16 | done |
| A1 | Yield back-off (`SleepThread(0)`) | 1 | unit test + host suite | done |
| A2 | Spin-loop gate in the lifter | 1 | FE3H generated with gates (231 chunks), ran and rendered; perf A/B next | done |
| A3 | Per-game shader facts table | 1 | parser/apply/file loader; prepare + draw key paths | done |
| A4 | Sync/memory tests (lost-wakeup control, TSan) | 1 | kernel wait/signal + control + region/heap map; 29/29 under ASan and TSan | done |
| A5 | Benchmark routes + power line | 1 | route/log parser + test; `[power]` line + sensor test | done |
| A6 | Docs symptom index | 1 | `docs/SYMPTOMS.md` + link check | done |
| A7 | Pipeline-key normalisation | 1 | `ShaderRec` canonicaliser + test; key already ignores unused targets | done |
| A8 | QoS refinement (third class, lock lending) | 1 | A/B title + editor: no gain (p95 17.08 vs 17.09); code in, default off | done |
| A9 | Suspend/sleep handling | 1 | `tests/manual/sleep_live.sh` ready; needs a real sleep/wake session | **blocked: user** |
| A10 | Per-game Mac profile (data + parser) | 1 | schema + parser test | done |
| B1 | Compiled shader libraries at setup + working archive | 2 | FE3H 41 libs/48-pipe archive; EoW 60 libs/99-pipe 11.9 MB archive | done |
| B2 | Metal 4 bases / specialisation / archive | 2 | FE3H + EoW gameplay; simple states M4, depth/MRT fall back M3 | done |
| B3 | Prepare strict gate (counts + `NXRT_SHADER_STRICT`) | 2 | FE3H 41/54; EoW 97/227; both exit 1 | done |
| C1 | Hybrid pulling stand-in | 3 | port plan in `docs/C1_C4_SCOPE.md`; 1–3 month shader-transform port | **blocked: user** |
| C2 | Present / encode / refresh measurement | 3 | fullscreen refresh match + mode picker test; present-snap/encode next | doing |
| C3 | Mac app packaging / per-Mac archive | 3 | ad-hoc-signed `dist/nxrt-0.1.0-macos-core.dmg`; notarisation/per-Mac archive next | doing |
| C4 | HLE breadth | 3 | per-title loop in `docs/C1_C4_SCOPE.md`; needs a title calling missing units | **blocked: user** |
| C5 | Survey / function matching | 3 | Dread base→2.1.0: 8,660 unique function matches; surveys on Dread/FE3H/EoW/Sky | done |
| C6 | Per-game notes / docs tests | 3 | docs test + template; FE3H, EoW and Tomodachi notes written | done |
| C7 | Final performance acceptance | 3 | needs games, display, GPU clock, battery | **blocked: user** |

## Needs the user (deferred, not guessed)

- **Game files / a port:** first-use shader counts, archive cold/warm time, pixel parity, hybrid stand-in, HLE,
  survey, per-game notes, final frame times.
- **A display session:** present/encode/refresh, fullscreen, visual checks.
- **Hardware measurements:** GPU clock and residency (`powermetrics`), energy, thermal, sleep/wake.
- **Product decisions:** app name/bundle, signing/notarisation, default settings per title.

Those are marked in the table. The code for B1/B2 can be written and unit-tested without them, but its acceptance is not
claimed until a game run happens.

## To unblock (one session each)

- **B2 / B3 / A2:** run one port with `NXRT_METAL4=1 NXRT_SHADER_STRICT=1`, capture first visits/skips and a screenshot;
  compare `NXRT_METAL4=0` and `=1` on the same route.
- **B1 (#52):** with `NXRT_METAL_ARCHIVE=1` and a game, force a clean prepare, then a second start; confirm the archive
  saves, loads and is complete (the current crash is the macOS 26 writer).
- **A8:** A/B a route with `NXRT_QOS_LEND=0/1`; record frame p95, CPU% and the performance-core share.
- **A9:** a real sleep/wake with a save in progress and audio playing; check save integrity, audio resume and GPU state.
- **C5:** one real `main.nso` (the player's own) for `tools/survey.py` and `tools/nso_fnmap.py`.
- **C6:** the port's notes, using `docs/GAME_NOTES_TEMPLATE.md`.
- **C1 / C2 / C3 / C4 / C7:** the hybrid stand-in, present/refresh measurement, packaging/signing, the next title's HLE
  services, and the final performance run.

## Phase 1 — host-side, no game

1. **A1 yield back-off.** Policy extracted into `runtime/yield_backoff.c`, used by `kernel.c`: after 2 ms of consecutive
   yields with unchanged `x19–x28`, sleep an eighth of the wait (10–250 µs); reset on any other SVC. `NXRT_YIELD_BACKOFF=0`
   off, `<cap_us>[:<start_us>]` tunes it. Unit test covers the policy.
2. **A2 spin gate.** `tools/recomp/loops.py` (nxnative's classifier, conservative) and a gate call on proven back-edges
   in `emit.py`, behind `NXRT_SPIN_GATE=1` at generation; `nx_spin_gate` in `kernel.c` sleeps after 2 ms in the same loop.
   Tested on synthetic and real `ldr/cbnz` code; a game run is still needed to enable it by default.
3. **A3 shader facts.** `games/<id>/shader_facts.tsv` format + parser, consumed by the translator key path; test the
   parser and a key built with and without facts.
4. **A4 sync tests.** Port the `t_condvar` scenario and its negative control, plus heap/alias memory cases; add a TSan
   build target.
5. **A5 benchmark/power.** `tools/bench.py`: `bench.tsv` routes, `[pace]` window selection, percentiles and `--json`;
   a `[power]` line from `proc_pid_rusage` (energy, wakeups). Tests on synthetic logs and a live sensor read. The
   in-game runner and thermal state stay with C2.
6. **A6 symptom index.** `docs/SYMPTOMS.md` for Mac bring-up; link check.
7. **A7 key normalisation.** Extract the pipeline key normalisation into a pure helper; test equivalence classes.
8. **A8 QoS.** Three guest classes in `platform/macos.c` (≤44 interactive, 45–59 initiated, rest utility); lock lending
   in the kernel's waits is the remaining half.
9. **A9 suspend/sleep.** `NSWorkspaceWillSleep` / wake handling: pause audio, drain GPU, checkpoint or exit cleanly;
   compile only.
10. **A10 Mac profile.** Data file + parser for per-game defaults; test the parser.

## Phase 2 — Metal, compile + synthetic tests

1. **B1 compiled libraries.** `tools/shader_libs.py` compiles MSL to `.metallib` (runtime cache names included);
   `renderer.m` loads them with `newLibraryWithURL` and falls back to source. Next: the setup step that translates and
   writes the MSL, and compiled functions in the Metal archive for #52.
2. **B2 Metal 4 bases.** On macOS 26+, build a base per shader pair/depth mode and specialise per draw; keep the Metal 3
   path. Compile-only until a game run.
3. **B3 strict gate.** Coverage counters and `NXRT_SHADER_STRICT`; unit-test the counters.

## Phase 3 — blocked

C1–C7 above. They are listed so the plan is complete; they are not started until the user provides a game run, a
display session or the product decisions.

## Log

- 2026-10-27: baseline 16/16; plan. A1 done: `runtime/yield_backoff.c/.h`, `kernel.c` SleepThread(0) run + x19–x28 progress, `NXRT_YIELD_BACKOFF`,
  `tests/test_yield.c` (10 checks); full suite 17/17; template app builds.
- 2026-10-27: A3 parser + `shader_fact_apply` (`runtime/nvn/shader_facts.c/.h`, 38 checks); loader per game is next.
  A6 `docs/SYMPTOMS.md` for Mac bring-up. Full suite 18/18.
- 2026-10-27: A2 classifier + capstone back-edge finder (`tools/recomp/loops.py`), gate emission in `emit.py` behind
  `NXRT_SPIN_GATE=1`, runtime gate in `kernel.c`; `test_loops` (synthetic + real `ldr/cbnz`) and a generated-C gate check.
  A5 `tools/bench.py` (routes, `[pace]` window, percentiles), `[power]` line (`platform/power_macos.c`), `test_bench` +
  `test_power`. Full suite 21/21; template app builds.
- 2026-10-27: A10 `tools/mac_profile.py` (scale/fps/cap/upscaler/fullscreen/refresh/tdp schema, defaults, validation);
  `test_mac_profile`. A8 three-class guest QoS in `platform/macos.c` (lock lending still in the kernel). Full suite 22/22.
- 2026-10-27: A3 done: `shader_facts_load` (file), loaded once from `NXRT_SHADER_FACTS` or `<data root>/shader_facts.tsv`,
  applied in `renderer_prepare` and on the draw path (`NvnProgram.vs_hash/fs_hash`); `test_shader_facts` 43 checks.
- 2026-10-27: B1 started: `tools/shader_libs.py` compiles MSL to `.metallib` (normal and `--cache-dir` <hash> names);
  `renderer.m` loads `cache/shaders/<hash>.metallib` with `newLibraryWithURL` and falls back to source; `test_shader_libs`.
  Setup wiring (translate → write MSL → compile) and archive function entries remain. Full suite 23/23.
- 2026-10-27: B1 setup step: `NXRT_MSL_DUMP=1` writes `cache/msl/<hash>.metal`; `shader_libs.py` compiles them into
  `cache/shaders`; `shader_source_key` shared by runtime/tool with vectors (`test_shader_key`). The Metal archive now
  adds/reads compiled functions; a synthetic function+pipeline archive serializes and reloads on this Mac. #52 remains
  off by default. Full suite 24/24.
- 2026-10-27: B3 strict gate: `runtime/nvn/shader_strict.*` counts first visits and skipped draws; `shader_first_visit`
  and `skip_note` feed it; `NXRT_SHADER_STRICT=1` logs and exits 1 at shutdown. `test_shader_strict`; full suite 25/25.
- 2026-10-27: B1 archive load safety: `runtime/nvn/archive_file.*` checks the Mach-O magic before Metal opens the
  archive; `archive_open` discards a foreign/truncated file and starts a new one. `test_archive_file`; full suite 26/26.
- 2026-10-27: A7 done: `runtime/nvn/shader_rec_norm.*` clears state that cannot affect the result (targets without a
  format, blend factors when blending is off, stale tail entries), applied in `renderer_prepare` and `rec_from_state`;
  `test_shader_rec_norm`. Full suite 27/27.
- 2026-10-27: B2 foundation: `tests/manual/test_mtl4.m` builds an unspecialised Metal 4 render pipeline and specialises
  it (BGRA8, blending off) on this Mac (macOS 26.3.1). The renderer integration is next; full suite 27/27.
- 2026-10-27: B2 integration: `renderer.m` has a Metal 4 path (base per program/vertex layout + per-colour-state
  specialisation) behind `NXRT_METAL4=1`, with the Metal 3/archive fallback. `tests/manual/test_mtl4_draw.m` proves a
  Metal 4 pipeline draws through the existing `MTLRenderCommandEncoder`. Full suite 27/27; a game/pixel check remains.
- 2026-10-27: A9 start: `platform/sleep_macos.m` observes `NSWorkspaceWillSleep/DidWake`; `renderer_init` registers a
  callback that flushes GPU work before sleep. `tests/manual/test_sleep.m` posts the notifications and checks the
  callback. Audio pause/restore and a real sleep remain. Full suite 27/27.
- 2026-10-27: A8 lock lending: a waiter's QoS raises the mutex owner through a `pthread_override_qos_class` while it
  holds the lock (started in ArbitrateLock/condvar wait, ended on release), behind `NXRT_QOS_LEND=1`. Compile-only; the
  scheduler measurement is next. Full suite 27/27.
- 2026-10-27: A9 audio: `audio_suspend()` tracks the open SDL streams and pauses/resumes them; the sleep callback now
  pauses audio and flushes the GPU (resumes audio on wake). Linked into the app; full suite 27/27. Restore and a real
  sleep remain.
- 2026-10-27: A4 TSan: `tests/run-tsan.sh` runs the whole host suite under ThreadSanitizer; 27/27 pass. The lost-wakeup
  control and the kernel memory tests still need a kernel harness.
- 2026-10-27: B1 manual probe: `tests/manual/test_metal_archive.m` adds a function + pipeline to an archive, serializes
  and reloads it (file written, exit 0). #52's save crash remains off by default.
- 2026-10-27: A4 kernel test: `tests/test_kernel_sync.c` compiles `kernel.c` and drives `svcWaitProcessWideKeyAtomic` /
  `svcSignalProcessWideKey` for 200 iterations; a negative control reproduces the old order and must time out. TSan and
  ASan/UBSan: 28/28. Heap/alias memory cases remain.
- 2026-10-27: A4 done: `tests/test_kernel_memory.c` checks region lookup/gaps, the heap at `HEAP_BASE` and the `HEAP_MAX`
  cap (15 checks). Full suite 29/29 under ASan/UBSan and TSan.
- 2026-10-27: C5 start: `tools/survey.py` reports build id, segment sizes, imports/exports and HLE coverage from an NSO;
  `test_survey` uses a fake NSO. Function matching and a real-title survey remain. Full suite 30/30 (ASan and TSan).
- 2026-10-27: C5 matching: `tools/fnmap.py` (length-tagged FNV, unique matches only) and `tools/nso_fnmap.py` (NSO
  CLI over `.eh_frame` functions); `test_fnmap`. Full suite 31/31 (ASan and TSan).
- 2026-10-27: C6 docs: `tests/test_docs.py` checks every relative link in the root docs and `docs/*.md` (71 links);
  the stale Deck comparison copy was removed from this worktree. Full suite 32/32 (ASan and TSan).
- 2026-10-27: FE3H validation (port `B=build-macoscore`): M3 and M4 title screenshots both render; M4 initially black
  because the base left the colour format invalid (fix: concrete format in the base) and because `g_m4_bases` was mutated
  from parallel builds (fix: `dispatch_once` + `@synchronized`). Strict gate exits 1 with 41 first visits / 54 skips.
  Archive saves 48 pipelines/1.1 MB and loads on the next start; fixed the archive guard to accept Metal's `cb fe ba be`
  magic, and `renderer_shutdown` saves before a screenshot `_exit`. Host suites 32/32. Port game libraries restored.
- 2026-10-27: FE3H menu screenshot on M3/M4; M4 draw probe now reads back a red pixel (`0,0,255,255`) with a concrete
  base format, confirming the fix. The route input timing differs between runs (M3 reached the difficulty menu, M4 the
  main menu); a deterministic gameplay route is still needed for pixel parity.
- 2026-10-27: A2 validated on FE3H: `NXRT_SPIN_GATE=1 make gen GEN=generated-spin` put gates in 231 chunks; the spin
  build ran and rendered (`NXRT_SPIN_GATE=1`, M4). A frame-time A/B is still needed before enabling by default.
- 2026-10-27: B1 end-to-end: `NXRT_MSL_DUMP=1` wrote 41 MSL files; `tools/shader_libs.py --cache-dir` produced 41
  `.metallib`; the next run loaded them (5+ `[metal] compiled library` lines) and rendered the title. Fixed the old text
  cache cleanup, which deleted the `.metallib` files (it now removes only `.metal`).
- 2026-10-27: C6 notes: `/Volumes/FE3HDev/fe3h/VALIDATION.md` and `/Volumes/FE3HDev/tomo/VALIDATION.md` (FE3H validated;
  Tomodachi crashes in guest code before the first frame, reproduced with yield/QoS off).
- 2026-10-27: A8 A/B on the FE3H title (`NXRT_QOS_LEND=0/1`, 10 s window): 59.3 vs 59.4 fps, period p95 16.99 vs
  16.98 ms — no measurable difference on a CPU-light title; a gameplay scene is needed.
- 2026-10-27: B2 gameplay: the M4 route faulted the GPU (page fault) once FE3H reached the character editor. Restricted
  `metal4_pipeline` to single-colour, no depth/stencil, single-sample states; the same 5000-frame route then reached the
  editor with 0 command-buffer errors, and depth/MRT pipelines use Metal 3. Title/menu/gameplay all render.
- 2026-10-27: A8 A/B on the FE3H character editor (15 s window): 59.32 vs 59.30 fps, period p95 17.08 vs 17.09 ms, cost
  p95 11.82 vs 13.01 ms. No gain; lock lending stays off by default and needs a genuinely CPU-bound scene.
- 2026-10-27: A9 harness: `tests/manual/sleep_live.sh` + `docs/A9_SLEEP_TEST.md` launch FE3H, ask for a real sleep/wake,
  and report `[sleep]`, audio and crash lines. C5: the only second FE3H module on disk is nxnative's raw
  `main.patched.bin` (not an NSO), so the matcher still needs a second NSO build/version.
- 2026-10-27: Second title: built EoW from `/Volumes/FE3HDev/eow-gen` into `/Volumes/PortableSSD/eow-macoscore`.
  Metal 3/4 render the Suthorn Ruins; archive 99 pipelines/11.9 MB save+load; strict 97 first visits/227 skips;
  60 MSL→60 metallibs loaded. `shader_libs.py` now skips AppleDouble `._*.metal` files (tested). EoW survey: 711
  imports/7 HLE; Sky survey: 346/7. Notes: `/Volumes/PortableSSD/eow-macoscore/VALIDATION.md`.
- 2026-10-27: C5 second version: the EoW base NSP extracts to the same v1.0.0 build id `a581...` as the game folder;
  there is no EoW 1.1.0 update NSP on the mounted volumes, so the matcher still has no real second build. The nxrt
  0.1.0 DMG was opened for reinstall; it predates the macos-core changes and does not contain them.
- 2026-10-27: C5 real match: Metroid Dread base v1.0.0 (`main` build `49161d9c...`) vs update 2.1.0 (`main` build
  `646761f6...`), both extracted to `/Volumes/PortableSSD/dread-base` and `/Volumes/FE3HDev/runs/dread-recomp`:
  **8,660 exact unique function matches of 102,288 old / 102,993 new** (`tools/nso_fnmap.py --json`).
- 2026-10-27: C2 start: `platform/display_match.c` (pure mode picker) + `display_macos.m` switch a fullscreen screen to
  the highest mode that is a multiple of the frame rate (`NXRT_SCREEN_MATCH=0` off); `test_display_match`; wired into
  `metal.m` window/present paths. EoW fullscreen at 60 Hz reports "already a multiple of 60 fps". Host suite 33/33.
- 2026-10-27: C3 start: packaged the current runtime source into an ad-hoc-signed `nxrt.app` and a verified DMG at
  `dist/nxrt-0.1.0-macos-core.dmg`; opened it. Notarisation and the per-Mac pipeline archive remain.
- 2026-10-27: C1/C4 scoped from nxnative's `nvn_mtl.m`/`dmsh_pull.py`: `docs/C1_C4_SCOPE.md`. C1 needs the MSL
  `_bp` transform, layout block and non-waiting pipeline selection (1–3 months); C4 is per-title (survey, trace,
  implement one unit, test).
- 2026-10-27: C5 on a real title: `tools/survey.py` on FE3H `main` (554 imports, 8 HLE covered, 22 exports) and `sdk`
  (13 imports, 24753 exports). The matcher still needs two builds of one module.

- 2026-10-27: renderer refactor start: archive split (`renderer_archive.m`), CI, `tools/smoke.py` (FE3H/EoW),
  headless GPU runner. Fixed FE3H M4 black menu quads by falling back to Metal 3 for blending pipelines; 34/34 host
  tests, both smoke tests pass. See `docs/RENDERER_REFACTOR.md`.
- 2026-10-27: renderer context + cache extracted (`renderer_context.m`, `renderer_cache.m`); FE3H/EoW smoke pass,
  EoW archive saves/loads 99 pipelines/9.4 MB, 34/34 ASan and TSan. See `docs/RENDERER_REFACTOR.md`.
- 2026-10-27: renderer pipeline/Metal 4 modules extracted (`renderer_prog.m`, `renderer_metal4.m`, `renderer_pipelines.m`);
  FE3H/EoW smoke pass, archive saves/loads, 34/34 ASan and TSan. renderer.m 3,500 lines.
- 2026-10-27: renderer split: context/cache/Prog/Metal4/pipeline creation extracted; fixed an async-compile stat-lock
  regression (pthread mutex) that darkened EoW; FE3H/EoW smoke and archive pass; 34/34 ASan and TSan.
- 2026-10-27: headless renderer context/cache test added (`tests/manual/test_renderer_context.m`, run by
  `tests/run-gpu.sh`). Next renderer slice is prepared objects/pass encoding.
- 2026-10-27: renderer.h narrowed (input declarations moved to input/input_host.h), `tools/build_port.sh fe3h|eow`
  added; FE3H/EoW smoke pass, 34/34 ASan and TSan.
- 2026-10-27: renderer state (`st`) and pass geometry moved into the context/pass module; FE3H/EoW smoke pass,
  34/34 ASan and TSan. `renderer.m` 3,820 lines.
- 2026-10-27: texture-view resource module extracted (`renderer_textures.m`); renderer.m 3,694 lines. FE3H/EoW smoke,
  archive 99 pipelines, 34/34 ASan and TSan.
- 2026-10-27: clear path extracted (`renderer_clears.m`); renderer.m 3,558 lines. FE3H/EoW smoke, archive 99 pipelines,
  34/34 ASan and TSan.
- 2026-10-27: deferred-clear state moved to `renderer_pass.m`; renderer.m 3,461 lines. FE3H/EoW smoke, archive 99
  pipelines, 34/34 ASan and TSan.
- 2026-10-27: `begin_pass`/`end_pass` moved to `renderer_pass.m` with debug/capture/stats callbacks in renderer;
  renderer.m 3,365 lines. FE3H/EoW smoke, archive 99 pipelines, 34/34 ASan and TSan.
- 2026-10-27: format helpers extracted (`renderer_formats.m`); renderer.m 3,234 lines. 34/34 ASan and TSan.
- 2026-10-27: sampler helpers extracted (`renderer_samplers.m`); EoW's darker frame reproduced on the pre-refactor
  binary too, so it is game/save state, not a refactor regression. renderer.m 3,169 lines; 34/34 ASan and TSan.
- 2026-10-27: program translation/cache extracted (`renderer_programs.m`); renderer.m 3,037 lines. FE3H/EoW smoke,
  34/34 ASan and TSan; archive save works.
- 2026-10-27: shader-facts loader extracted (`renderer_facts.m`); renderer.m 3,014 lines. FE3H/EoW smoke, 34/34
  ASan and TSan.
- 2026-10-27: texture-copy path extracted (`renderer_copies.m`); renderer.m 2,730 lines. FE3H/EoW smoke, archive,
  34/34 ASan and TSan.
- 2026-10-27: dummy/shadow texture helpers and swizzle views moved to `renderer_textures.m`; renderer.m 2,642 lines.
  FE3H/EoW smoke, 34/34 ASan and TSan.
- 2026-10-27: prepared program/pipeline/compute tables extracted (`renderer_prep.m`); renderer.m 2,608 lines.
  EoW full Suthorn frame (658 KB), archive 99 pipelines, 34/34 ASan and TSan.
- 2026-10-27: compute program class/translation/compilation extracted (`renderer_compute.m`); renderer.m 2,501 lines.
  FE3H/EoW full frames, 34/34 ASan and TSan.
- 2026-10-27: sampled_texture moved to `renderer_textures.m`; renderer.m 2,472 lines. FE3H/EoW smoke, 34/34 ASan
  and TSan.
- 2026-10-27: shader-texture lookup/buffer slots/capture moved to `renderer_bind.m`; renderer.m 2,439 lines.
  FE3H/EoW smoke, 34/34 ASan and TSan.
- 2026-10-27: bind_cbufs moved to `renderer_bind.m`; renderer.m 2,396 lines. FE3H/EoW full frames, 34/34 ASan and
  TSan.
- 2026-10-27: snapshot validity extracted (`renderer_snapshots.m`); renderer.m 2,388 lines. FE3H/EoW full frames,
  34/34 ASan and TSan.
- 2026-10-27: is_attachment moved to `renderer_bind.m`; feedback extraction attempted and reverted. renderer.m 2,383
  lines, 34/34 ASan and TSan.
- 2026-10-27: feedback state/resolve moved to `renderer_bind.m` with explicit bindings; renderer.m 2,316 lines.
  FE3H/EoW full frames, 34/34 ASan and TSan.
- 2026-10-27: bind_textures moved to `renderer_bind.m`; texture metadata/draw_watched moved to context. renderer.m 2,199
  lines. FE3H/EoW full frames, 34/34 ASan and TSan.
- 2026-10-27: compute dispatch moved to `renderer_compute.m`; renderer.m 2,087 lines. FE3H/EoW full frames,
  34/34 ASan and TSan.
- 2026-10-27: final validation: 34/34 ASan, 34/34 TSan, GPU context/M4/archive tests, FE3H/EoW smoke. Present is
  already in metal.m (backend boundary); renderer.m 2,087 lines. Next: CPU shadow/LTO evaluation.
- 2026-10-27: CPU codegen evaluation (`docs/CPU_CODEGEN_EVAL.md`): O1 vs O2 on a real 1.22 MB generated module
  (+2.3% code, +11% compile), no CPU-bound evidence on FE3H/EoW; shadow-register/LTO work deferred until a
  CPU-bound profile. Renderer split + present boundary + validation complete.
- 2026-10-28: onboarding pass started. Added `tools/onboard.py`, `tools/crash_report.py`, tests and
  `docs/ONBOARDING.md`; Tomodachi port is structurally ready and crashes at guest `f_7000178b00`.
- 2026-10-28: onboarding diagnostics: `FNPROF=1` wired into nxrt.mk, `NXRT_SP_CHECK` guest-entry SP guard added;
  Tomodachi entries all valid but the first stack store still faults. Host suite 36/36 ASan and TSan.
- 2026-10-28: Tomodachi onboarding: SP guard ruled out stack corruption; crash_report now flags a bad pointer
  argument (`x1=1` in `f_700223302c`). Next: producer of that field. Tests 36/36.
- 2026-10-28: onboarding goal closed with Tomodachi documented blocker (no verified frame): virtual call at
  main_023.c:10029 passes x1=1; x1 is already 1 at L_700017a590. Host suites 36/36.
