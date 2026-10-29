# What LASRecomp's new main does, and what nxrt keeps

LASRecomp `main` (clone at `build/LASRecomp-main`, `b945481`) now uses nxrt as a submodule. Its own folder contains
only Link's Awakening:

```
Makefile                 LAS build/packaging
runtime/las_game.c       nx_game: identity + NxGameHooks (the whole game's interface to nxrt)
runtime/hle/*.c          LAS HLE: warp, randomizer features, pots, pets, widescreen, mirror, ...
runtime/nvn/texpatch.c   LAS texture identification/restyling
runtime/ui/*.mm          LAS menu pages, overlays, save editor, tracker
tools/, third_party/     LAS recompiler patches and randomizer
```

Everything common is in the `nxrt` submodule. This is the split nxrt's macOS line already made; the Steam Deck
Vulkan renderer is the part that still had direct calls into LAS names.

## Generalization already in nxrt

`runtime/nxgame.h` exposes `NxGameHooks`: identity/info, `game_tick`, `present_tick`, input filters, file/save
callbacks, texture identify/apply/replace, `draw_shader` / `draw_textures` / `draw_tint`, instanced tints,
`present_wide` / `present_crop` / `present_shape` / `present_frame_end`, scene helpers, menu pages and overlays.
LASRecomp's `las_game.c` is a concrete implementation of those hooks; nxrt must never include LAS headers or call
LAS symbols.

## What was still LAS-coupled on `steam_deck`

The Vulkan renderer predates that split. It called LAS names directly:

| Old direct call | Generic replacement |
|---|---|
| `texpatch_marker` / `texpatch_generation` / `texpatch_apply` | `NX_HOOK(tex_identify/tex_generation/tex_apply)` |
| `widescreen_allowed` | `NX_HOOK(present_wide)` |
| `widescreen_present_crop` | `NX_HOOK(present_crop)` |
| `widescreen_set_factor` | `NX_HOOK(present_shape)` |
| `widescreen_frame_end` | `NX_HOOK(present_frame_end)` |
| `warp_frame` / `save_editor_frame` | `NX_HOOK(present_tick)` |
| `game_level_name` | `NX_HOOK(scene_name)` |
| `pot_*`, `pets_*`, `escape_bottle_*`, `hint_sheet_*` | `NX_HOOK(draw_shader/draw_textures/draw_tint)` |
| `mirror_draw` | `NX_HOOK(draw_mirror)` |
| `widescreen_effect_stretch` | `NX_HOOK(draw_effect_stretch)` |
| `texpatch_is_item_cursor/bottle_kind/heart_kind` | `NX_HOOK(tex_identify)` + draw hooks |

The Steam Deck branch now uses the hook interface for texture identification/apply/replace, presentation (wide, crop,
shape, frame end, present tick), scene names, per-draw shader/texture/tint hooks, mirror, effect stretch and instanced
tints. There are no LAS symbols in the Vulkan runtime; a game supplies its own implementations through `NxGameHooks`.

## Rule for the Steam Deck branch

No game name, title id, path or LAS symbol in `runtime/`, `ui/`, `platform/` or `tools/`. A game lives in its own
folder and wires `NxGameHooks`; nxrt supplies defaults (no-op, plain Switch behaviour) for every hook.
