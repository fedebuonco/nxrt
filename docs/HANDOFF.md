> **Steam Deck branch.** This document predates the macOS/Metal and Windows removal. Paths and commands
> mentioning them are historical; the target is Linux/Vulkan on Steam Deck.

# nxrt — project handoff

**v0.4.0, 2026-10-23.** Release: <https://github.com/fedebuonco/nxrt/releases/tag/v0.4.0> (`master` = the release tag).
v0.4.0 is the hardening/testing/tooling release: the host test suite, the safe shared LZ4 decoder, bounds checks in the
NSO/package/RomFS parsers, the LayeredFS wiring fix, and the clang-format/clang-tidy setup. Game-facing behavior is
otherwise unchanged from v0.3.0.
The NVN-specific history is in [HANDOFF_NVN.md](HANDOFF_NVN.md); the gap against LASRecomp `beta_5` is in
[BETA5_GAP.md](BETA5_GAP.md); the port roadmap is [PLAN_BETA5_PARITY.md](PLAN_BETA5_PARITY.md).

**Current goal: fix Echoes of Wisdom's graphical glitches.** Everything else is either done or parked on purpose
(see "Deliberately not done" below). Section 9 is the starting point for that work.

## 1. What nxrt is

A shared runtime for Switch **static recompilations**. A game's own NSOs (main, subsdk0/1, sdk) are translated to
host C by `tools/gen.py` and compiled into `<id>game.dylib`. nxrt provides everything a console would: kernel,
services, NVN → Metal, input, audio, saves, settings, the start screen, the in-game menu.

Two halves, glued by a tiny app:

```
games/<id>/runtime/<id>_game.c     the app: nx_game.info (who the game is) + hooks
build/<id>game.dylib               the game: recompiled NSOs (gen.py), cached per game build
nxrt                               runtime/ ui/ input/ platform/ settings/ save/ tools/ examples/
```

Game code is native host code that carries a `Cpu` struct; it is not interpreted. Imports the console would resolve
(service calls, `nn::*` APIs, NVN) are bound at load time to host stubs the runtime implements.

## 2. Repository map

| path | what |
|---|---|
| `runtime/` | kernel, loader, dispatch/HLE, services, NVN renderer, game builder, settings |
| `runtime/kernel.c` | guest threads, mutex/condvar/arbiter/process-wide keys, events, handles, memory, TLS |
| `runtime/hle/` | HLE of `nn::*` APIs that are not worth running recompiled (fonts, movie, libc, input glue) |
| `runtime/services/` | IPC services: fs, am/applet, hid, vi, audio, pctl, nfp, swkbd, time, … |
| `runtime/nvn/` | NVN objects, command buffers, Maxwell decode (`maxwell/translate.c`), Metal renderer (`metal.m`, `renderer.m`) |
| `runtime/nvn/vulkan/` | the Vulkan backend — **beta_5's code, still game-coupled, see §7** |
| `ui/` | nx-ui: start screen (`launcher.mm`), in-game menu (`menu.mm`), option rows, pad navigation, name dialog |
| `input/`, `settings/`, `save/`, `platform/` | SDL input + bindings, settings layer, save folders/backups, per-OS glue |
| `tools/gen.py` | NSO → C recompiler; produces the game dylib and its id |
| `examples/game-template` | the starting point for a new game (builds as-is) |
| `examples/ui-host` | proof the UI/settings/input/save layers stand alone, without nx-runtime |
| `docs/` | this handoff, BUILD-windows, linux, NVN history, gap/plan documents |

`games/` is gitignored: the reference ports (LAS, EoW, Sky) live only on this machine (or a copy), with the player's
game files.

## 3. How a game runs

1. **Start screen** — pick the game files: a folder (`exefs/` + `romfs/`) or a package (`exefs/` + `romfs.src`, read
   in place, nothing extracted). Setup copies or checks what it needs, then builds the game code (`gamebuild.c`,
   `build_cache`), cached and rebuilt only when the files/version change.
2. **Shader preparation** — after setup (and in the background on later starts) the shipped variant list, plus the
   shaders the player has already met, are translated and compiled so first visits don't hitch. A game with no list
   (every game but LAS) just prepares its seen list. `NXRT_SHADER_PREP=0` disables it.
3. **Load** — `gamelib.c` maps the modules at their fixed addresses and rebinds imports to host stubs.
4. **Start** — `nn::init::Start` runs on a guest thread created by `kernel.c`. From there:
   - `svc*` → `kernel.c` (threads are host threads; guest mutex/condvar/arbiter semantics);
   - `nn::*` → `runtime/hle/` or an IPC service;
   - NVN → `runtime/nvn/`: command buffers recorded, Maxwell decoded, shaders translated to MSL and compiled,
     textures uploaded, `renderer.m` replays on Metal and presents.
5. **Hooks** — the runtime calls back into the app for textures, tint, overlays, menu pages, presentation, saves.

```
game.dylib (recompiled main/subsdk/sdk)
   │ svc*   → kernel.c            threads, sync, memory, handles
   │ nn::*  → hle/ + services/    files, applet, hid, audio, fonts, nfp, ...
   │ NVN    → nvn/ → Metal        cmdbuf → Maxwell → MSL → GPU, present
   └ hooks → games/<id>/runtime   identity, textures, overlays, UI, patches
```

## 4. The three reference ports (local, not committed)

| | game files | hooks it sets | state |
|---|---|---|---|
| **LAS** (2D) | folder | about thirty: `tex_identify/apply/generation`, `draw_tint` + instance tints, overlays + menu pages, pad filters/scripts, `present_crop/shape/wide`, save hooks, `romfs_override`, `romfs_files`, … | 60 fps, clean |
| **EoW** (3D) | package | **only `late_module_init`** | 60 fps past its shader compiles; **graphical glitches, the current goal** |
| **Sky** (3D) | package | language, max frame rate, menu pages, overlays, pad filters, `present_tick`, settings, `tex_*`, strap-guide override in `modules_loaded` | 60 fps; one pipeline fails on blend targets (~240–360 draws/frame skipped) |

EoW is the proof the library is generic: essentially no game glue, everything through the shared runtime.

## 5. What a new port supplies

- `nx_game.info`: `id`, `exe`, `title`, `game_name`, `game_version`, `title_id`, `build_id`, `bundle_id`,
  `data_dir`, `ini`, `version`, `headline`, optional `menu_name`/`romfs_probe`/`romfs_files`/release links.
- An app Makefile: `GAME_ID`, `GAME_EXE`, `GAME_SRC`, optional `NX_SHADER_LIST` (a shader variant list).
- Only the hooks it needs (`runtime/nxgame.h`): textures, tint, instances, overlays, menu pages, profiles, RomFS
  override/read filter, saves, settings, `modules_loaded`, `language_code`, `max_frame_rate`, presentation.
- The game code from `tools/gen.py`, from the player's own files.

Both examples build against the current tree (`make -C examples/game-template`), which is the reference for a new port.

## 6. What is in, from LASRecomp `beta_5`

- the NVN renderer: async render queue, command memory + host shadow, MSL translator (BREV/FLO.SH/HSETP2 fixes,
  structured loops), shader preparation and seen-shader list, frame pacing, texture hashing;
- game-agnostic mechanisms: tint v2 (16 floats) + HSV recolour, overlay list, run clock + LiveSplit client, NFP
  (amiibo), rumble/LED, RomFS read filter;
- start screen: setup (folder/package), cached game-code builds, shader preparation with bar/Skip, overlay caching,
  mechanisms' Controls rows, OneDrive warning;
- kernel diagnostics: thread dump with guest backtraces, kernel trace ring (`NXRT_KTRACE=1`), hang watch that catches
  a main thread stuck in one kernel wait;
- genericity cleanup: no game names/paths/ids in the library; identity via `nx_game`/`nx_app`; layers stand alone
  (examples/ui-host builds).

## 7. Deliberately not done / still open

- **Vulkan** — `runtime/nvn/vulkan/exec.c` and `textures.c` still call 38 LAS-only symbols (`pot_*`, `texpatch_*`,
  `recolour_*`, `escape_bottle_*`, `pets_*`) and have never been built here. Decoupling + tint v2 + the chunked heap
  is the big shared item, and needs a machine where Vulkan builds.
- **Kernel scheduling**: beta_5's wait-fairness/cancel/atomic-arbiter changes stay out — they deadlocked EoW.
- Sky's blend-target pipeline failure; the `opt_colour` UI row (cosmetics-only); the `.lasrecomp_*` move markers and
  the `region_common` RomFS-probe fallback (compatibility warts).

## 8. Building and running (this machine)

macOS builds use Homebrew clang, SDL3 and FFmpeg (`SDKROOT=$(xcrun --show-sdk-path)`; see `nxrt.mk`).

```zsh
cd games/<id> && make -j10 B=build-wip CLANG=/usr/bin/clang
NXRT_SETTINGS=<data root>/<id>recomp.ini NXRT_DATA_ROOT=<data root> ./build-wip/<id>recomp --play
```

Data roots used here:

| game | settings | game folder | notes |
|---|---|---|---|
| LAS | `/tmp/d/larecomp.ini` | `/Volumes/PortableSSD/LA-extract` (folder) | `romfs/` on disk |
| EoW | `/tmp/eowdata/eowrecomp.ini` | `…/EoW Recomp game` (package) | `romfs.src` |
| Sky | `/tmp/skydata/skyrecomp.ini` | `…/The Legend of Zelda Skyward Sword HD (NSP) (Base Game)/Sky Recomp game` | `romfs.src`, `resolution = 75%` |

Useful run variables: `NXRT_WINDOW=1280x720`, `NXRT_WINDOW_SHOT=<frame>` + `NXRT_SHOT_PATH=<png>` (a screenshot),
`NXRT_NO_LAUNCHER=1` / `--play` (skip the start screen), `NXRT_SETTINGS=…` / `NXRT_DATA_ROOT=…`.

## 9. The current goal: EoW's graphical glitches

### What is known

- One **compute shader does not translate**:
  `[compute] translate failed: unknown instruction eb20138600f70018 at 0x358 (code 0x72a4723000)`. It is retried and
  can also show up as `unknown instruction 0000000000000000 at 0x8` (the code buffer read back as zeros — a second,
  possibly lifetime-related symptom to check).
- Early in a session, first-visit shader compiles stall (a single shader can take seconds; ~20 s cumulative in one
  run). Draws/pipelines whose shader is still compiling are **skipped**, so effects/objects can appear late; the
  stats line shows `N draws, M skipped`.
- The historical EoW translator failures (`bfe brev`, `50d9e…`, `ebf9…`, FLO.SH) were fixed by the beta5-merge
  translator and no longer appear.
- The dungeon renders correctly in stills; the glitches the owner sees are to be catalogued (below) — they may or may
  not be caused by the missing compute shader.

### Plan

1. **Catalogue the glitches** — for each: screenshot (`NXRT_WINDOW_SHOT`), scene, what is wrong (missing effect,
   wrong colour, z-fighting, missing object, flicker). Cross-check the stats line: skipped count, `compile N/Mms`.
2. **Find the responsible draw/pipeline** — `NXRT_SKIP_FSHASH=<hash,…>` drops draws by fragment-shader hash (bisect
   which draw makes the artifact); `NXRT_SAVE_MSL=1` writes the translated MSL to `build/msl/`;
   `NXRT_DUMP_SHADERS=1` writes the raw shader code/control to `build/shaders/`.
3. **If a shader fails to translate** — the failing word is printed. The dispatcher is
   `runtime/nvn/maxwell/translate.c` (`else if (OPM(mask, value))` chain, ending in `fail(c, "unknown instruction …")`).
   The EoW word `0xeb20138600f70018` has the `0xeb2` opcode family; `0xebf` (RED.E.OR) was implemented earlier and is
   the closest reference. Add the opcode, rebuild, and confirm the failure disappears.
4. **If Metal validation fails** — e.g. "Fragment shader does not write to render target color(0), index(1) that is
   required for blending" (Sky's case, same renderer): the pipeline's blend state vs. the shader's declared outputs;
   look at `renderer.m`'s pipeline creation and the translator's render-target declarations.
5. **If it is the missing compute** — its effect will be absent rather than wrong; identify what the program does from
   its code (`0x72a4723000`) and the dispatch log (`[dispatch] new compute program …`).

### Likely first tasks

- Decode `eb20138600f70018` and implement it in the Maxwell dispatch (compare with the `0xebf` reduction and the
  `0x50a/0x509` predicate ops already there).
- Check why the retry reads zeros at `0x8` (code buffer lifetime/address on the second attempt).
- Measure EoW's first-visit compile cost before/after any translator change (`NXRT_SHADER_SALT=<text>` forces cold
  compiles; `compile N/Mms` in the stats line).

## 10. Pitfalls and environment notes

- Run **one instance per data root**: two processes sharing `cache/`, saves or the shader cache can corrupt each other.
- EoW and Sky occasionally froze at ~6 s / ~14 s with `NXRT_KTRACE=1`; the trace only shifts timing races. It is
  opt-in now. For a freeze, `NXRT_HANG_DUMP=15` (prints threads + the kernel trace if enabled) is the tool.
- The external SSD occasionally unmounts; `exefs/main is missing or unreadable` means retry.
- Ports live in `games/` (gitignored). `build-wip/` is a throwaway build dir; a clean build is `B=build-master`.
- The release tag is `v0.4.0`; earlier freeze tags: `v0.3.0`, `nvn-three-games-2026-10-23`, `v0.1.0`.
