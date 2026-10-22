# nxrt

**A runtime library for Nintendo Switch static recompilations.** You bring the game: its recompiled code, its identity,
and whatever only that game needs. nxrt brings the rest — running the recompiled code, the Switch system around it, the
graphics, and the app the player sees.

It is consumed as source: vendor this repository as `nxrt/`, include `$(NXRT)/nxrt.mk` from your game's Makefile, and
implement one `nx_game` descriptor. The contract is [docs/INTEGRATING.md](docs/INTEGRATING.md); the starting point is
[`examples/game-template`](examples/game-template).

## In the library

| | |
|---|---|
| [`runtime/`](runtime) | loader, dispatcher, kernel, IPC, HLE services, NVN → Metal/Vulkan, game-files setup. [`nxgame.h`](runtime/nxgame.h) is the game interface |
| [`tools/`](tools) | the recompiler: `gen.py`, `lifter.py` and the rest |
| [`platform/`](platform) | macOS / Windows / Linux, the data folder, paths, crash handling |
| [`settings/`](settings) | one settings file, a section per part |
| [`input/`](input) | keyboard / controller / mouse → the Switch pad, rebinding |
| [`save/`](save) | save folders, boot profiles, backups |
| [`ui/`](ui) | start screen, in-game menu, on-screen keyboard (Dear ImGui) |
| [`third_party/`](third_party) | imgui, SMAA, xxHash, ReShade headers, the Baloo 2 font |
| [`examples/`](examples) | `game-template` (a recomp in three files), `ui-host` (the five layers alone) |

## Build

macOS on Apple Silicon, with Homebrew's SDL3 and FFmpeg:

```bash
cd examples/game-template && make venv && SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang
```

`make` builds the app. The game code itself is generated from the player's own copy, at setup, on their machine. Windows
and Linux build with clang and Vulkan, but haven't been re-verified since the ports were merged.

## Put a game on nxrt

Copy [`examples/game-template`](examples/game-template) to `games/<id>/`, fill in `nx_game.info`, add hooks as the game
needs them, and point `NXRT` at this repository. That is the whole integration; [docs/INTEGRATING.md](docs/INTEGRATING.md)
has the build variables, what ships to players, and when their game code is rebuilt.

## How it works

The game's ARM64 code isn't emulated: it's recompiled to C ahead of time, on the player's machine, and compiled into a
native library. What nxrt emulates is the Switch around it — the Horizon OS services the game calls, and NVN translated
to Metal on macOS and Vulkan elsewhere. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the details.

## Status

macOS arm64 is the verified target. Windows and Linux are in the tree (Vulkan) but unverified since the ports were
merged. No game files, keys or assets are included.
