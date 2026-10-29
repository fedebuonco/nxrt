# Full shader precomputation — step-by-step

**Goal:** by the time the game needs a shader or a pipeline, it is already translated, compiled and built. No
first-visit compile, no draw skipped because a pipeline is pending.

This is not a rewrite. nxrt already has the pieces: a shader index over the game files (`runtime/nvn/shader_prep.c`),
a variant-record format (`ShaderRec`), per-game shipped lists, a player-seen list, Vulkan
pipeline caches, and backend preparation (`renderer_prepare`). What is missing is **coverage, gating and proof**.

"Full" means, precisely:

- every **shader program** used by the shipped route is translated and compiled;
- every **pipeline state combination** used by that route is created;
- a scripted full route produces **zero** `[shader] first visit` lines and **zero** draws skipped for
  `program building` / `pipeline building`;
- caches are versioned and invalidated on game/translator/GPU/record changes;
- the same holds on Vulkan.

It does **not** mean the Cartesian product of every possible NVN state, which is infinite in practice. It means every
variant the game actually uses, proven by a deterministic tour.

---

## Phase 0 — Measure and gate (small)

Without a coverage number, "full" is a guess.

1. Add a preparation report in `shader_prep.c`:
   - records in the shipped list and in the seen list (graphics/compute);
   - unique shader hashes indexed (VS/FS/CS);
   - records with a missing shader, failed translation, failed pipeline;
   - programs/compute programs/pipelines prepared;
   - first visits by kind;
   - draws skipped by reason (Vulkan).
2. Add `NXRT_SHADER_STRICT=1`: at exit, or after a fixed frame count, print the report and return non-zero if any
   first visit or avoidable skip happened. This is the CI gate.
3. Add per-phase timing: index, translation, compilation, pipeline build, archive load/save.

**Acceptance:** one EoW run reports exact coverage, and `NXRT_SHADER_STRICT=1` fails on a known gap.

---

## Phase 1 — Complete the shader index (small/medium)

The index is good, but it must prove it found everything.

1. Audit the candidate filter in `idx_romfs`: it currently scans `.bfsha`, `*.ptcl` and `.arc`. Include every
   container that can hold `BNSH` (at minimum: any file with a BNSH header; report per-extension hits so a missed
   container is visible).
2. Validate `idx_bnsh` for all six stages and check every code/constant bound. Confirm that the hash used by the
   index and by the runtime (`shader_code_hash` vs `shader_stage_hash`) is the same input.
3. Add an index manifest/report: per container, per stage, how many programs were found; how many indexed hashes are
   referenced by at least one record.
4. Bump `IDX_CACHE_VERSION` when the parser or candidate set changes.

**Acceptance:** every hash in the shipped/seen list is found; "not found" is zero for a known-good route; the report
lists programs with no record (unused shaders are fine, but they must be visible).

---

## Phase 2 — Produce a complete record list per game (weeks; game-specific)

This is the real work. A record carries shader hashes **plus pipeline state** (attribute formats, render-target
formats/views, blend, depth, samples, topology, flags, compute local size). The same shader code can have several
records because the translation key includes attribute/RT kinds. You cannot enumerate this from code alone.

1. Add `NX_SHADER_LIST` to the game Makefile (LAS pattern) and a
   `games/<id>/runtime/nvn/shader_variants.txt`.
2. Build a deterministic **shader tour** for the game:
   - LAS already has `NXRT_SHADER_TOUR` over its warp spots (`games/las/runtime/ui/shader_tour.mm`) and 1,585
     records.
   - EoW has no tour yet. Use its scene/level hooks or save states to visit every area, menu, cutscene, effect,
     photo/menu overlay, and the options/UI paths. Sky/Tunic likewise.
   - Run the tour with `NXRT_RECORD_KEYS=<file>`; every program/pipeline/compute variant the game builds is appended,
     deduplicated.
3. Merge records from all tours and from a real playthrough, then validate:
   - parse every line with `shader_rec_parse`;
   - every `vs`/`fs`/`cs` hash exists in the index;
   - sort and dedupe; count `G` and `C`;
   - reject records with out-of-range formats/state.
4. Add a small tool (`tools/shader_records.py` or per-game script) for merge/validate/sort, so this is repeatable.
5. Add the list to the Makefile and re-run the tour until the first-visit count is zero.

**Acceptance:** a scripted full tour with `NXRT_SHADER_STRICT=1` has zero first visits and zero skips from pending
builds; the record file is checked in (only hashes/state — no shader code).

**Risk:** full coverage may need manual playthrough for scenes with no teleport hook. Budget for it; that is the
per-game cost that replaces runtime generalisation.

---

## Phase 3 — Prepare every record before it is needed (medium)

Today preparation runs in the background after device init, and draws skip while builds are pending. Make the prepared
set authoritative.

1. Run preparation to completion at **setup/update** time (the start screen already has a progress bar and Skip), and
   on later boots load the caches first.
2. Gate `PLAY` until the critical set is ready, or prepare per scene before its load finishes. A progress bar is
   preferable to a stutter.
3. Make `renderer_prepare` cover every record:
   - Vulkan: program, compute, pipeline; save the pipeline cache.
4. Ensure the cache tag includes: app version, `SHADER_TRANSLATOR_ID`, `SHADER_CACHE_VERSION`, game build ID,
   record-set hash, GPU/driver. Bump when any input changes.
5. Measure cold (empty caches) and warm (archive/cache present) preparation times and set budgets.

**Acceptance:** cold preparation builds all records without failures; warm boot loads in the budget; a run after
preparation has no first-use compiles.

---

## Phase 4 — Stop skipping prepared draws (small/medium)

A prepared record that is merely *pending* should not silently disappear.

1. In the draw path, distinguish:
   - **known and prepared** → wait/queue/prefetch, never skip silently;
   - **unknown** → keep the skip, log it, and fail `NXRT_SHADER_STRICT` (a coverage bug);
   - **failed** → log once and keep the skip (a real unsupported variant).
2. Prefer per-scene prefetch: ensure a scene's records are ready before its first frame. This keeps the whole-set
   preparation optional for slow machines.
3. Keep the current asynchronous builds for unknown variants; they are the safety net, not the plan.

**Acceptance:** skipped draws due to "building" are zero in a full tour; unknown skips are logged and fail strict mode.

---

## Phase 5 — Validate both backends and the caches (medium)

1. Run the tour on the Deck; compare record coverage and skip counts across SteamOS/driver versions.
2. Test cache invalidation:
   - translator change (`SHADER_TRANSLATOR_ID`);
   - game update (build ID / shader hashes);
   - GPU/driver change;
   - record-set change.
3. Test cold/warm:
   - delete `cache/` and the Vulkan pipeline cache;
   - first run prepares; second run must have zero first visits and be faster.
4. Add a release check: run one port's scripted route with `NXRT_SHADER_STRICT=1` and archive the report.

**Acceptance:** deterministic on both backends; stale caches never load; warm run has zero first visits.

---

## Phase 6 — Keep it complete (ongoing)

- Re-run the tour on game updates and translator changes; records are pinned to the game build.
- Keep the strict gate in CI/release runs.
- Watch the report after every renderer/translator change: first visits are a regression.
- Do not ship shader code. Only hashes and state, as LAS already does.

---

## Where the tour lives

The route is game knowledge, so it stays with the game. The runtime only provides the mechanism.

| Part | Lives in | What it is |
|---|---|---|
| Record codec, recorder, index, preparation, strict gate | `runtime/nvn/shader_prep.c` | generic; no route, no game names |
| The tour itself | `games/<id>/tours/` (recorded clips) or `games/<id>/runtime/` (an optional warp hook) | input timeline + save slots; optional warp/teleport/checkpoint |
| Tour data | `games/<id>/runtime/ui/` or `games/<id>/tools/` | warp spots, save states, route list |
| The captured record list | `games/<id>/runtime/nvn/shader_variants.txt` | hashes + state, shipped with the game via `NX_SHADER_LIST` |
| Merge/validate/sort tool | `tools/shader_records.py` | generic data tool; no game content |

LAS is the precedent: `games/las/runtime/ui/shader_tour.mm` and `warp_data.h` hold the route, and
`games/las/runtime/hle/warp.c` calls the tour tick. `NXRT_SHADER_TOUR` is read by that game code, not by the runtime.
The runtime only sees `NXRT_RECORD_KEYS` and the records.

For EoW the tour belongs in `games/eow/runtime/` (e.g. `ui/shader_tour.mm`), called from EoW's own frame/tick hook.
If EoW has no warp function, the route uses save states or game-specific debug hooks — still in `games/eow/`.

**Do not add a per-game route or a game-name switch to `runtime/`.** If several games need the same *shape* of tour
framework, the framework can become generic, but the route and data stay in each game folder.

### Generic record/replay harness

The *mechanism* can be generic without putting game knowledge in `runtime/`: record raw input with a frame index,
replay it deterministically, capture with `NXRT_RECORD_KEYS`, merge with `tools/shader_records.py`, and prove with
`NXRT_SHADER_STRICT`. The route becomes data (`games/<id>/tours/*.jsonl`), not a `.mm` tour driver; optional warp hooks
stay in the game folder. Coverage is best-effort: there is no universal warp, and a missing pipeline must fall back
rather than skip a draw. The design, CLI, coverage tiers and fallback are in
[`docs/DECK_SPECIALIZATION.md`](DECK_SPECIALIZATION.md#11-a-best-effort-shader-tour-harness) and
[§12](DECK_SPECIALIZATION.md#12-no-universal-warp-coverage-tiers-and-graceful-degradation).

## How a native game does it: the SF6 pattern

Street Fighter 6's PC startup shader pre-compile is **not a tour**. From the outside it behaves like a bulk compile
of a known set:

- there is a "pre-compile shaders" option; it runs once, and is invalidated by a driver or graphics-settings change;
- on a Ryzen 5 3600 it took around 11 minutes, while a faster CPU could skip it and compile on the fly;
- on Steam Deck the game often does not show the same long step, because a precompiled cache is available.

That is the standard native-PC pattern: the engine can enumerate its shader/material permutations and has the bytecode
and state descriptions needed to create them, so it compiles the set for the current GPU in one go. The list may have
been produced by automated coverage during development, but what ships is the list, not a gameplay tour.

| Mechanism | What it is | Who does the work |
|---|---|---|
| Bulk compile from a manifest | Iterate known shader/PSO descriptions, compile them | Game at first run |
| Warmup tour | Render representative scenes to discover/compile what they use | Game or tool, once |
| Driver/OS pre-cache | Capture pipelines and distribute a compiled cache | Valve/Fossilize, driver, or OS |

### Why nxrt cannot simply copy it

- A native PC game owns its renderer and can ship a shader/PSO manifest. nxrt sees **guest code that creates NVN
  pipeline state at runtime**; there is no manifest in the game files.
- Shader **programs** can be enumerated from the files (static pretranslation, below).
- Pipeline **state** cannot, in general. It is produced by the game's draw calls; the only ways to learn it are a
  tour/capture or a runtime fallback.
- That is why the plan uses all three: static programs, a best-effort tour for states, and a fallback for the rest.

The Deck analogue of Valve's pre-cache would be a central capture + distribution system for our titles. It has the
same versioning and trust problems as any precompiled artifact; local build plus a local cache is the safe default.

Sources for the SF6 behaviour: [Digital Foundry's PC port analysis](https://www.digitalfoundry.net/articles/digitalfoundry-2023-street-fighter-6s-pc-port-is-competent-but-lacking-in-ambition)
and [GamingOnLinux's Steam Deck/Linux test](https://www.gamingonlinux.com/2023/04/street-fighter-6-on-steam-deck-linux/).

## Static shader pretranslation: what is possible

"Static" means: translate shader code from the game files without running the game and without a tour. That is
possible for a useful subset, but it is not the same as precompiling pipelines.

| Goal | Possible? | Why |
|---|---|---|
| Find every BNSH program in the files the index knows | **Yes, with audit** | `idx_bnsh` already scans `.bfsha`, `*.ptcl`, `*.arc` and the main NSO; the candidate filter must be widened and proven |
| Translate a shader program to GLSL/SPIR-V without a GPU | **Yes** | `maxwell_translate` emits GLSL; `vk_glsl_to_spirv` compiles with shaderc on the CPU |
| Deduplicate programs by code hash | **Yes** | `shader_code_hash` / `ShaderRec` already identify by code bytes |
| Use the cached SPIR-V when a draw needs the same translation key | **Yes** | `cache/spirv/<hash>.spv` and the `ShaderKey` translation key |
| Build the exact pipeline for every draw without running | **No** | Pipeline state (vertex formats, render-target formats, blend, samples, topology, dual-source) is only known from the draw |
| Cover runtime-generated shaders (Unreal/Oodle, Unity, GLSL capture) | **No, not statically** | The shader bytes are produced by the engine at run time; they need a boot/capture or an engine-specific extractor |
| Cover every container/engine | **No** | Compressed, encrypted or engine-specific containers need parsers; unknown is unknown |

### The key limitation: the same shader has several translations

`ShaderKey` ([`runtime/nvn/maxwell/shader.h`](../runtime/nvn/maxwell/shader.h)) includes:

- vertex input kinds (float / signed / unsigned) per attribute;
- render-target kinds per attachment;
- `vs_out_mask` (varyings the vertex stage provides);
- `depth_minus_one`, `flip_y`, `flip_x`;
- `alpha_func`, `dual_source`.

Some of these are properties of the draw, not of the shader's code. A static translator can pick a **canonical key**
(almost always float inputs and float render targets, no flip, no dual source) and translate that variant. It cannot
know that a particular draw uses an integer vertex attribute or an integer render target until the draw happens. That
is why nxnative carries `shader_facts_manual.tsv` for the cases the machine code cannot tell.

So the honest static target is:

> **Translate the canonical variant of every shader the index can find, cache it by code hash + key, and report the
> hit rate. Translate the remaining key variants lazily, then cache them.**

That removes most shader compilation from first use. It does not remove pipeline creation, and it does not guarantee a
hit for every draw.

### Practical design

1. **Index wider.** Scan every RomFS file and NSO segment for `BNSH`, not only the known extensions. Report per
   container/extension how many programs were found; a missed container must be visible, not silent.
2. **Parse and validate.** Check all six stages, code/constant bounds, and that the hash used by the index is the same
   input the runtime uses (`shader_code_hash` vs `shader_stage_hash`).
3. **Translate canonical variants.** For each unique stage code, build a canonical `ShaderKey`, translate with
   `maxwell_translate` (`glsl = 1`), compile with shaderc, and write the SPIR-V cache plus a manifest:
   `{code_hash, stage, key_hash, translator_id, status}`.
4. **Use it at runtime.** On a draw, build the real `ShaderKey`; if the SPIR-V cache has that key, use it. If not, the
   existing lazy path translates it and stores the variant. Count `static hit` vs `lazy`.
5. **Report coverage.** Per game: programs found/translated/failed, static hits, lazy variants, unknown containers.
   The metric is "share of shader compiles removed", not "pipelines precompiled".
6. **Do it on the player's machine.** The translation is derived from their own game files; do not ship SPIR-V.
7. **Combine with records for pipelines.** Static translation plus `NXRT_RECORD_KEYS`/`NX_SHADER_LIST` is the full
   precompute story: code from the index, state from the tour, pipelines built locally.

### What it buys

- No shader compile for the common variant of every indexable shader; first use is cheaper even without a tour.
- A useful Tier B/C baseline: the game starts warm on shader code, and only unknown pipeline states cost.
- A measurement that separates **shader translation** from **pipeline creation**, so the next optimization is aimed
  correctly.

### What it does not buy

- Zero first-visit stalls by itself. Pipeline creation remains, and unusual key variants still translate lazily.
- Coverage of runtime-generated shaders or unknown containers.
- A replacement for a tour when the goal is "zero first visits/skips" (Tier A).

## The first ten concrete steps

1. Add coverage counters + `NXRT_SHADER_STRICT` to `shader_prep.c` and both backends.
2. Run EoW with `NXRT_RECORD_KEYS` and record a baseline (first visits, skips, compile ms).
3. Build an EoW tour route (scene warp/save states) that visits every area and menu.
4. Generate the first EoW `shader_variants.txt`; validate every hash against the index.
5. Add `NX_SHADER_LIST` to `games/eow/Makefile`.
6. Prepare that list; fix missing/failed records.
7. Replace the background-only path with prepare-before-play (progress + warm caches).
8. Make the draw path wait/prefetch for prepared-but-pending pipelines.
9. Iterate the tour until strict mode reports zero first visits and zero building skips.
10. Repeat for Sky and Tunic; then run the same gate on Vulkan.

## What not to do

- **Do not** generate the Cartesian product of all states. Capture what the game uses; prove it with a tour.
- **Do not** make "full precomputation" mean "block the first launch for an hour." Warm caches plus a progress bar
  and Skip are part of the design.
- **Do not** put a game's tour/records in `runtime/`. The list and the tour live in `games/<id>/`; the runtime stays
  generic.
- **Do not** trust a record list without the strict gate. A stale list is worse than none: it hides the missing
  variants.
