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
| [`runtime/`](runtime) | loader, dispatcher, kernel, IPC, HLE services, NVN → Vulkan, game-files setup. [`nxgame.h`](runtime/nxgame.h) is the game interface |
| [`tools/`](tools) | the recompiler: `gen.py`, `lifter.py` and the rest |
| [`platform/`](platform) | Linux / SteamOS, the data folder, paths, crash handling |
| [`settings/`](settings) | one settings file, a section per part |
| [`input/`](input) | keyboard / controller / mouse → the Switch pad, rebinding |
| [`save/`](save) | save folders, boot profiles, backups |
| [`ui/`](ui) | start screen, in-game menu, on-screen keyboard (Dear ImGui) |
| [`third_party/`](third_party) | imgui, SMAA, xxHash, ReShade headers, the Baloo 2 font |
| [`examples/`](examples) | `game-template` (a recomp in three files), `ui-host` (the five layers alone) |
| [`manage/`](manage) | `libnxrt_manage`: game folders and packages (setup, updates, RomFS) for a manager app such as [nxrt-frontend](https://github.com/fedebuonco/nxrt-frontend) |
| [`frontend/`](frontend) | `latest.json`, the update manifest nxrt-frontend checks |

## Build

This branch targets **Steam Deck / SteamOS, Linux x86-64, Vulkan** only. It needs clang, lld, make, SDL3, FFmpeg,
Vulkan, shaderc and Python 3.

```bash
cd examples/game-template
make venv
make -j10 CLANG=clang
```

`make` builds the app. The game code itself is generated from the player's own copy, at setup, on their machine.

The macOS/Metal and Windows backends have been removed from this branch. The Vulkan/Linux port is still in progress:
[`STEAM_DECK.md`](STEAM_DECK.md) lists what is missing and what the branch does not build yet.

## Tests

The library's host-side units — settings, saves, mods, the NSO/RomFS/package parsers, the texture layout and the
shared hashes — build and run without a game, a GPU or a window:

```bash
tests/run.sh
```

They compile the sources for real, with AddressSanitizer and UndefinedBehaviorSanitizer on by default, so a parser
that reads past a buffer fails the run. From a game project that includes `nxrt.mk`, `make test` runs them too; see
[`tests/README.md`](tests/README.md) for what each test covers and how to add one. The recompiler and renderer have
their own differential/fuzz tests under [`tools/`](tools), some of which need a game's own files or shader dumps.

## Formatting and static analysis

The tree carries [`.clang-format`](.clang-format) (LLVM's style, adjusted to the sources: 4 spaces, 120 columns) and
[`.clang-tidy`](.clang-tidy) (LLVM's `llvm-else-after-return` plus the high-signal bug-prone/readability checks and
the Clang static analyzer). Apple's clang ships neither tool; use Homebrew LLVM (`brew install llvm`).

```bash
tools/format.sh            # check the tracked sources
tools/format.sh --fix      # rewrite them
tools/tidy.sh              # clang-tidy over the runtime and layers (makes build/compile_commands.json)
tools/tidy.sh --all        # every translation unit, including the UI's .m/.mm
```

From a game project that includes `nxrt.mk`, the same is `make format`, `make format-fix` and `make tidy`. The
existing tree predates the formatter: the first `tools/format.sh --fix` is a large, formatting-only change and should
be its own commit. New code is expected to be formatted.

## Put a game on nxrt

Copy [`examples/game-template`](examples/game-template) to `games/<id>/`, fill in `nx_game.info`, add hooks as the game
needs them, and point `NXRT` at this repository. That is the whole integration; [docs/INTEGRATING.md](docs/INTEGRATING.md)
has the build variables, what ships to players, and when their game code is rebuilt.

## How it works

The game's ARM64 code isn't emulated: it's recompiled to C ahead of time, on the player's machine, and compiled into a
native library. What nxrt emulates is the Switch around it — the Horizon OS services the game calls, and NVN translated
to Vulkan on Linux/SteamOS. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the details.

## Status

Steam Deck / SteamOS (Vulkan) is the only target on this branch. It is a work in progress: the Vulkan UI backend and
the Linux build are not complete yet. No game files, keys or assets are included: players bring their own. Changes by
release: [CHANGELOG.md](CHANGELOG.md).

The apps also answer a manager: `--describe` and `--status` (who the game is, how its files and game code stand), and
`--setup-files`, `--generate`, `--compile`, `--setup` (the steps from a game package to a game that plays), as
[nxrt-frontend](https://github.com/fedebuonco/nxrt-frontend) uses them.

## Logging

The runtime and the generator share one line format (`[time] [LEVEL] [category] [run=<id>] message`), with levels,
categories and a `NXRT_LOG` filter (`warn`, `debug`, `input=trace,gen=debug`), an optional `NXRT_LOG_FILE`, and the
manager's `NXRT_RUN_ID`. Stdout stays the manager protocol. See [docs/LOGGING.md](docs/LOGGING.md).

```sh
NXRT_LOG=debug build/eowrecomp --play
```
