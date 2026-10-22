# A new game from scratch

How to bring up a Switch game that has no recomp project yet, on nxrt: from your own dump to the game running, then
making it good.

A note on words: this is a **recompilation**, not a decompilation. A decompilation rewrites the game as readable
source code by hand, function by function. A static recompilation translates the game's machine code automatically
into C that compiles and runs natively; the result isn't meant to be read. nxrt does the second. You never need the
game's source, and you don't rewrite its code. Your work is making the *Switch around it* behave as the game expects.

If the game already has a recomp that forked LAS Recomp, don't start from scratch: merge it instead
([PORTING.md](PORTING.md), [KICKOFF_PROMPT.md](KICKOFF_PROMPT.md)).

## What you need

- **Your own copy of the game,** dumped from your Switch: either the extracted `exefs/` and `romfs/` folders, or the
  `.nsp` / `.xci` and your console's `prod.keys`. Pick **one version** (usually the base game, without updates) and
  stick to it. Patches and addresses are tied to that exact build.
- **The toolchain** (macOS, Apple Silicon): Xcode's command-line tools (`/usr/bin/clang`), Homebrew `sdl3` and `ffmpeg`.
- **Python 3** with `capstone`, `lz4` and `unicorn`. The recompiler needs them.
- A disassembler you like (Ghidra, IDA, Binary Ninja) helps from step 6 on. You won't need it before.

Never commit game files, dumps, keys or generated code. Your `games/<id>/` folder is yours, not part of this
repository: keep it with your port (or your own fork of nxrt).

## 1. Lay out the folder

```bash
cp -R examples/game-template games/<id>
cd games/<id>
python3 -m venv tools/venv && tools/venv/bin/pip install capstone lz4 unicorn
```

`<id>` is a short name such as `mk8` or `smo`. It becomes `<id>game.dylib` and file names. Rename
`runtime/game.c` to `runtime/<id>_game.c` and set `GAME_ID` and `GAME_EXE` in the Makefile.

Put your game files at `games/<id>/extracted/`, as a copy or a symlink. Keep this folder out of git. When the app finds
`extracted/` in the folder it starts from, it runs in **developer mode**: the data folder is that folder itself, and it
uses the source tools.

## 2. Identify the game

You need four facts. All of them come from your own files.

| Fact | Where | How |
|---|---|---|
| **Title id** | `exefs/main.npdm` | The program id in the ACI0 section (or the 16 hex digits of the NSP's name / your dump tool's listing) |
| **Build id** | `exefs/main`, offset `0x40` | `xxd -s 0x40 -l 8 extracted/exefs/main` gives the 8 bytes for `build_id` |
| **Code modules** | `exefs/` | Every NSO: `main`, `rtld`, `sdk`, `subsdk0`, `subsdk1`... (`rtld` is not loaded: the runtime does its job) |
| **Module sizes** | each NSO | `tools/venv/bin/python ../../tools/nso.py extracted/exefs/*`: the `image 0x...` line |

## 3. Choose where the modules load

On the Switch, rtld picks the addresses; here you pick them once. Rules:

- `main` at `0x7000000000`.
- Each module needs room for its image. Round its size up generously. A bigger module overwriting the next is caught
  at boot ("overlaps the next one").
- Everything stays below `0x7080000000` (`HLE_BASE`).
- List them in **Horizon's rtld order**: main, then subsdk0, subsdk1..., then sdk. That's also the order symbols are
  looked up in.

Example (Echoes of Wisdom: main is `0x4004000` bytes, so it gets 128 MB, and the others 32 MB each):

```
main 0x7000000000 · subsdk0 0x7008000000 · subsdk1 0x700A000000 · sdk 0x700C000000
```

Write the same list in two places: `GAME_MODULES` in the Makefile (`name:0xbase` ...) and `nx_game.info.modules` in
`<id>_game.c`.

## 4. Write the descriptor

```c
#include "nxgame.h"

const NxGame nx_game = {
    .info = {
        .id = "<id>", .exe = "<id>recomp", .title = "<Name> Recomp",
        .game_name = "<The game's full name>", .game_version = "1.0.0",
        .title_id = 0x0100...ull,
        .build_id = {0x.., 0x.., 0x.., 0x.., 0x.., 0x.., 0x.., 0x..},
        .bundle_id = "org.example.<id>recomp", .data_dir = "<id>recomp", .ini = "<id>recomp.ini",
        .version = "alpha_1", .headline = "<Name> Recompiled",
        .romfs_probe = "<a folder or file every complete RomFS has>",
        .modules = {{"main", 0x7000000000ull}, /* ... */ {"sdk", 0x...ull}},
    },
};
```

For `romfs_probe`, pick a top-level folder or file of `romfs/` that is distinctive. The setup uses it to recognise the
data folder.

## 5. First build and first boot

```bash
SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang   # the app
SDKROOT=$(xcrun --show-sdk-path) make gen                         # recompile every module (minutes)
SDKROOT=$(xcrun --show-sdk-path) make -j10 CLANG=/usr/bin/clang   # now also builds build/<id>game.dylib
./build/<id>recomp --play extracted
```

`make gen` runs `tools/gen.py` on each module with your `tools/patches.py` (empty for now), writing C into
`generated/<module>/` (keep it out of git too). The second `make` compiles that C into the game code library. A player gets the same thing
from the start screen's Set up, on their machine.

`--play` skips the start screen and boots straight in. Everything is logged to the terminal. Use a scratch save folder
while you work: `NXRT_SAVE_DIR=/tmp/<id>-save`.

## 6. Get it to boot: the loop

The first boots stop early. Each stop names what is missing, so you fix it and boot again. These are the usual ones,
roughly in the order you meet them:

| What the log says | What it means | What to do |
|---|---|---|
| `overlaps the next one` | A module is bigger than its slot | Move the next module's base up (step 3) |
| `[hle] UNIMPLEMENTED <symbol> ... -> returning 0` | The game imports an SDK function the runtime replaces but has no body for | Often harmless. If behaviour breaks right after, implement it with `HLE_FUNC("<mangled name>", fn)` in a file under `runtime/hle/` |
| `[ipc] STUB <service> cmd N ... -> ok` / `[am] ... (unhandled, ok)` | The game called a system service command that answers "ok" with nothing | Fine until the game depends on the reply. Then implement the command in `runtime/services/` |
| `[sm] unknown cmd` / a service the game can't open | A whole service is missing | Add it with `SERVICE("name", handler)` (see `ipc.h`) |
| `unimplemented svc 0x..` | A kernel call the runtime lacks | Implement it in `runtime/kernel.c` |
| `no function at <addr> (lr=...)` | The game jumped somewhere the recompiler didn't see as a function start: usually into the middle of a function, or a missed jump-table target | Middle of a function: add the range to `inner_entries`. A missed table: a recompiler fix (`gen.py`), which helps every game |
| A hang | A thread waits forever | `NXRT_DUMP_THREADS=1` shows what each thread waits on; `NXRT_TRACE_SVC=1` traces kernel calls |
| A crash in recompiled code | Wrong behaviour of an instruction, or a bad pointer from a stubbed service | Find the function (`f_<address>` in the backtrace) in your disassembler; check the lifter for that instruction |

Rule of thumb: **fixes to the kernel, services, HLE, recompiler or renderer go into the library (this repository).** They are what the next
game needs too, and every game gets them. **Fixes that are true only for your game go into `games/<id>/`** as hooks or
patches. If your fix in shared code needs an `if (this game)`, it should be a hook instead.

## 7. Get it to draw

Once the game runs, frames come through the NVN renderer (Metal on macOS). Typical problems are missing shader
instructions in the translator, formats, and render-target handling: renderer fixes, so they go into nxrt.

Useful switches:

| Switch | Does |
|---|---|
| `NXRT_WINDOW_SHOT=<frame>` + `NXRT_SHOT_PATH=<png>` (+ `NXRT_EXIT_AFTER_SHOT=1`) | Saves that presented frame |
| `NXRT_DUMP_SHADERS=1` | Writes the translated shaders |
| `NXRT_LOG_FSHASH=1`, `NXRT_SKIP_FSHASH=<hash>,...` | Names each draw's fragment shader; leaves those draws out (find the broken one) |
| `NXRT_LOG_FILES=1` | Logs every RomFS file the game reads |
| `NXRT_HLE_SKIP=<text>` | Turns off HLE for functions whose name contains it (is the HLE body the problem?) |
| `NXRT_FNPROF=1` | Profiles recompiled functions (slow frames) |
| F9 in game | Dumps the next frame's passes |

## 8. Make it playable

Now it's the game's own work, all in `games/<id>/` through hooks ([nxgame.h](../runtime/nxgame.h); grouped in
the [README](../README.md#put-a-game-on-nxrt)):

- **Timing:** games that assume 30/60 FPS. Patch the timing sites (`tools/patches.py`) and set `max_frame_rate` if
  120 FPS works.
- **Input:** `pad_filter` / `pad_final` for camera or control improvements; `input_event` and `mouse_wanted` for mouse
  controls; `stick_deadzone`.
- **Movies:** `movie_skippable` for the boot movies.
- **Widescreen:** patch the camera's 16:9 constants, then `present_wide` / `present_crop`. Until then the image stays
  16:9 and boxed, which is the safe default.
- **Options:** the game's settings keys (`settings_*`) and its pages in the menu (`menu_pages`, built from
  `option_rows.h`).
- **Instruction patches:** `PATCHES[addr] = fn` in `tools/patches.py`. Always `expect()` the original instruction, so
  another build of the game fails loudly instead of miscompiling.

Every change to `patches.py` means rebuilding the game code (`make gen && make`; players get "Game code not built
yet"). See [ARCHITECTURE.md](ARCHITECTURE.md#when-every-game-must-rebuild-its-game-code).

## 9. Ship it

- Run the start screen the way a player meets it: no `extracted/` in the working directory, with
  `NXRT_DATA_ROOT=<scratch>` and setup from your folder or `.nsp`. Shots: `NXRT_LAUNCHER_SHOT=60` (or
  `60:settings`).
- Set `releases_api` / `release_page` to your project's GitHub releases for the update check. Use `alpha_<n>` tags
  until the game is playable, then `beta_<n>`, then `v1.0.0`.
- Add your game to the README's table, and a section to [PORTING.md](PORTING.md) with what went into nxrt, so the next
  game knows.

## Known rough edges

- `tools/sym.py` (symbolizes guest addresses) reads your game's modules and their load addresses from
  `runtime/*_game.c`; give it `--game games/<id>` and `--exefs <your extracted exefs folder>` (default
  `../extracted/exefs`). Games with no `.modules` table get the runtime defaults (`main`, `sdk`).
- The setup builds the game code with the compiler it finds: the bundled zig in a packaged release, else
  `/usr/bin/clang`, `/usr/local/bin/clang` or `/usr/bin/cc`. `NXRT_CC` overrides it. A failed build now leaves the game
  files "not ready" instead of reporting "ready".
- Only macOS has been verified since the merges; Windows and Linux use the Vulkan renderer.
