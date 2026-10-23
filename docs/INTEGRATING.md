# Build a recomp on nxrt

> **Steam Deck branch.** Only Linux/Vulkan is present here. macOS/Metal and Windows references in older
docs are historical; see [STEAM_DECK.md](../STEAM_DECK.md).

nxrt is a **runtime library**: you bring a game, it brings everything every recomp needs. This page is the contract
between the two — what you vendor, what you implement, how it builds, and when things must be rebuilt.

For the layer-by-layer design, read [ARCHITECTURE.md](ARCHITECTURE.md) after this.

## Two ways in

| You are building | Include | You get | Example |
|---|---|---|---|
| A recomp | `nxrt.mk` | The whole runtime: recompiler, loader, kernel, IPC, HLE services, NVN renderers, and the app | [`examples/game-template`](../examples/game-template) |
| A runtime of your own that wants the app around it | `nx.mk` | The five layers only: platform, settings, input, save, ui | — |

Both are Makefile includes, consumed as source. There is no binary to link against and no install step: a clone of this
repository (or a copy of `nxrt/` inside your project) is the dependency.

## Level 1: a recomp

### 1. Vendor nxrt

```
your-project/
  nxrt/            this repository (git submodule, or a copy: nxrt.mk at its root)
  games/<id>/      yours
```

The library is self-contained apart from SDL3 and FFmpeg (see [Building](#building)). Keep the layout: the build rules
derive paths from `NXRT`.

### 2. Your game folder

```
games/<id>/
  Makefile             ~6 lines (below)
  runtime/<id>_game.c  const NxGame nx_game = { .info = { ... }, .hooks = { ... } };
  runtime/...          only what this game needs: hooks, HLE functions, menu pages
  tools/patches.py     per-instruction fixes for the recompiler (may be empty)
```

Start from [`examples/game-template`](../examples/game-template) — copy it to `games/<id>/` and rename
`runtime/game.c`. Nothing else is required: with no hooks, the game is a plain recompilation with the system start
screen and menu.

```make
NXRT     := ../nxrt        # games/<id>/ -> your-project/nxrt (git submodule), or the path you copied it to
GAME_ID  := <id>              # -> <id>game.so
GAME_EXE := <id>recomp
GAME_SRC := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/ui/*.mm)
GAME_MODULES := main:0x7000000000 sdk:0x7004000000   # the NSOs in exefs/ and their load addresses
include $(NXRT)/nxrt.mk
```

Other variables a game may set: `GAME_INC` (extra includes for its sources), `GAME_TOOLS` (its tools folder, default
`tools`), `WINDOWS_ICON`.

### 3. The one interface you implement

`nx_game` ([nxgame.h](../runtime/nxgame.h)) has two halves:

- **`.info`** — identity: names, `title_id`, the 8-byte build id of `exefs/main`, the module list and bases, a
  `romfs_probe` path, the release feed for the update check, optional title movie and profile label. The setup uses
  this to recognise the player's copy and to refuse the wrong version.
- **`.hooks`** — everything optional, every entry may be NULL: boot order, per-frame ticks, input filters, file and
  save interception, settings keys, draw/ texture hooks, the game's menu pages, and boot profiles.

Anything shared belongs in the library behind a hook, never as an `if (this game)` in the runtime.

### 4. Build

```bash
cd games/<id>
make venv                                 # the recompiler's Python env (capstone, lz4, unicorn)
SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang
```

`make` builds the **app**. It does not build the game code: that is generated from the player's own game files, at
setup, on the player's machine, and compiled into `<id>game.so` next to the app. In a source checkout you can do it
yourself with `make gen` once `extracted/exefs/` holds your own dumped modules.

## What ships to the player, and what must not

The app you distribute contains:

- your app binary (nxrt + your game folder compiled in),
- the recompiler: `gen.py` and its Python packages (`tools/venv`), or a packaged `nxgen` helper,

It does **not** contain, and must never contain: game files (`exefs/`, `romfs/`, `.nsp`, `.xci`), keys
(`prod.keys`), generated C, or the built `<id>game.so`. The player supplies their own copy; the setup copies or
reads it in place and generates the code from it.

## When the game code is rebuilt

The game code library carries `GAMELIB_ID`: a hash of the recompiler tools, `hle_symbols.txt`, the headers the game
code is compiled against, the game's `patches.py`, the generator flags and `GAME_MODULES`. A library with another id
is never loaded — the start screen says "Game code not built yet" and Set up rebuilds it.

| You change | Who rebuilds |
|---|---|
| `tools/*`, `cpu.h`, `simd*.h`, `gamelib.h`, `hle_symbols.txt` | Every player, once |
| your `patches.py` or `GAME_MODULES` | Your players, once |
| anything else (runtime, layers, your hooks and menu pages) | Nobody: it is the app |

`GAMELIB_ABI` ([gamelib.h](../runtime/gamelib.h)) is the format of what the app loads from the generated library;
it changes only when that format does.

## What nxrt does not do for you

- **The game's own code.** You run `tools/gen.py` over your modules and, usually, add `patches.py` entries where the
  translator's guess is wrong. That is the port's own work.
- **HLE for services nobody implemented yet.** `runtime/hle/` and `services/` cover what the ports so far needed.
  A new game that calls something missing gets a trap or a stub, and the fix is a new `HLE_FUNC` in the library — shared
  with every game, not a fork.
- **Game files, keys, fonts or artwork.** Never, by design.

## Platform support

This branch targets **Steam Deck / SteamOS: Linux x86-64, Vulkan**. macOS/Metal and Windows have been removed. The
Vulkan/Linux target is still in progress; see [STEAM_DECK.md](../STEAM_DECK.md).

## Building

- Linux / SteamOS: clang, lld, make, Python 3, SDL3, FFmpeg, Vulkan, shaderc.
- On a Deck, use a writable toolchain (distrobox or a SteamOS development environment); the read-only root does not
  carry the build dependencies.

Environment variables are `NXRT_*`; `NXRT_CC` overrides the compiler the setup uses for the game code.
