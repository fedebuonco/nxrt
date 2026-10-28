# C1/C4 scope (from nxnative's implementation)

## C1 — hybrid pulling stand-in

nxnative reference:

- `dm/gfx/nvn/nvn_mtl.m`: `hybrid_get`, `pso_desc_pull`, `pull_bind`, `pso_pend_demand`/`pso_pend_try`.
- `dm/gfx/tools/dmsh_pull.py` + `dmsh_pull.metal`: the `<key>_bp` vertex entry and the `dm_vlayout` block.
- `dm/gfx/tests/test_dmsh_pull.py`, `t_pull.m`, `t_pull_inv.m`: parity of the pulling and ordinary vertex paths.

What it needs in nxrt:

1. A setup-time MSL transform that, for every vertex shader with stage inputs, writes a `main0_bp` entry reading
   each attribute from a per-draw layout block (stream base/stride/divisor + attribute offset/format). This is the
   port of `dmsh_pull.py`, not a small runtime shim.
2. The layout block filled per draw and bound at a fixed buffer index; `pull_bind` in `renderer.m`.
3. A pulling pipeline variant in `renderer.m`'s pipeline cache (no vertex descriptor; same colour state), used only
   when its base is already made or in the archive.
4. Background building of the real pipeline, and a swap on a later draw; the draw must not wait.
5. Gates: EoW/FE3H screenshots pixel-identical; draw waits drop; no GPU faults.

This is nxnative's strongest anti-stutter feature and its own estimate is 1–3 months. It should be a dedicated
branch with the shader transform first, then the pipeline selection, then parity.

## C4 — HLE breadth

There is no single "port HLE" step: nxnative has 36 units and each is driven by a title. The useful next loop is:

1. Take one title whose survey (`tools/survey.py`) shows imports the runtime does not implement.
2. Run it with `NXRT_LOG=hle=trace` and collect the unhandled commands.
3. Implement one service unit with a unit test and a title-level check.
4. Record the title in `docs/GAME_NOTES_TEMPLATE.md` and repeat.

Dread/EoW/FE3H currently boot and run; they do not exercise a missing service in the tested route, so C4 needs the
next title/route rather than speculative services.
