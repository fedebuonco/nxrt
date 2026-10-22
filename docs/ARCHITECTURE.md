# How nxrt works

What nxrt is, what is shared by every game and what each game brings, and how the recompiler handles many games
without changing. For building a recomp on nxrt — what to vendor, what to implement, what ships — see
[INTEGRATING.md](INTEGRATING.md); for the short version, the [README](../README.md#put-a-game-on-nxrt). The ports
themselves (`games/<id>/`) are not part of this repository.

## The role of nxrt

nxrt is the engine under every Switch recomp in this repository. A game project brings only what is unique to its game.
nxrt brings what is the same for all of them: running the recompiled code, the Switch system around it, the graphics,
and the app the player sees.

**Why it exists.** LASRecomp, SkyRecomp and EoWRecomp each started as a copy of the whole LAS project with the game
swapped out. So the same runtime existed three times. A fix in one never reached the others, and each copy drifted
apart. nxrt is that common part pulled out once, with the game-specific parts moved behind a small interface
([`runtime/nxgame.h`](../runtime/nxgame.h)).

**What "native" means here.** The game's ARM64 code isn't emulated instruction by instruction. It's translated to C
ahead of time, on the player's machine from their own copy of the game, and compiled into a native library
(`<id>game.dylib` / `.so` / `.dll`). What nxrt emulates is the *Switch around the game*:
- the Horizon OS services the game calls (kernel, file system, audio, input, IPC: high-level emulation);
- the NVN graphics API and its shaders, translated to Metal on macOS and Vulkan elsewhere.

## The layers

| Layer | Folder | Role | Interface |
|---|---|---|---|
| nx-platform | [`platform/`](../platform) | macOS / Windows / Linux differences, the data folder, the game folder (e.g. on a portable drive), crash handling | `nx_app.h`, `paths.h`, `platform.h` |
| nx-settings | [`settings/`](../settings) | One settings file; the runtime, the game, input and UI each register a section | `nx_settings.h` |
| nx-input | [`input/`](../input) | Keyboard / controller / mouse → the Switch pad, rebinding, dead zones, button-prompt style | `nx_input.h` |
| nx-save | [`save/`](../save) | Save folders, per-profile slots, backups, read/write listeners | `nx_save.h` |
| nx-ui | [`ui/`](../ui) | Start screen, game-files setup UI, in-game menu (system tabs + the game's tab), on-screen keyboard | `nx_ui.h`, `menu.h` |
| nx-runtime | [`runtime/`](../runtime), [`tools/`](../tools) | Recompiler, loader, dispatcher, kernel, IPC, HLE services, NVN → Metal/Vulkan, game-files setup, the game interface | `nxgame.h` |

Dependencies only point down: nx-platform ← nx-settings ← nx-input, nx-save ← nx-ui ← nx-runtime ← the game. The build
enforces it: each layer compiles with only its own headers and the ones below it on the include path.

**nx-runtime is optional.** A recomp with its own runtime can keep it and use only the five layers below. That gives it
the same start screen, menu, controls, settings and saves ([`examples/ui-host`](../examples/ui-host)).

## Shared by every game

All of this lives at the root of this repository and is the same code for every game:

- **The recompiler:** `tools/gen.py`, `lifter.py`, `analyze.py`, `nso.py`, `passes.py`, `simd_c.py`, and the headers the
  generated C compiles against (`runtime/cpu.h`, `simd.h`, `simd_fp.h`, `gamelib.h`).
- **The runtime:**
  - loader, dispatcher, kernel, IPC;
  - every emulated service (fs, audio, hid, am, pl…);
  - movies and the software keyboard;
  - the NVN renderer for Metal and Vulkan, with its shader translator.
- **The app:**
  - the start screen and game-files setup: extracted folders or `.nsp` / `.xci` read in place, where to keep the game,
    building the game code;
  - the menu's system tabs: Controls, Video, Audio, Saves, Mods, Quit;
  - controls and rebinding, the settings file, save backups, mods (LayeredFS) and texture packs, the update check.

**One fix reaches every game.** When the Sky and EoW forks were merged, their runtime fixes went into nxrt, so LAS got
them as well. Examples: the condition-variable timeout, TIPC, more atomic instructions, shader-translator fixes. Their
new features (mods, texture packs, 120 FPS, `.nsp` setup) became available to every game.

## Brought by each game

All of this lives in `games/<id>/`:

| Part | What it says | Example |
|---|---|---|
| `nx_game.info` | Who the game is: names, title id, the first 8 bytes of `main`'s build id (the setup refuses other versions), settings file name, data folder, release feed, its code modules and their load addresses | EoW: main `0x7000000000`, subsdk0, subsdk1, sdk `0x700C000000` |
| `nx_game.hooks` | Where the game departs from a plain Switch. Every hook may be NULL. | Sky: the mouse camera in `pad_filter` / `pad_final`, `max_frame_rate = 120` |
| Menu pages (`menu_pages`) | The game's own tab in the menu; system tabs can't be extended by games | LAS: Gameplay, Cosmetics, Randomizer, Cheats, Warp, Save Editor |
| Its own code | HLE overrides (`HLE_FUNC`), texture restyling, overlays, boot profiles | LAS: the randomizer, its seeds as boot profiles, the tracker overlay |
| `tools/patches.py` | Hand-written replacements for specific instructions | Sky: 2 timing sites; EoW: none yet |
| `Makefile` | `GAME_ID`, `GAME_EXE`, `GAME_SRC`, `GAME_MODULES` (the same modules as `info.modules`), then `include ../../nxrt.mk` | |

A game with no hooks runs as a plain recompilation: no overlay, no extra pages, files served as they are. EoW is
currently exactly that: a descriptor and a Makefile.

## The recompiler: one `gen.py` for every game

There is one recompiler for every game, `tools/gen.py`. Each game's output is different because each game's
binary is different, not because the tool is. The game-specific parts are inputs:

```
gen.py <module> <nso> <base> <outdir> [--game games/<id>/tools] [--hle hle_symbols.txt]
```

- **Module and address.** The setup runs it once for each module in `nx_game.info.modules` whose file is in `exefs/`,
  with that module's base address. All modules are generated at the same time, and their output is compiled while
  generation goes on.
- **`--hle`** (the `sdk` module only): the list of SDK functions the runtime implements itself. Calls to them go to the
  host instead of the recompiled code.
- **`--game`:** loads the game's `patches.py`:
  - `PATCHES[addr] = fn`: one instruction replaced with C. Each patch checks the original instruction word first, so a
    different build of the game fails generation loudly instead of miscompiling.
  - `VARIANTS[offset] = [words]`: sites in `main` where the game chooses an instruction at run time, through the
    `code_variant` hook. LAS's randomizer uses this for its seed patches.
- **`--shift`:** moves the guest address range lower (for a future Android port). Off by default; it changes nothing on
  desktop.

The generated C for all modules, plus `tools/gamelib_entry.c` (one table per module), is compiled into the game code
library. The runtime loads that library and looks recompiled functions up in it by guest address.

### Changes made to it while merging the games

These apply to every game:

- **Before:** LAS's randomizer variants were read from a hard-coded file. **Now:** each game's `patches.py` is loaded
  instead (`load_game`), with `VARIANTS` limited to `main`.
- **From Sky:** jump-table fixes. An `adr` feeding a nearby `br` is a table's case base, not a new function, so functions
  are no longer split there.
- **From EoW:**
  - `lifter.py` lifts exclusive pairs, CAS / CASP and the LSE atomics.
  - `--shift` and label naming that keeps the unshifted address.
- **Game library format version 2:** a list of modules instead of the fixed main + sdk pair. Sky and EoW each designed
  this on their own and arrived at the same format.

### When every game must rebuild its game code

The game code library carries an id: a hash of everything it is built from. That is the recompiler's files, the
runtime's `hle_symbols.txt`, the headers it is compiled against, the game's `patches.py`, the generator flags and
`GAME_MODULES` (`GAMELIB_ID` in [`nxrt.mk`](../nxrt.mk)). The app knows the id it expects. A library with any
other id is never loaded: the start screen says "Game code not built yet", and Set up rebuilds it. That takes a few
minutes and reuses the game files already there.

| You change | Who has to rebuild their game code |
|---|---|
| `gen.py`, `lifter.py`, the other recompiler tools, `cpu.h` / `simd*.h` / `gamelib.h`, `hle_symbols.txt` | Every game |
| A game's `patches.py` or `GAME_MODULES` | That game only |
| Anything else in the runtime, the layers or the game's runtime code | Nobody: it is part of the app, not the game code |

After the EoW merge, for example, LAS's library on the portable SSD had to be rebuilt once: 324 s.

## How a game is started

1. **The layers come up** (`layers_init` in `runtime/main.c`): app identity from `nx_game.info`, settings sections,
   input, UI, saves, the game's hooks (dead zone, prompts, save listeners), the UI backend.
2. **The start screen** (nx-ui `launcher_run`, with the runtime as host in `runtime/rt_ui.mm`):
   - it shows whether the game files and game code are ready, and runs setup if not;
   - from it, the player sets options or picks a boot profile.
3. **Boot:**
   - the loader maps each module at its address and checks that none overlaps the next;
   - the game code library is loaded and its id checked;
   - the dispatcher builds one function table per module;
   - the kernel starts the main thread at `nn::init::Start` with the SDK's init, main and fini callbacks.
4. **Module constructors** run in the order the game needs. By default every module but main runs first, then main.
   With `late_module_init`, sdk runs first, then the others, with main last.
5. **Each frame:**
   - the game reads the pad through nx-input: the game's `pad_filter` and `pad_final` hooks have the last word;
   - its draws go through the renderer and the game's draw hooks;
   - the presenter shows the frame, and the menu and overlays draw on top.

## Where things live at run time

| What | Where |
|---|---|
| The app's data: settings file, saves, save backups, mods, shader cache | macOS `~/Library/Application Support/<bundle_id>`, Linux `$XDG_DATA_HOME/<data_dir>`, Windows next to the exe |
| The game folder | `extracted/` in the data folder, the player's own folder ("With my game files"), or `<chosen folder>/<title> game` |
| Inside the game folder | `exefs/`, `romfs/` or `romfs.src` (the RomFS read from the package), `romfs.bin` (the data image), `<id>game.dylib` + `.id` |

The "Delete game files" button only removes what the app built: the player's own files are never deleted.
