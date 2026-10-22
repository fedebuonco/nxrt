# Contributing

Thanks for helping. nxrt runs several Switch recompilations from one runtime, so the most useful contribution is usually
one that fixes something for every game at once.

## Where a change goes

| Change | Goes in |
|---|---|
| Something every game needs: the recompiler, loader, kernel, IPC, an emulated service, the NVN renderers, the start screen, the menu's system tabs, saves, input, settings | `nxrt/` |
| Something only one game needs: its hooks, its HLE functions, its menu pages, its patches, its randomizer or texture code | `games/<id>/` |
| A shared fix that one game needs to opt out of | `nxrt/`, behind a hook in `nxrt/runtime/nxgame.h` |

Dependencies point one way only: `platform` ← `settings` ← `input`, `save` ← `ui` ← `runtime` ← the game. The build
enforces it by putting only the layers below a file on its include path, so an upward `#include` does not compile.

**Never add an `if (this game)` to shared code.** Add a hook to `nxgame.h` (every hook may be NULL) and implement it in
the game folder. `docs/ARCHITECTURE.md` describes the layers and the interface; `docs/NEW_GAME.md` walks through
putting a new game on nxrt.

## Never commit

Not because they are secret, but because they are the player's, not ours:

- game files: `exefs/`, `romfs/`, `romfs.bin`, `romfs.src`, `.nsp`, `.xci`, `.nca`;
- keys: `prod.keys`, `title.keys`;
- generated C (`generated*/`), built libraries (`*game.dylib` or `.so`/`.dll` and their `.id`), build output (`build/`);
- saves, settings files, `tools/venv/`.

`.gitignore` covers these. If you find yourself needing `git add -f`, something is wrong.

## Building and testing

macOS (Apple Silicon), with Homebrew's SDL3 and FFmpeg:

```bash
cd examples/game-template
make venv
SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang
```

The same works in your own `games/<id>` folder and in `examples/ui-host`. The `games/` folder is not part of this
repository (each port stays with its own project). The game code library
(`<id>game.dylib`) needs your own game files and is built by the start screen, not by `make`; `make` builds the app.

Before sending a change:

- build every game, not just the one you are working on — a shared change can break the others;
- if you changed the recompiler (`nxrt/tools/`), say so: every game's code has to be rebuilt, which players will see as
  one slow setup;
- run the launcher with a scratch data folder (`NXRT_DATA_ROOT`, `NXRT_SAVE_DIR`) rather than your real one;
- report what you verified and what you didn't. "Builds on macOS, unverified on Windows" is a useful sentence.

## Adding a game

Follow `docs/NEW_GAME.md`, add the game to the README's table, and add a section to `docs/PORTING.md` saying what came
into `nxrt/` and what stayed in the game folder.

Most Switch recomps began as a fork of LAS. To bring one in, merge it 3-way against the LAS commit it forked from as the
base: see `docs/PORTING.md` for how Sky and EoW were done.

## Licenses

Everything here is MIT, with the upstream authors' notices kept: Peiikai (LASRecomp), Apolinario-coder (SkyRecomp) and
the EoWRecomp contributors. New files carry the same MIT terms; see [`nxrt/LICENSE`](nxrt/LICENSE) and
[`THIRD_PARTY.md`](THIRD_PARTY.md). By contributing you agree your changes are licensed the same way.

Do not add code, assets, fonts or shaders you cannot license, and do not add game files, keys or extracted assets.
