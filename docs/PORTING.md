# Porting notes

How each game came onto nxrt, what changed for players, and what has been verified. The short version is in the
[README](../README.md).

## How a fork is merged

LASRecomp, SkyRecomp and EoWRecomp share an ancestor: each fork started from a LAS commit. For every file the fork
touched, a 3-way merge runs with that LAS commit as the base, the fork's HEAD as "theirs" and nxrt as "ours"
(`git merge-file`). Conflicts that are only renames (LAS → the fork's name) resolve themselves; the rest are decided
one by one:

- **Into nxrt** when it helps every game: kernel, IPC and service fixes, recompiler and shader-translator fixes, new
  player features (mods, package setup...).
- **Into `games/<id>`** when it is the game's: its identity, module layout, patches, input logic, menu pages.
- **A new hook** when the fork changed shared code with an `if (this game)`: the shared code calls the hook, and the game
  provides it.
- **Left out** when it is a platform nxrt doesn't target yet (Android).

## Sky (SkyRecomp `b3e22ed`, forked from LAS `6244eb9`)

- **Into nxrt, for every game:**
  - Kernel: Horizon's condition-variable timeout semantics, thread-start errors, a thread dump.
  - Windows: precise timers and waits.
  - Movies: streaming from `romfs.bin`. Software keyboard: UTF-16 text.
  - Audio, shader translator, NVN object and Vulkan fixes; NVIDIA NIS upscaling (Vulkan).
  - Recompiler: jump-table fixes. Setup: resumable game-code builds. Diagnostics: a freeze watchdog.
  - Language: the system's own language picked up on macOS and Windows.
  - Controls: mouse buttons as bindable keys, relative mouse capture.
  - New features: a mod manager (LayeredFS: `mods/<name>/romfs`, a Mods tab, enable and order), texture packs,
    ReShade on Windows, a 60/120 FPS setting.
- **Into `games/sky`:** its identity and four-module layout, its two timing patch sites (`tools/patches.py`), its
  input logic (pad hooks), its texture code.
- **New hooks** (from Sky's `g_is_skyward` / `g_is_las` checks): `inner_entries`, `movie_skippable`, `language_code`,
  `max_frame_rate`, `stick_deadzone`, `keyboard_prompts`, `box_depth_draws`, `input_event`, `mouse_wanted`.
- **Per-game code modules:** `nx_game.info.modules` plus `GAME_MODULES` in the Makefile. The game-library format is now
  version 2, a list of modules.
- **Not ported yet:** accepting a prebuilt `romfs.bin`, its 4K texture release and ReShade packaging, its packaging
  scripts (dmg, AppImage, zip). Its `texpatch.c` still has LAS's dead branches (`g_is_las` compiled to 0).

## EoW (EoWRecomp `dd9c79d`, forked from LAS `ff09ff1`)

EoW built the same per-module design (ABI 2) on its own, so the two met cleanly.

- **Into nxrt:**
  - Game packages read in place (`runtime/nxpkg.c`): a folder with an `.nsp` / `.xci` / `.nca` and `prod.keys` is
    enough. The ExeFS is written out and the RomFS is served straight from the package (`romfs.src`, no 8 GB
    `romfs.bin`). Works with every "Keep the game" choice; "With my game files" puts it in a `<title> game` folder
    beside the package. Compressed `.nsz` / `.xcz` get their own message.
  - Kernel: `nn::init::Start`'s third callback (x4), newer GetInfo types. IPC: TIPC (SDK 12+ talks to `sm:` with it).
  - Recompiler: LSE/CAS atomics and exclusive pairs, `--shift`.
  - Shader translator: RED min/max and INC/DEC, FLO, structured loops. Command buffers with a host shadow.
  - Loader: a module bigger than its slot is a FATAL error.
  - Vulkan on macOS: MoltenVK portability flags, the bundled driver, background/foreground handling.
  - Update check: `alpha_<n>` tags rank below betas.
- **New hook:** `late_module_init` (sdk constructors first, the other modules after the SDK heap is up, main last).
- **Left out:** the Android platform, touch controls, the on-phone build, the Android guest address shift.
- **Into `games/eow`:** identity, title id `0x01008CF01BAAC000`, build id, the module layout in rtld's order (main,
  subsdk0, subsdk1, sdk), its empty `patches.py`. Its widescreen and texture files were stubs; the default hooks cover
  them.

## Behaviour changes from upstream LAS

- **Save backups (new).** Once per boot, when the game first opens its saves, the save folder is copied to
  `save_backups/<folder>/<timestamp>/`. The newest 5 are kept (`NXRT_SAVE_BACKUPS=<n>`, `0` = off). Test runs with
  `NXRT_SAVE_DIR` are skipped.
- **Menu:** LAS's six tabs are pages inside one "Link's Awakening" tab. "360-degree movement", "Remove blur" and "Hide
  Hero Mode icon" moved to its Gameplay page. New system tabs: Saves, Mods.
- **Settings file order:** keys are written in section order (runtime, game, input, UI). Keys and values are unchanged.
- **Vulkan bush fix:** the Vulkan renderer runs the same draw hooks as Metal, so LAS's cut-bush item fix also applies on
  Windows and Linux.
- **The game library is rebuilt once.** The module list changed its format, so the start screen says "Game code not
  built yet" and Set up rebuilds it (about 5 minutes, from the files already there).
- **From Sky:** a timed condition-variable wait no longer re-takes the mutex itself (the game's code does, as on the
  Switch); "Auto" language follows the system language; the Switch reports the game's real title ID.
- **Renamed:** "D-pad moves Link" → "D-pad moves the player"; the generator helper is `nxgen`; environment variables are
  `NXRT_*` (upstream: `LARECOMP_*`).
- **Removed:** three LAS-only debug hooks (`LOG_INDOOR`, `ASPECT_TEST`, the detailed `POT_PROBE` log).

## Verified (macOS arm64, 2026-10-22)

- **Builds:** `las`, `sky`, `eow`, `template` and `examples/ui-host`.
- **LAS on the merged runtime (after the EoW merge):** the game library rebuilt in place on a portable SSD (324 s), then
  the game booted from there. The opening movie plays and skips, and the 3D title screen renders at 60 fps, with no
  faults.
- **LAS against upstream (before the merges):** start screen and menu pages match pixel for pixel, except the intended
  label change. An upstream `larecomp.ini` with every key survives load → save (`NXRT_SETTINGS_ROUNDTRIP=1`).
- **Sky, EoW:** start screen, Game Files and Settings pages render.
- **`examples/ui-host`:** links none of nx-runtime; its start screen, settings tab and Controls page render, scripted pad
  input runs, save backups rotate.
- **Game folder on a drive:** "With my game files" builds next to the extract and Delete removes only what was built; a
  missing drive is reported.

**Not verified:** LAS gameplay past the title screen on the merged runtime; Sky and EoW in-game (no game files here);
setup from an `.nsp` (compiled, not run); Windows and Linux builds; packaging scripts.

**Fixed since that check:** the setup no longer reports "ready" after the game code failed to build, and outside developer
mode it falls back to the system compiler (`/usr/bin/clang`, `/usr/local/bin/clang`, or `/usr/bin/cc`) instead of
stopping with "the game code builder is missing". `NXRT_CC` still overrides it.
