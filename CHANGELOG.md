# Changelog

Notable changes to nxrt and the games in this repository. "Upstream" means the project a game came from:
[LASRecomp](https://github.com/peiikai/LASRecomp), [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp) or
[EoWRecomp](https://github.com/Nycolazs/EoWRecomp).

## v0.1.0 — first release (unreleased)

The first release of nxrt as one shared runtime: the common part of LAS Recomp, Sky Recomp and EoW Recomp, pulled out
once and put behind the `nxgame.h` interface. A game folder now holds only what is unique to its game.

### Added

- **One runtime for every game.** Recompiled game code is loaded as a native library
  (`<id>game.dylib` / `.so` / `.dll`) and run by a shared loader and dispatcher; the Switch around it is emulated:
  kernel, IPC and the Horizon OS services, plus NVN graphics translated to Metal on macOS and Vulkan elsewhere.
- **One recompiler for every game** (`nxrt/tools`): the game's modules and its patches
  (`games/<id>/tools/patches.py`) drive `gen.py`; no per-game copy of the tool.
- **Start screen setup from your own copy:** pick a folder with `exefs/` and `romfs/`, or read an `.nsp` / `.xci` in
  place with your `prod.keys` beside it. The game code is recompiled once, on your machine.
- **One menu in every game:** system tabs (Controls, Video, Audio, Saves, Mods, Quit) plus the game's own tab.
- **Saves:** per-profile slots, automatic backups once per boot (the newest 5 kept), "Back up now", and the list of
  backups.
- **Mods:** LayeredFS mods (`mods/<name>/romfs`) with enable and order, and texture packs.
- **Input:** keyboard, controllers and mouse, all rebindable, with matching button prompts and an on-screen keyboard.
- **Video:** unlocked aspect where the game supports it, resolution scale, MetalFX / FSR / NIS upscaling, SMAA, 60 or
  120 FPS where supported, ReShade on Windows.
- **`games/template`** — the minimal game to copy for a new port — and **`examples/ui-host`**, a host that uses only
  the five runtime-independent layers (platform, settings, input, save, ui).
- **`make venv`** in every game folder: creates the recompiler's Python environment (`tools/venv` with capstone, lz4,
  unicorn), which is not committed.

### Changed from upstream LAS

- The game code library is rebuilt once on first setup after upgrading: the start screen says "Game code not built
  yet" and Set up rebuilds it in place (about 5 minutes, from the files already there).
- LAS's six menu tabs are now pages inside one "Link's Awakening" tab; "360-degree movement", "Remove blur" and "Hide
  Hero Mode icon" moved to its Gameplay page. New system tabs: Saves, Mods.
- **The start screen has the same items in every game**: PLAY, SETTINGS, SETUP GAME FILES, EXIT. Setting the game files
  up and managing them is one page — Settings' Prelaunch tab — and SETUP GAME FILES goes straight to it, instead of a
  separate window.
- Settings keys are written in section order (runtime, game, input, UI). Keys and values are unchanged, so an upstream
  settings file survives load → save.
- Environment variables are `NXRT_*` (upstream: `LARECOMP_*`); the generator helper is `nxgen`.
- The Vulkan renderer runs the same draw hooks as Metal, so LAS's cut-bush item fix also applies on Windows and Linux.
- From Sky: a timed condition-variable wait no longer re-takes the mutex itself; "Auto" language follows the system
  language; the Switch reports the game's real title ID.
- Removed: three LAS-only debug hooks (`LOG_INDOOR`, `ASPECT_TEST`, the detailed `POT_PROBE` log).

### Fixed

- **The setup no longer reports success when the game code didn't build.** A failed build used to leave the game files
  looking ready when an older library was still beside the executable; the status now says what failed.
- **The setup finds a compiler outside developer mode.** With no bundled `zig`, it falls back to `/usr/bin/clang`,
  `/usr/local/bin/clang` or `/usr/bin/cc` instead of reporting a missing builder; `NXRT_CC` still overrides it. The
  message now says which piece is missing and what to do.
- **`nxrt/tools/sym.py` reads any game's modules** from its `runtime/*_game.c`, with `--game` and `--exefs`, instead of
  assuming LAS's two-module layout and `../extracted`.
- **Removed a dead font lookup** (`third_party/fonts/DejaVu`, never shipped); the substitute-font list keeps the
  system paths it actually found.

### Known limitations

- Windows and Linux have not been built since the merges; the Vulkan renderer is unverified there.
- In-game play has been verified for LAS only (to its title screen and the opening movie). Sky and EoW have had their
  start screens checked.
- Setup from an `.nsp` / `.xci` is compiled but has not been run.
- The macOS packaging scripts (`games/las/tools/bundle_macos.sh`, `make_dmg.sh`) have not been re-run since the merges.

See [`docs/PORTING.md`](docs/PORTING.md#verified-macos-arm64-2026-10-22) for the full verification record.
