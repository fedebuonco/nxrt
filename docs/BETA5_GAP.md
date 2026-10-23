# How far is nxrt from LASRecomp `beta_5`?

Measured 2026-10-23 against **`peiikai/LASRecomp` `beta_5` = `f7b986e`** (2026-10-22), a read-only clone in
`/tmp/las_beta5`, compared with `nvn-renderer-wip` @ `b32de7e` (tag `nvn-three-games-2026-10-23`).

The short answer: **the shared runtime is beta_5's, almost line for line. What is not there is LAS's own game
code** (16.3k lines, of which the local `games/las` port carries 9.5k), plus two translator features we have
deliberately not taken yet. Vulkan is beta_5's code verbatim but has never been run here.

## Method

- Every `.c/.h/.m/.mm/.cpp` under `runtime/` (beta_5) was matched by file name against `runtime/`, `platform/`,
  `settings/`, `input/`, `save/`, `ui/` here (the layers nxrt split out), then compared by line count.
- Backend files were diffed to separate real differences from the `LARECOMP_*` → `NXRT_*` renames.
- LAS-specific files were matched against `games/las/runtime/` (the port is the owner's, not committed).

## The numbers

| beta_5 (f7b986e) | lines | files |
|---|---:|---:|
| whole `runtime/` | 59,496 | 169 |
| — shared runtime (has a same-name counterpart here) | 42,661 | 101 |
| — LAS game code (`hle/warp.c`, `ui/rando_tab.mm`, …) | 16,336 | 60 |
| — version/address tables, seeds, rumble, led headers | 499 | 8 |

| nxrt (`nvn-renderer-wip`) | lines | files |
|---|---:|---:|
| `runtime/` + `platform/ settings/ input/ save/ ui/` | 44,584 | 124 |
| — in files beta_5 also has | 41,025 | 101 |
| — nxrt-only (nx-ui, nxpkg, mods, settings split, docs) | 3,559 | 23 |
| `games/las/runtime/` (the port) | 10,011 | 41 |

So **96% of the shared lines are in place, and every shared file exists**; the 3.6k nxrt-only lines are the
refactors that made the runtime game-agnostic (nx-ui/launcher split, package RomFS, mods, settings layer). Area by
area (beta_5's figures include its LAS-only files, ours include the equivalent nxrt split):

| area | beta_5 | nxrt | state |
|---|---:|---:|---|
| kernel/loader/ipc/dispatch/gamebuild/cache (root) | ~6.5k shared | 8.6k | ours is nxrt's line + the beta_5 hang dump; beta_5's other ~464 lines are diagnostics/scheduling that deadlocked EoW (`e481c2a`) and stay out |
| HLE (shared part) | ~1.4k | 1.7k | `movie`, `pl`, `heaps`, `vi`, `swkbd`, `libc`; beta_5's `input.c`/`bindings.c` are nxrt's `input/` |
| services | 3.1k | 3.2k | equal, plus nxrt fixes (package RomFS, applet accessor, crash texts) |
| NVN Metal (shared) | ~13.1k | 13.3k | beta_5's renderer + nxrt's fixes (tint hooks, crop, clear kind, command shadow, sampler map) |
| NVN Vulkan | 10,663 | 10,663 | **beta_5's, byte-for-byte apart from `LARECOMP_*`→`NXRT_*` renames** — never run here |
| UI (generic) | ~2.3k | 2.9k | menu, option_rows, pad_nav, osk, launcher (nx-ui split) |
| platform | 1.9k | 3.0k | ours split + session log/crash/thread helpers |

## What is still missing, and where

### In the LAS port, not in nxrt (feature parity for LAS)

From beta_5's 16.3k game lines, the port carries 9.5k. 27 files (~6.8k lines) are not in `games/las/runtime`:

- cosmetics/recolour: `recolour.c`, `recolour_seasons.c`, `recolour_enemies.c`, `recolour_link.c`, `recolour.h`,
  `cosmetic_colour.c/.h`, `cosmetics_seed.c` (~750) — and the translator support they need (below);
- randomizer/overlays: `hint_sheet.c` (370), `shell_spots.c` (526), `enemy_random.c` (464), `pickup_flyin.mm` (231),
  `race_overlay.mm` (199), `triforce_icons.mm` (183), `triforce.c` (169), `keysanity`/`shopsanity` present in part;
- QoL/UI: `livesplit.c` (249), `run_timer.c` (149), `silent_pickups.c` (141), `quick_pickup.c` (112),
  `delegate_null.c` (135), `hooks.c` (162), `led.h`, `warp_data.h`, `item_names.h`;
- platform glue: `game_addresses.c/.h` + `game_addresses/*.h` (~430), `game_release.cpp` (51), `nfp.c` (94).

These do not affect the three games booting/playing; they are LAS's extra features. They belong in the LAS port,
not in nxrt.

### In nxrt (shared), deliberately not taken

- **Translator recolour** (`emit_recolour_window`, `ltint[4]`): **now in** (M1 — the 16-float tint hook and the
  translator's recolour branch); the LAS recolour module that drives it is P1 port work.
- **Vulkan chunked heap** (`maxwell_gmem_chunk_shift`/`lazy`, windows/buffer references): ~300 lines, Vulkan-only.
  The Vulkan backend still reads the controls; the current translator binds one heap buffer.
- **Kernel diagnostics**: beta_5's `KTRACE` and richer thread trace (~464 lines of delta). The scheduling half of
  that kernel deadlocked EoW and stays out; the hang dump is already ported.

## Unknowns, with risk

- **Vulkan**: the code is beta_5's, unchanged, but this machine cannot run it. It built when the branch was created
  (beta_5 did); it has not booted a game here. The translator swap does not emit beta_5's chunked-heap code, so the
  Vulkan backend runs its one-buffer path (what nxrt used before) — functional, but not beta_5's fast path on
  AMD/Intel.
- **Sky**: ~240 draws/frame are skipped (one bindless-texture fragment shader). Everything else renders.
- **EoW**: one compute shader still fails to translate (`eb20138600f70018`).

## Verdict

- **Shared runtime: done, to within a few percent of beta_5 by lines, and ahead in places** (package RomFS, applet
  accessor, game-agnostic strings, the three-game verification).
- **Distance to beta_5 is now mostly LAS game code**: ~6.8k lines / 27 features in the LAS port, plus ~0.4k lines
  of translator recolour/chunking and ~0.4k of kernel diagnostics.
- **The one real unknown is Vulkan**, not line count: same code, no runtime evidence yet.

Reproduce:

```zsh
git clone --depth 1 -b beta_5 https://github.com/peiikai/LASRecomp /tmp/las_beta5
# then the file/line comparison in this document's method section
```
