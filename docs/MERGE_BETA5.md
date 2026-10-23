# Merging LAS beta_5 into nxrt

**Superseded:** this branch's work is merged to `master` (2026-10-23, `v0.2.0`), together with beta_5's renderer and
the shared gap work. The current state and what is left are in [BETA5_GAP.md](BETA5_GAP.md); the port plan is
[PLAN_BETA5_PARITY.md](PLAN_BETA5_PARITY.md). This page records what `beta5-merge` itself brought.

This branch (`beta5-merge`) brings LASRecomp `beta_5`'s runtime work into the library. It builds and runs the start
screen; **it is not finished, and none of it is released**. `master` and `v0.1.0` are untouched.

## What is merged

| Area | State |
|---|---|
| nx-platform (`platform/`, `paths`, `posix_log`, `crash_explain`, thread helpers) | merged |
| RomFS served from the game folder — **no `romfs.bin`** (`services/fs.c`, `romfs_build.*`, setup in `rt_ui.mm`, `hle/movie.c`) | merged |
| Game builder + build cache (`gamebuild.c`, `build_cache.*`, C++ in the runtime build) | merged |
| Library id names the game build: `GAMELIB_ID-<main build id>` (`gen.py`, `nxrt.mk`, `gamelib.*`) | merged |
| kernel / loader / dispatch / services | partial (beta_5's additions where they didn't need a redesign) |
| NVN renderers (Metal + Vulkan) | **not merged** — beta_5 removed the draw-hook interface; needs re-introduction |
| NVN shader translator (`maxwell/translate.c` + `shader.h`) | merged (our Vulkan alpha-test/dual-source additions kept) |
| UI features (startup shader prep, OneDrive, pad_nav wiring, overlay caching) | **not merged** |

## Test checklist (on a dump you own — use scratch folders)

1. **Build from the branch**
   ```bash
   git clone -b beta5-merge https://github.com/fedebuonco/nxrt.git nxrt-beta5
   cd nxrt-beta5/examples/game-template
   make venv && SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang
   ```
2. **Start screen** (no dump needed): `./build/templaterecomp` → the four items render, PLAY is greyed with
   "No game files".
3. **Setup with the RomFS change** — the important one:
   ```bash
   NXRT_DATA_ROOT=/Volumes/YourDrive/nxrt-scratch ./build/lasrecomp   # build games/las, not template
   ```
   - pick your dump, let it set up
   - **there must be no `romfs.bin`** in the game folder afterwards
   - the game folder holds `exefs/` and `romfs/` only, plus `.gamebuild/` and `<id>game.dylib`
   - "Game code not built yet" must clear to "Game files ready." once the build finishes
4. **Play**: the game must boot and read its data from the folder. Watch the log for `[fs]` errors about a missing
   RomFS file; if the game shows black or "data missing" screens, the layout code is wrong for this dump.
5. **Build cache**: stop the app during the game-code build, start it again, and watch the second run say it is reusing
   objects (`[gamebuild]` lines) instead of starting from zero.
6. **Id**: after setup, `<game folder>/*game.dylib.id` must end in the build id of your `exefs/main`
   (`xxd -s 0x40 -l 8 exefs/main`), not `unknown` and not another dump's id.

Report: what step failed, the tail of the terminal log, and whether `romfs.bin` was created.

## Ports that build against this branch

- `games/las` — builds.
- `games/eow` — builds.
- `games/sky` — builds: the library keeps `romfs_find_file()` / `romfs_pread()` as a game-side helper (the file
  itself, offset 0) since the RomFS redesign removed the image fd.

## Known gaps that will show up in a play test

- **NVN is not merged**: the renderer, shader translator and Vulkan backend are still nxrt's. Expect the graphics of
  the previous release, not beta_5's.
- **GPU movie upload is stubbed** (`renderer_upload_nv12` returns false): movies decode through swscale, so they play
  but cost more CPU.
- **Pacing/run-timer hooks were dropped** from `fs`/`movie`; they were LAS-specific and come back as hooks.
- The UI features listed above are absent: no startup shader preparation, no OneDrive detection.

## Why NVN is a separate pass

beta_5's `metal.m`/`renderer.m` wire LAS's texture restyling (`texpatch_*`, `recolour_*`, bottle/heart/item-cursor
classification) into the renderer. nxrt's rule is that the renderer only calls `tex_identify`/`tex_apply` and the game
implements them. Converting that is ~30 call sites across two files and cannot be verified without a running game, so it
stays out of this branch until it can be done and tested in one go.
