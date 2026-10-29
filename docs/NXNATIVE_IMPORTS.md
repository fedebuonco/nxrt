# What nxrt can take from nxnative

**Target matters.** On macOS, nxrt and nxnative run on the same CPU family and the same graphics API. nxnative's Metal
pipeline work is not an analogy for what nxrt could do — it is a reference implementation of the same problems on the
same hardware. This page is therefore the macOS plan only.

It covers the **macOS** line (`macos-core`); the paths below are the Mac line. nxrt has already taken one thing from
nxnative: the NVN device-info values and the API-version rule in `runtime/nvn/objects.c` are credited to nxnative in
[`THIRD_PARTY.md`](../THIRD_PARTY.md). Everything below is the same exercise at a larger scale.

## 0. Already on macos-core — do not re-import

| nxnative mechanism | nxrt equivalent today |
|---|---|
| Metal binary archive | `MTLBinaryArchive` in `cache/shaders.metalarchive`, tagged by build / translator / GPU / macOS; **off by default** because of the macOS 26 writer crash (#52) |
| Prepare step | `renderer_prepare` builds the shipped/seen variants on worker threads at the start screen (`runtime/nvn/renderer.m` and [`shader_prep.c`](../runtime/nvn/shader_prep.c) (Mac line)) |
| Encode/render thread | `rqueue` render thread: translation queues work, the thread encodes/submits/presents |
| Guest thread QoS, real-time presenter | `platform/macos.c` (Mac line): priority ≤ 44 → user-interactive, else user-initiated; `THREAD_TIME_CONSTRAINT_POLICY` for the presenting thread |
| Scripted runs | `NXRT_INPUT`, `NXRT_SHOT_AT`, `NXRT_GPU_PASSES`, `NXRT_LOG_PACE` |
| Build cache and resume | [`gamebuild.c`](../runtime/gamebuild.c) |
| Asserted per-build cuts | `patches.py` `PATCHES` / `VARIANTS` |
| NVN device info / API version | Already imported from nxnative (`objects.c`) |

## 1. macOS Tier 1 — small to medium

| Import | nxnative reference | nxrt today | What the change is | Effort |
|---|---|---|---|---|
| Compiled shader libraries at setup | Offline shader build: MSL compiled to Metal libraries at pack time, shipped in the app | `newLibraryWithSource` at prepare; if macOS's shader cache is cold, the MSL compile is ~300 s of CPU. The archive holds only pipeline states (~1 s), not the compiled functions | Compile the MSL once at setup and persist the **functions/libraries** (in the archive or a `.metallib`), not just pipelines. Load them at play; fall back to compiling when the toolchain is absent | 2–4 weeks |
| Metal 4 bases, specialisation, archive | `MTL4Compiler`, a base per shader pair/depth mode, µs specialisation per draw, `MTL4Archive` with validation | A full `MTLRenderPipelineState` per state (3–5 ms for an unseen one); Metal 3 archive off (#52) | On macOS 26+: build bases once, specialise per draw, archive the bases. Keep the Metal 3 path as fallback. nxnative's binary-only capture and archive fallback are the reference for #52 | 4–8 weeks |
| Pipeline key normalisation | `pso_key_norm`: drop blend factors when blending is off, and the blend/mask bits of targets without a format; keep merged-pass logical state | `pipeline_key` includes the full descriptor state, so equivalent draws create duplicate pipelines and recorded keys miss live ones | Normalise the key the same way, with nxrt's merged-pass/MRT cases preserved; measure pipeline count and archive size | ~1 week |
| Yield back-off | `hle_thread.c`: after 2 ms of consecutive yields, sleep an eighth of the wait (10–250 µs); `x19–x28` changed means progress | `SleepThread(0)` is a bare `sched_yield()` ([`kernel.c`](../runtime/kernel.c)) | Per-thread run + progress check; reset on any other SVC. nxnative measured this on the Mac (Three Houses: 78% → 19% of a core) | 1–2 days |
| Spin-loop gate in the lifter | `patch_nso.py` "Spin waits": gate a loop whose paths back do only loads/arithmetic/branches, with a limited lock/store allowance | A guest spin loop compiles into a C loop that spins on a core | Classify the guest CFG in `gen.py`, emit a gate call on the back-edge; start with detection, then gate. nxnative measured the gate on the Mac (Paper Mario: frame p95 29–37 → 18–22 ms) | 1–2 weeks |
| Shader facts table | `shader_facts_manual.tsv`: per-hash JSON pinning attribute/RT kinds the code cannot tell | Canonical `ShaderKey` guess; the same shader can need several translations | Per-game `shader_facts.tsv`, used by static translation and the draw-time key. Link's Awakening's palm trees are nxnative's example | days + data |
| Sync/memory tests | `t_condvar` with a lost-wakeup negative control, `t_memory`, `t_heap_size` | 13 host tests, none for kernel synchronisation | Port the scenarios and the negative control; add TSan for audio/kernel races | 1–2 weeks |
| Benchmark + power | [`bench.py`](https://github.com/seedds/nxnative/blob/main/scripts/bench.py), [`bench.tsv`](https://github.com/seedds/nxnative/blob/main/dm/games/01007EF00011E000/bench.tsv): route, window, variants, runs, percentiles, screenshots; a power line (energy, wakeups, thermal, top yielding threads) | Scripted walks, screenshots, `[pace]`, `[gpu-passes]`; no route schema, variant comparison or in-app energy line | Add `games/<id>/bench.tsv`, a harness that drives `NXRT_INPUT`, writes `frames.tsv` / `results.json` / `results.md`, and a `[power]` line from `proc_pid_rusage` / `NSProcessInfo.thermalState` | ~1 week |
| Docs symptom index | `docs/agents/symptoms.md`: log text → entry → cause/fix/found-on | Architecture docs and `tools/log_triage.py`; no index | Start `docs/SYMPTOMS.md` for Mac bring-up; one entry per symptom | days, ongoing |

**Why the shader work comes first.** On the Mac the expensive thing is not building a pipeline from compiled functions
(~1 s for all 895 variants); it is compiling the MSL when macOS's shader cache does not have it (~300 s). nxnative moves
that compile to pack time and ships the result. nxrt can move it to setup, on the player's own Mac, which avoids both
the per-launch cost and any portability question. That is a bigger win than the pipeline archive alone, and it makes the
archive worth having.

**Why Metal 4 is second.** A base plus µs specialisation removes the 3–5 ms build for a state the preparation missed.
It only applies on macOS 26 and later; below that, the Metal 3 archive plus the hybrid stand-in (§2) are the fallback.

## 2. macOS Tier 2 — weeks to months

| Import | nxnative reference | nxrt today | What the change is | Effort |
|---|---|---|---|---|
| Hybrid pulling stand-in | `_bp` pulling vertex entries, `pull_bind`, `hybrid_get`: a miss draws with a pulling pipeline while the real one compiles | A miss builds the full pipeline and waits | Add a pulling vertex path and a per-draw layout block; use it as the stand-in. It is nxnative's strongest anti-stutter mechanism and the same Metal API | 1–3 months |
| Prepare strict gate | The prepare step logs compiled / archive / reused; `pso_user.tsv` records misses for the next launch | Prepare exists, but no coverage report or strict pass/fail | Count records, missing/failed translations, first visits and skips; `NXRT_SHADER_STRICT` exits non-zero on a scripted route | 2–4 weeks + tours |
| Present / encode / refresh | `present_snap`, encode thread, `DM_SCREEN_MATCH` (144/165 Hz → 120) | Render thread queues; SDL chooses the display mode; no refresh matching | Test present-snapshot and encode-N+1 in the C8 experiments; match the display to a multiple of the game's rate (120 on ProMotion, 120 on a 144/165 Hz external) | 1–3 weeks, measured |
| Mac profile + packaging | `game.tsv` per game; `--platform mac-app`; per-Mac pipeline archive | A manager/frontend and a settings file; no shipped per-game profile; the archive is per Mac at run time | A per-game Mac profile (scale, frame cap, MetalFX, fullscreen, refresh) with tested defaults; package the app and its per-Mac archive | 2–3 weeks |
| Suspend / sleep | A teardown order: pause audio, drain GPU work, end in order | No sleep/wake handling | On macOS sleep, pause audio, drain the GPU, checkpoint or exit cleanly; restore on wake. The teardown order and state inventory are the reusable part | 2–4 weeks |
| QoS refinement | Three classes by guest priority, lock lending | Two classes; no lending | Map 45–59 to user-initiated and the rest to utility, and lend the waiter's class to a lock owner | ~1 week |

**Present/encode is an experiment, not a port.** nxrt's Mac work already queues frame N+1 while the GPU runs frame N
(RESTRUCTURE C8). nxnative's `present_snap` is the strongest version of that idea, but on a Mac with a real window server it has to be
measured against the existing render thread.

## 3. macOS Tier 3 — larger or per-title

- **HLE breadth.** nxnative has 36 `hle_*` units (account, friends, net, Mii, codec, movie). Port behaviours and tests
  for the services a new Mac title calls, not files.
- **Survey and function matching.** nxnative surveys an unknown NSP (modules, SDK, engine, graphics API) and writes a
  provisional profile; `nso_fnmap.py` / `MATCH_CUTS` carry patches across builds by function fingerprints. nxrt pins a
  build and refuses others. A survey mode and fingerprint matching would speed bring-up and let a port survive an update.
- **Per-game notes and the docs tests.** nxnative's `dm/games/<id>/README.md` and `test_docs.py` are the working model
  for the notes nxrt's ports need.

## 4. What not to take

- **The ARM rewriter's execution model.** It works because the host is ARM64. The loop classifier and spin-gate idea
  are portable; the binary rewriting is not.
- **The Ryujinx → SPIR-V → SPIRV-Cross → MSL chain.** nxrt has its own Maxwell → MSL translator. Take the facts table and
  the offline-build discipline, not the translator.
- **Per-game profiles wholesale.** They encode another GPU/driver/OS; re-derive on the target Mac.
- **Breadth as a goal.** nxnative can afford 74 titles because it runs unknown code; nxrt's advantage is depth on a few.

## 5. Order, with gates (macOS)

1. **Offline/setup shader compile + a working archive.** Gate: cold prepare time, warm prepare time, first-visit count.
2. **Metal 4 bases** (macOS 26+) **or** the Metal 3 archive + stand-in. Gate: unseen-pipeline build ms, archive size.
3. **Pipeline key normalisation.** Gate: pipeline count and archive size fall, pixels unchanged.
4. **Yield back-off + spin gate.** Gate: same frame times, lower CPU% and energy.
5. **Benchmark + power harness.** Gate: p50/p95/p99, GPU time, energy and wakeups for one route.
6. **Hybrid pulling stand-in.** Gate: no draws wait, pixel parity.
7. **Mac profile, packaging, refresh.** Gate: tested defaults on the target Macs.
8. **Suspend/wake.** Gate: repeated sleep/wake without a lost save or a stuck GPU.
9. **HLE breadth, survey, function matching** per new title.

## 6. Licence

nxnative is MIT (© 2026 seedds). Code copied or closely derived must keep its notice; add it to
[`THIRD_PARTY.md`](../THIRD_PARTY.md), as the NVN device-info import already does. The safest default is to reimplement
from behaviour and tests, because the Apple host model leaks into most files. No game profiles, shader tables or game
data should be copied.
