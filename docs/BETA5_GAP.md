# How far is nxrt's shared runtime from LASRecomp `beta_5`?

Measured 2026-10-23 against **`peiikai/LASRecomp` `beta_5` = `f7b986e`** (2026-10-22), a read-only clone in
`/tmp/las_beta5`, compared with `nvn-renderer-wip`. This document covers the **shared runtime only** — the part every
game built on nxrt uses. beta_5's LAS game code (its 16.3k lines of randomizer, overlays, cosmetics and version
tables) is deliberately excluded; that belongs to the LAS port and is not library work.

**Verdict: 96.5% of beta_5's shared runtime lines are in place, every shared file exists, and what is left is
roughly 650 lines of UX plus getting Vulkan to build and run.**

## Method

- Every `.c/.h/.m/.mm/.cpp` under beta_5's `runtime/` was matched by file name against `runtime/`, `platform/`,
  `settings/`, `input/`, `save/`, `ui/` here (the layers nxrt split out), then compared by line count.
- Backend files were diffed to separate real differences from the `LARECOMP_*` → `NXRT_*` renames.
- A "shared" file is one that has a same-name counterpart in nxrt; beta_5's game files (no counterpart) are not
  counted at all.

## The numbers

| shared runtime (beta_5 f7b986e) | lines | files |
|---|---:|---:|
| beta_5's shared files | 43,173 | 101 |
| nxrt's lines in those same files | 41,680 | 101 |

| nxrt (`nvn-renderer-wip`) | lines | files |
|---|---:|---:|
| `runtime/` + `platform/ settings/ input/ save/ ui/` | 45,305 | 129 |
| — in files beta_5 also has | 41,680 | 101 |
| — nxrt-only (nx-ui split, package RomFS, mods, settings layer, timer/NFP/rumble mechanisms) | 3,625 | 28 |

**96.5% of the shared lines, 100% of the shared files.** The nxrt-only lines are refactors, not gaps: the runtime was
made game-agnostic and gained mechanisms beta_5 had inside its game layer. Area by area:

| area | beta_5 | nxrt | state |
|---|---:|---:|---|
| kernel/loader/ipc/dispatch/gamebuild/cache | ~6.5k | 8.6k | nxrt's line plus the ported hang dump; beta_5's extra ~464 lines are diagnostics/scheduling, and the scheduling half deadlocked EoW (`e481c2a`) — stays out |
| HLE (shared part) | ~1.4k | 1.7k | `movie`, `pl`, `heaps`, `vi`, `swkbd`, `libc`; beta_5's `input.c`/`bindings.c` are nxrt's `input/` |
| services | 3.1k | 3.2k | equal, plus nxrt fixes (package RomFS, applet accessor, crash texts, NFP, vibration) |
| NVN Metal (shared) | ~13.1k | 13.3k | beta_5's renderer + nxrt's fixes (tint hooks, crop, clear kind, command shadow, sampler map) |
| NVN Vulkan | 10,663 | 10,663 | beta_5's, byte-for-byte apart from env renames — **never run here** |
| UI (generic) | ~2.3k | 2.9k | menu, option_rows, pad_nav, osk, launcher (nx-ui split) |
| platform | 1.9k | 3.0k | split out, plus session log/crash/thread helpers |

## What is left in the shared runtime

| item | what's actually left | size |
|---|---|---:|
| **Vulkan backend** | beta_5's code, never compiled here: **38 references to LAS-only symbols** (`pot_tint`, `recolour_tint_draw`, `texpatch_*`, `escape_bottle_tint`, `pets_*`), no `RECOLOUR_TINT_FLOATS`, no tint v2, no chunked heap | ~150 lines of decoupling + the chunking, then a Vulkan machine to verify |
| **Startup shader preparation** | `shader_prep_run` + progress bar + Skip on the start screen; the code is imported, but only reachable through the `NXRT_SHADER_PREP` test at device init | ~35 lines of launcher wiring |
| **Overlay caching** | beta_5 renders HUD overlays into a cached texture (`g_ov_cache`); here they are re-rendered every frame | ~70 lines |
| **Controls rows for the new mechanisms** | rumble on/off + intensity, LED on/off, LiveSplit on/host/port — mechanisms and ini keys exist, the rows do not | ~50 lines |
| **`opt_colour` row** | the colour-picker row type in `option_rows` | ~20 lines |
| **OneDrive warning** | Windows start screen warning when the data root is in a synced folder | ~70 lines |
| **Kernel diagnostics** | beta_5's `KTRACE`/wait trace; optional developer tooling | ~460 lines |

That is the whole shared list: **~650 lines of UX plus Vulkan** (the kernel diagnostics are optional and not needed by
any game).

## Two things that look like gaps but are not

- **Files smaller than beta_5's are mostly splits**: `launcher.mm` (832 here + `rt_ui.mm`), `settings.c` (62 +
  `rt_settings.c`), `input.c` (566 + `bindings.c`), `controls_ui.mm` (114 + the binding rows). Line counts per name
  understate nxrt's code there.
- **Deliberately not taken**: beta_5's kernel scheduling changes (deadlock on EoW) and, for now, the Vulkan chunked
  heap. The translator recolour that was on this list is **in** now (16-float tint + recolour branch).

## The one unknown

**Vulkan.** The code is beta_5's, unchanged, but this machine cannot run it and it has never been compiled here. Its
line count is complete; its runtime behaviour is unproven. Everything else shared — kernel, HLE, services, Metal NVN,
UI framework — is in and exercised by LAS, EoW and Sky.

Reproduce:

```zsh
git clone --depth 1 -b beta_5 https://github.com/peiikai/LASRecomp /tmp/las_beta5
# then the file/line comparison in this document's method section
```
