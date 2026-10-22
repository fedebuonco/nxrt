# nxrt

**The shared runtime for Nintendo Switch static recompilations.** A recomp built on nxrt contains only what is
unique to its game. nxrt provides everything games have in common: running the recompiled code, the Switch system
around it, the graphics, and the app the player sees (start screen, menu, settings, saves, controls, mods).

| Port | From | Status on nxrt |
|---|---|---|
| The Legend of Zelda: Link's Awakening | [LASRecomp](https://github.com/peiikai/LASRecomp) | Playable upstream; boots on nxrt to the title screen |
| The Legend of Zelda: Skyward Sword HD | [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp) | Builds; menus verified |
| The Legend of Zelda: Echoes of Wisdom | [EoWRecomp](https://github.com/Nycolazs/EoWRecomp) | Builds; start screen verified (upstream itself is early) |

Those three ports are built on nxrt in private `games/<id>/` folders and are **not part of this repository**: each stays
with its own project and its own license. What ships here is the runtime they share, plus
[`examples/game-template`](examples/game-template), the minimal game to copy for a new port.

## How it works

The game's ARM64 code isn't emulated. It's **recompiled to C**, on the player's machine from their own copy of the game,
and compiled into a native library (`<id>game.dylib` / `.so` / `.dll`). What nxrt emulates is the *Switch around the
game*: the Horizon OS services it calls (high-level emulation), and the NVN graphics API, translated to Metal on macOS and
Vulkan elsewhere.

```
            ┌──────────────────────────────────────────────────────────┐
  games/    │ games/<id>: one `nx_game` descriptor + optional hooks,   │  what is unique to the game
            │ its menu pages, its patches for the recompiler           │
            └──────────────────────────────────────────────────────────┘
            ┌──────────────────────────────────────────────────────────┐
  nx-runtime│ recompiler (tools/gen.py) · loader · kernel · IPC · HLE  │  optional: a recomp with its
            │ services · NVN → Metal/Vulkan · game-files setup         │  own runtime skips it
            ├──────────────────────────────────────────────────────────┤
  nx-ui     │ start screen · in-game menu · on-screen keyboard         │
  nx-save   │ save folders · backups · per-profile slots               │  usable by any runtime
  nx-input  │ keyboard / controller / mouse → Switch pad · rebinding   │
  nx-settings│ one settings file, a section per part                   │
  nx-platform│ macOS / Windows / Linux · data paths · game folder      │
            └──────────────────────────────────────────────────────────┘
```

Dependencies only point down. The build enforces it: each layer compiles with only its own headers and the ones below
it on the include path, so an upward `#include` doesn't compile.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the rest:
- what is shared by every game and what each game brings;
- how the one recompiler (`gen.py`) handles every game;
- when game code must be rebuilt;
- how a game starts.

## For players: what every game gets

- **Setup from your own copy:** pick a folder with the extracted `exefs/` and `romfs/`, or the `.nsp` / `.xci` with your
  `prod.keys` beside it (read in place: no 8 GB copy). The game code is recompiled once, in a few minutes.
- **Keep the game where you want:** in the app's folder, with your game files (e.g. on a portable drive: nothing is
  copied, only what the app builds goes there), or in a folder you choose. A missing drive is reported, not mistaken
  for a missing game.
- **One menu, the same in every game:**
  ```
  [Controls] [Video] [Audio] [Saves] [Mods] [<the game's tab>] [Quit]
                                           └─ the game's own pages (ZL / ZR, or [ / ])
  ```
  System tabs hold system options only. Everything game-specific lives in the game's tab.
- **Controls:** keyboard, controllers and mouse, all rebindable, with matching button prompts.
- **Saves:** automatic backups once per boot (the newest 5 kept), "Back up now", the list of backups.
- **Mods:** LayeredFS mods (`mods/<name>/romfs`) with enable and order, texture packs.
- **Video:** unlocked aspect where the game supports it, resolution scale, MetalFX / FSR / NIS upscaling, SMAA, 60 or 120
  FPS where the game supports it, ReShade on Windows.
- **Update check** against the game project's GitHub releases.

## Build

macOS (Apple Silicon) with Homebrew's SDL3 and FFmpeg:

```bash
cd examples/game-template && make venv && SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang
```

`make venv` creates the game's `tools/venv` with the recompiler's Python packages (capstone, lz4, unicorn). It isn't
committed, and it's needed both by `make gen` and by the start screen's own build of the game code in a source checkout.

Run `build/<exe>` and follow the start screen. `examples/ui-host` builds the same way and needs no game at all. Windows
and Linux build with clang and the Vulkan renderer (not re-verified since the merges). Environment variables are
`NXRT_*`. For example, `NXRT_CC=/usr/bin/clang` picks the compiler the setup uses (otherwise it finds zig from a
packaged release, or the system clang).

## Put a game on nxrt

A game is a folder:

```
games/<id>/
  Makefile             GAME_ID, GAME_EXE, GAME_SRC, GAME_MODULES; include ../../nxrt/nxrt.mk
  runtime/<id>_game.c  const NxGame nx_game = { .info = {...}, .hooks = {...} };
  runtime/...          the game's own code: hooks, HLE functions (HLE_FUNC), menu pages (ui/*.mm)
  tools/patches.py     the game's instruction patches for the recompiler (may be empty)
```

1. **Copy [`examples/game-template`](examples/game-template)** to `games/<id>` (that folder is yours: it is not
   part of this repository).
2. **Fill in `nx_game.info`** ([nxgame.h](nxrt/runtime/nxgame.h)): names, `title_id`, the first 8 bytes of
   `exefs/main`'s build id (offset `0x40`), a `romfs_probe` path every complete copy has, and `modules`. The modules are
   the NSOs in `exefs/` with their load addresses, matching `GAME_MODULES` in the Makefile. The loader refuses modules
   that overlap.
3. **Build and run it.** With no hooks, the game runs as a plain recompilation: no overlay, no extra pages.
4. **Add hooks as the game needs them.** Every hook may be NULL. The main groups:

   | Need | Hooks |
   |---|---|
   | Boot order, odd code | `late_module_init`, `inner_entries`, `code_variant`, `modules_loaded` |
   | Per frame | `game_tick`, `present_tick`, `in_play`, `scene_name` |
   | Input | `pad_filter`, `pad_final`, `pad_mirrored`, `input_event`, `mouse_wanted`, `stick_deadzone`, `keyboard_prompts` |
   | Files | `romfs_override`, `content_dir`, `romfs_file_opened`, `save_read`, `save_written`, `movie_skippable` |
   | Settings | `settings_defaults`, `settings_key`, `settings_write` (the game's keys in the same file) |
   | Graphics | `tex_identify` / `tex_apply` (restyling), `draw_shader`, `draw_textures`, `draw_tint`, `present_wide`, `present_crop`, `max_frame_rate` |
   | Menu | `menu_pages` (the game's tab), `overlay_wanted` / `overlay_draw`, `menu_opened` |
   | Start screen | `profiles_list` / `profile_get` / `profile_set` (e.g. randomizer seeds) |

5. **Menu pages** are plain draw functions (`bool draw(float scale)`; return true to close the menu). Build them from the
   rows in [`option_rows.h`](nxrt/ui/option_rows.h), so they look like the system tabs.
6. **Patches** (`tools/patches.py`): `PATCHES[addr] = fn` replaces one instruction with C. Each patch checks the
   original word first, so a wrong build fails loudly. `VARIANTS` lets the game choose an instruction at run time.

**Porting a fork of LAS** (most Switch recomps started as one): merge it 3-way with the LAS commit it forked from as the
base. Fixes that help every game go into nxrt, the game's own parts go into `games/<id>`, and `if (this game)` checks in
shared code become hooks. [docs/PORTING.md](docs/PORTING.md) describes how Sky and EoW came in this way.

**A game with no recomp yet:** [docs/NEW_GAME.md](docs/NEW_GAME.md) goes from your own dump to a running game:
identifying it, laying out its modules, the boot-fix loop, graphics, then making it playable.

**Starting a port with an AI assistant:** [docs/KICKOFF_PROMPT.md](docs/KICKOFF_PROMPT.md) is a ready-made prompt.

## Use the layers from another runtime

A recomp with its own runtime can keep it and adopt only the five layers below nx-runtime.
[`examples/ui-host`](examples/ui-host) does this in about 150 lines, without linking any of nx-runtime:

1. `nx_app_init(&info)`, `plat_init()`, `rt_paths_init()`.
2. `nxin_init()`, `nxui_init()`, `nx_save_init()`, `nx_settings_register(&your_section)`, `settings_load()`.
3. An SDL window and a drawing backend for nx-ui. Use the Metal one (`nxui_metal_setup` + `nxui_set_backend`) or write
   an `NxUiBackend` for your renderer (8 functions).
4. Register what you have: `nxui_add_tab`, `nxui_set_game_tab` + `nxui_add_game_page`, `nxui_add_overlay`,
   `nxui_set_game_files`, `nxui_set_profiles`.
5. `launcher_run()` for the start screen. In your loop:
   - Events go to `menu_handle_event`, then `input_handle_event`.
   - When your game reads the pad: `nxin_read` → `nxin_dpad_walk` → `nxin_stick_buttons`.
   - `menu_render(frame, w, h)` after your image.
6. Saves: `nx_save_dir()`, `nx_save_backup()` once per boot, `nx_save_read_done` / `nx_save_write_done`.

## Repository

```
nxrt/
  platform/ settings/ input/ save/ ui/   the five layers (headers: nx_app.h, nx_settings.h, nx_input.h, nx_save.h, nx_ui.h)
  runtime/                               nx-runtime; nxgame.h is the game interface
  tools/                                 the recompiler (gen.py, lifter.py ...)
  third_party/                           imgui, SMAA, fonts, xxhash, ReShade
  nx.mk / nxrt.mk                        build rules: the layers / nx-runtime + a game
games/       your ports (las, sky, eow locally: not part of this repository)
examples/    game-template (the starting point), ui-host (the five layers on their own)
docs/        ARCHITECTURE.md (shared vs per game, the recompiler, rebuilds, boot), NEW_GAME.md (a game from scratch),
             PORTING.md (merge notes, behaviour changes, verification), KICKOFF_PROMPT.md
```

## Other Switch recomps

| Project | Path onto nxrt |
|---|---|
| letsgo-recomp, LASRecomp-Android-Plus | Forks of LAS: a `games/<id>` folder, merged as Sky and EoW were. Android support would come in as an nx-platform port. |
| ssbu-recomp, B2recomp, WindowSmash, HWDER, AC-Recomp, mk8-recomp | Own runtimes: adopt the five layers like `examples/ui-host`, with an `NxUiBackend` for their renderer (D3D11 for WindowSmash). |
| smm2recomp, sm35recomp | GPL-3.0: they can use nxrt (MIT), but their changes can only come back into nxrt with their authors' permission. |

## Status

See [docs/PORTING.md](docs/PORTING.md#verified-macos-arm64-2026-10-22) for what has been verified and what hasn't.
Windows and Linux builds and in-game play of Sky and EoW are not yet verified on nxrt.

## License

MIT ([`nxrt/LICENSE`](nxrt/LICENSE)): nxrt's own code, including runtime work merged from
[LASRecomp](https://github.com/peiikai/LASRecomp) (© Peiikai), [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp)
(© Apolinario-coder) and EoWRecomp (© Peiikai and the EoWRecomp contributors). Each game folder keeps its project's
license. Third-party code keeps its own; [`THIRD_PARTY.md`](THIRD_PARTY.md) lists every component and its license.
No game files, fonts or keys are included: players use their own copy.
