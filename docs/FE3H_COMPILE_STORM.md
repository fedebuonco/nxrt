# Fire Emblem: Three Houses — the compile storm (2026-10-30)

Why the battle runs at 9–17 fps, why the picture softens, and why a cutscene sat black. One cause: **our interpreted
compute shaders are pathological for the driver to compile**, and the game meets ~16 of them per session.

## The measurement

`tools/pipebench.c` creates a real device on this Deck (RADV VANGOGH) and times pipeline creation on a saved SPIR-V
module, so the driver's cost is a number instead of a guess:

```sh
clang -O2 -o pipebench tools/pipebench.c -lvulkan -lshaderc_shared
./pipebench cs <module>.spv      # or: ./pipebench vs <module>.spv / ./pipebench fs <module>.spv
```

| module | stage | size | `vkCreate*Pipelines` |
|---|---|---|---|
| `24410ba908426db0.spv` | compute | 4 KB | **15.8 ms** (0.0 ms when created again with the same cache object) |
| `82c02081f7a20542.spv` | compute | **2.2 MB** | **>120 s — never finished** (a 900 s run also had to be killed); still >240 s with `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` |

The 2.2 MB modules are the **interpreted** compute shaders:

```
[shader] cs interpreted (BRX jump table) at 72b9d6a500 (c6e6990a8c88248d): 11975 words, 30 loops
```

The emitter writes one `while (true) switch (pc)` case per Maxwell instruction — ~12,000 basic blocks inside a single
loop. ACO is fast on ordinary shaders and catastrophic on those.

## What that explains

All four symptoms the player reported are this one bug:

| symptom | evidence |
|---|---|
| 17 → 9 fps "decay" that never recovers | four `nxrt-build` workers inside single builds for minutes: `[cpu] 350–500% of one core` while the fps line read `compile 0` and the new `jobs:` field showed ~0 s of *completed* jobs per interval. They never finish, so they are never cached, so every session pays again — it is not a decay, it is a storm that never ends. |
| GPU clock 200–1040 MHz, SoC pinned at 15 W | with the storm running the SoC sits at its 15 W cap and DVFS drops the GPU to its lowest steps; without it the title screen runs at 1040 MHz / 5.1 W. The GPU is starved by our own compiler threads. |
| black cutscene | no `[movie] open` anywhere in the session, so the scene was in-engine: at that moment `2093 draws, 144 skipped` (then 120–161) with `first visit: program/pipeline` lines in the same window. The draws had no pipeline yet. |
| models sharp, then soft | `drawn 1280x720` inside a `1920x1080` target: Dynamic (output_scale.c) lowered the render scale to 0.667 because the frames were slow. Already at the floor for a 1080p window, so it is a symptom, not a cause. |

Graphics pipelines are **not** the problem any more: the last session had **1** new graphics pipeline (`first visit`), the
rest were cache hits.

## What is in the tree

- `abc6573` — the fps line carries the Deck's own state (`sclk min-last MHz, ppt W, edge C, ac|battery`) and every
  `[gpuprof]` row carries vertices/instances/MSAA. This is how "is it throttling or is it us" became a reading.
- `850c773` — a draw whose pipeline is only *queued* builds it on the spot (state 3 claims it; the worker skips it)
  instead of the recorder waiting behind an unrelated queue, plus per-job-kind accounting (`jobs: pipeline 21.3s(9)`).
  Verified harmless: title screen 59.4 fps, no faults. It cannot help when the pipeline being waited for is one of the
  monsters above.
- `tools/pipebench.c` (+ this note) — the measuring instrument.

Measured baselines kept for comparison: title screen 5.9k verts a frame, 29 passes, 1040 MHz, 5.1 W; a battle frame
1.3–1.7M verts, ~500 draws, GPU work 22–37 ms of a 63–105 ms frame.

## Next, in value order

1. **Make the interpreter affordable for the driver** — the only thing that fixes this title's stalls. Either structure
   the compute shaders (the translator's structuring workstream) or emit the dispatch so the driver can handle it
   (nested switches / one block per basic block rather than one per instruction). Check with
   `./pipebench cs 82c02081f7a20542.spv`: a fix should take it under a second.
2. **Not enough on its own: skipping the driver's optimisation.** `NOOPT=1 ./pipebench cs 82c02081f7a20542.spv`
   (`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`) **still had not finished after 240 s** for the same module, so the
   cost is in converging 12k blocks at all, not in the optimisation pass. Fix (1) is the one that matters.
3. **Then the GPU:** the deferred lighting pass is 17.8–45 ms for a *single* 1280×720 draw (4 G-buffer fetches, light
   tables, 6 dependent shadow taps, a cubemap), ~5x the Switch's cost per pixel for the same pass. That is the
   remaining distance to 30 fps once the CPU stops burning.

Target for reference: this title is a **30 fps game on Switch** (60 in menus), so Switch parity is 30, not 60.
