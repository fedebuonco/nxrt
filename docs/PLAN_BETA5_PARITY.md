# Plan: beta_5 feature parity without LAS in the library

Companion to [BETA5_GAP.md](BETA5_GAP.md). The gap left after the freeze is ~6.8k lines of **LAS game code** plus a
handful of **mechanisms** it needs. This plan separates the two: nxrt grows the mechanisms (hooks, services), and the
LAS port brings the content. Nothing LAS-specific goes into `runtime/`.

## The rule

- **nxrt owns mechanisms**: a tint buffer with a recolour mode, an overlay list, a run clock + LiveSplit client,
  NFP (amiibo), rumble/LED, a RomFS read filter.
- **The port owns policy**: which textures are recoloured and to what, which splits exist, which files an enemy
  randomizer rewrites, what the overlays show.
- Every mechanism lands with a game-neutral gate (boot LAS/EoW/Sky) and a feature gate (the LAS feature that needed
  it). If a mechanism only makes sense for LAS, it is not a mechanism.

## Status (2026-10-23)

- **M1 done** (Metal + translator): `draw_tint` is `ltint[16]`, the translator emits beta_5's recolour (Metal and
  GLSL), `NXRT_NO_RECOLOUR=1` is the A/B switch. Vulkan's tint path stays beta_5's LAS-coupled code for the Vulkan
  port task (it has never been compiled here — it does not even define `RECOLOUR_TINT_FLOATS`).
- **M2 done**: `overlays`/`noverlays`, the old pair still drawn after them.
- **M3 done**: `runtime/run_timer.c` + beta_5's LiveSplit client, verified against a loopback server (starttimer,
  splits, setgametime every ~260 ms, pausegametime); `NXRT_TIMER_TEST=1` self-test.
- **M4 done**: `nfp:user` (amiibo, no tag present) ported verbatim.
- **M5 done**: HID vibration 201/202/206 → SDL rumble; `input_led_set/poll/stop` for ports.
- **M6 done**: `romfs_read_filter(path, off, buf, n)` on the IStorage(romfs) read path.

LAS, EoW and Sky build and boot after every mechanism.

**P1 in progress** (the LAS port, `games/las/runtime`): beta_5's palette (`cosmetic_colour`) and recolour core
(`nvn/recolour.c`, adapted to read the game's files through the runtime RomFS rather than host paths) are in, with
the hair/shield option (`nvn/recolour_link.c`) wired through `texpatch_identify` (ids from
`TEXPATCH_RECOLOUR_BASE`) and `draw_tint`. Verified: `hair = Pink` in the ini turns Link's hair pink live, 60 fps,
the three textures recognised (`MI_hair_alb`, `MI_shieldA_alb/B`). Still P1: the other options
(seasons/enemies/tunic), the remaining colour rows (skin, leaves, grass, sand), Random/Custom in the tab, seed
cosmetics, and retiring the old upload-time tunic/heart path. The port went from 9,506 to 10,011 lines.

## Inventory (beta_5 files not in `games/las`, 27 files / ~6.8k lines)

| beta_5 files | lines | mechanism it needs | owner |
|---|---:|---|---|
| `recolour*.c/.h`, `cosmetic_colour*`, `cosmetics_seed` | ~780 | **M1** tint v2 (16 floats) + translator recolour; texture classes via existing `tex_identify` ids | port (policy), M1 nxrt |
| `hint_sheet.c` | 370 | `tex_replace` (exists) + the port's message/HLE code | port |
| `shell_spots.c`, `digs.c`, keysanity/shopsanity (partly present) | ~1,100 | existing HLE + `romfs_override`/`content_dir` | port |
| `enemy_random.c` | 464 | **M6** RomFS read filter | port (policy), M6 nxrt |
| `pickup_flyin.mm`, `race_overlay.mm`, `triforce_icons.mm` | ~610 | **M2** overlay list (+ existing menu) | port (content), M2 nxrt |
| `run_timer.c`, `livesplit.c` | ~400 | **M3** run clock + LiveSplit client | port (splits), M3 nxrt |
| `silent_pickups.c`, `quick_pickup.c`, `delegate_null.c`, `hooks.c` | ~550 | existing HLE + M2 for cards | port |
| `game_addresses*`, `game_release.cpp` | ~570 | `hle_override` (exists) + build-id checks | port |
| `services/nfp.c` (amiibo) | 94 | **M4** service in nxrt | nxrt |
| `led.h`, `rumble.h` (+ the input half) | ~100 | **M5** rumble/LED in nxrt input | nxrt |
| tracker/inventory/chaos/save-editor/warp/rando tabs, item names, difficulty, cheats, spoiler, shader tour, pets | ~4,000 | existing `menu_pages` + M2 for the HUD overlays | port |

## Phase 1 — mechanisms in nxrt

### M1. Tint v2 (recolour)
- `draw_tint` takes **16 floats** (`ltint[4]`: `[0]` multiply rgb + strength, `[1]` add rgb + luma exponent,
  `[2]`/`[3]` the second recolour window; strength < 0 = HSV recolour, as beta_5's shaders encode it).
- Port beta_5's `emit_recolour`/`emit_recolour_window` into the merged translator (Metal and GLSL), gated on
  `uses_tint` and `ltint[0].w < 0`; shaders without a tint hook still get zeros.
- Renderer (Metal `renderer.m`, Vulkan `exec.c`) passes 32→64 bytes; `NxDraw`'s `tex_ids` (from `tex_identify`)
  already give the port the per-draw texture set it needs for classes.
- Gate: LAS cosmetics change a tunic/grass colour on Metal; EoW/Sky pixel-identical without a tint hook;
  `tools/test_translate` builds the recolour branch.

### M2. Overlay list
- `NxGameHooks.overlays`: `{ bool (*wanted)(void); void (*draw)(float w, float h, void *font); }` drawn in order
  under the menu, above the game image; the existing `overlay_wanted`/`overlay_draw` remain as one-item fallback.
- Gate: LAS shows timer + pickup card together; menu still draws above both.

### M3. Run clock + LiveSplit
- `runtime/run_timer.c`: monotonic run clock (`nx_timer_start/stop/split/time_ms`), reset on reboot.
- `runtime/livesplit.c`: opt-in TCP client (settings `livesplit_host`, `livesplit_port`, `livesplit_enabled`),
  own thread, queue, reconnect; commands as beta_5 (`starttimer`, `setgametime`, `split`).
- The port decides when to start/split; nothing game-specific in nxrt.
- Gate: a loopback LiveSplit-server stub receives the LAS run; no socket when disabled.

### M4. NFP (amiibo)
- Port beta_5's `services/nfp.c` verbatim (module 115; "NFC available, no amiibo"), game-agnostic.
- Gate: LAS/EoW/Sky boot; a service request no longer falls to the logging stub.

### M5. Rumble and controller LEDs
- Port beta_5's rumble/LED half of `hle/input.c` (`rumble.h`, `led.h`) into `input/`: `input_rumble_set`,
  `input_led_set`, poll/stop via SDL; call `input_rumble_poll`/`input_led_poll` from the Metal present loop too
  (Vulkan already calls them, which today link to nothing).
- Gate: a controller rumbles and changes LED on an LAS event; no controller → silent.

### M6. RomFS read filter
- `bool (*romfs_read_filter)(const char *path, uint64_t off, void *buf, size_t n)` in `fs.c`: called for every
  guest read of RomFS bytes, true = patched in place. Complements `romfs_override` (whole-file replacement).
- Enables content patches that depend on the read pattern (enemy randomizer), without host files or copying.
- Gate: LAS room files with a seed's `enemies_random.tsv` change actors; a vanilla seed is untouched.

## Phase 2 — the LAS port, in dependency order

1. **P1 recolour/cosmetics** (M1): the class table + options + tint. Gate: a cosmetic change in game.
2. **P2 hint sheet** (tex_replace exists): message text + seed hints. Gate: X/Y on the sheet shows the next hint.
3. **P3 overlays** (M2): pickup fly-in, race overlay, triforce icons; then tracker/inventory/chaos. Gate: each draws
   where beta_5 draws it, alongside the menu.
4. **P4 pickups**: silent/quick pickups, delegate_null, hooks (needs P3's cards). Gate: a Heart Piece is silent and
   flies in.
5. **P5 data patches**: shell spots, enemy randomizer (M6), the digs/keysanity/shopsanity remainder. Gate: a seed's
   shells/enemies/shop checks behave.
6. **P6 run timer + LiveSplit** (M3), NFP usage (M4), rumble/LED (M5). Gate: timer/splits, amiibo "none" path,
   controller feedback.
7. **P7 version tables**: `game_addresses*`, `game_release.cpp` (the port's own build-id switch).

## Phase 3 — proof and docs

- Per-phase matrix: LAS feature gate **and** EoW/Sky boot + smoke (no regression).
- `docs/EXTENDING.md`: every hook with a signature and a small example — `tex_identify/apply/replace`, `draw_*`,
  `overlay` list, tint v2, `romfs_override` vs read filter, `hle_override` from `modules_loaded`, timer API,
  profiles, menu pages.
- Update `BETA5_GAP.md` numbers after each phase; `v0.2.0` only after the owner's three-port run.

## Order and effort

| phase | work | estimate |
|---|---|---:|
| M1 | tint v2 + translator recolour (Metal + Vulkan) | 1.5–2 d |
| M6 | read filter | 0.5 d |
| M2 | overlay list | 0.5 d |
| M4 | NFP service | 0.5 d |
| M5 | rumble/LED | 0.5–1 d |
| M3 | run clock + LiveSplit | 1–2 d |
| P1–P7 | LAS features on those mechanisms | 6–8 d |
| docs | EXTENDING + gap refresh | 0.5 d |

## Risks and decisions

- **Hook signature change** (`draw_tint` 8→16): only LAS implements it today; update the port in the same commit,
  keep a deprecated 8-float shim if any other port has one.
- **Translator recolour** is beta_5 code meeting our translator: it must not change shaders without a tint. Compare
  LAS frames before/after on the same draw hashes; keep `NXRT_NO_RECOLOUR=1` as an A/B escape hatch.
- **LiveSplit** opens a socket: off unless the setting is on; never on the render/guest thread.
- **Read filter** runs in the FS path: keep it cheap (memcmp on the path, then the patch), no allocation per read.
- **Vulkan** gets tint v2 in `exec.c` at the same time as Metal, but its recolour/chunking verification waits for a
  Vulkan machine.
- **NFP** must return the "no amiibo" path exactly as beta_5 does, or games that feature-probe crash (beta_5 issue
  #282).
