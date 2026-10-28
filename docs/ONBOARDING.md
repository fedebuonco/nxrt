# Onboarding a game into nxrt

This is the repeatable path from a dumped title to a runnable port, and the checklist for extending the tooling when a
new title exposes a gap. The current end-to-end target is **Tomodachi Life: Living the Dream** (`tlltd`).

## 0. What a port is

A port is a small directory outside nxrt:

```
port/
  Makefile                 # GAME_ID, GAME_EXE, GAME_MODULES, GAME_SRC; includes $(NXRT)/nxrt.mk
  runtime/<id>_game.c      # NxGame metadata (title/build id/modules) and, later, hooks
  data/extracted/exefs/    # main, main.npdm, sdk, rtld (from the package)
  data/extracted/romfs/    # romfs, if the game needs it
  generated/               # produced by `make gen`; do not edit
  build/                   # produced by the build; do not edit
```

The nxrt runtime, recompiler and HLE stay shared. Only the game dylib (generated C) and the port's hooks are per game.

## 1. Check the inputs and scaffold

```
python3 tools/onboard.py scaffold --game-dir /path/to/extracted --id mygame \
    --name "My Game" --title-id 0x0100... --out ~/ports/mygame
python3 tools/onboard.py check --port ~/ports/mygame
```

`check` validates the Makefile, `exefs/main`, `main.npdm`, `exefs/sdk`, `generated/`, the executable and the game dylib.
It also resolves `main`'s imports across the recompiled `sdk` before reporting runtime HLE gaps: an import is only a
gap if no module exports it and `runtime/hle/hle_symbols.txt` does not cover it.

## 2. Generate and build

```
make gen B=build NXRT=/Users/fede/mastersw/build/nxrt-macos
make -j8 B=build NXRT=/Users/fede/mastersw/build/nxrt-macos
```

## 3. Run with a deterministic smoke test

```
python3 tools/onboard.py run --port ~/ports/mygame --nxrt /Users/fede/mastersw/build/nxrt-macos --frames 300
```

This sets `NXRT_DATA_ROOT`, runs the port, takes a screenshot at the given frame, and on failure runs
`tools/crash_report.py` automatically. The same run can be done manually:

```
NXRT_DATA_ROOT=$PWD/data NXRT_NO_LAUNCHER=1 NXRT_WINDOW_SHOT=300 \
NXRT_SHOT_PATH=build/onboard.png NXRT_EXIT_AFTER_SHOT=1 ./build/mygamerecomp
```

## 4. Triage a failure

```
python3 tools/crash_report.py --port ~/ports/mygame --log build/onboard.log
```

It parses the crash block and prints:

- the guest PC and module (`main +0x...`),
- the NSO function start and the first instructions from `eh_frame`/symbols,
- the generated C definition (`void f_<addr>(Cpu *CPU_RESTRICT c)`) and its first lines,
- guest registers (`x0..x3`, `x29`, `sp`, `lr`),
- the last kernel/HLE lines.

Typical causes, in order: a missing/incorrect HLE return, a translated instruction with bad register semantics, a
guest pointer built from an unhandled SVC/import, or a game-specific hook that the port needs.

Existing diagnostics: `NXRT_TRACE_SVC=1`, `NXRT_FNPROF=<from>:<to>` (build with `FNPROF=1`, now wired into
`nxrt.mk`), `NXRT_SP_CHECK=1` (guest-function entry SP guard; `=2` aborts on the first bad entry), `NXRT_CRASH_DUMP=1`
(dump the object/array words around x0/x1/x22/x25 at the fault),
`NXRT_FNPROF_BT=<addr>`, `NXRT_LOG_HLE`, the `[kernel]`/`[hle]` logs, and `tools/log_triage.py`.

## Plan / status

| Phase | Status |
|---|---|
| Audit tools and gaps | done — inventory above |
| Port readiness checker | done — `onboard check` (cross-module import resolution) |
| Scaffold generator | done — `onboard scaffold` |
| Build/run/smoke loop | done — `onboard run` |
| Crash diagnosis | done — `crash_report` |
| Guide and tests | done — this file, `test_onboard`, `test_crash_report` |
| Tomodachi end-to-end | **no verified frame**: blocked at the virtual call `main_023.c:10029` (`L_700017b384`); the dispatch object is valid but `x1=1`, and `x1` is already 1 at `L_700017a590` before the loop loads `x25`. Port structurally ready. |
| Generic CI port build | open (template build is in CI) |

## 5. Make it repeatable

Record per-game findings in the port (not in nxrt): data root, required firmware/HLE, known unsupported service calls,
hacks/hooks, screenshot frame and expected size. Keep the smoke command in the port README. Add a `tools/smoke.py`
entry only when the game is a supported regression target.

## Tool inventory and gaps (2026-10-28)

Already present:

- `tools/gen.py`, `make gen` — NSO → C, one command.
- `tools/survey.py` — build id, imports/exports, HLE list.
- `tools/build_port.sh` — builds FE3H/EoW ports (hard-coded supported games).
- `tools/smoke.py`, `tests/run-gpu.sh` — screenshot and GPU checks.
- `tools/mac_profile.py`, `shader_libs.py`, `log_triage.py`, `fnprof`, kernel trace.
- frontend-generated ports (`nxrt-frontend`) with NxGame metadata.

New in this onboarding pass:

- `tools/onboard.py check|scaffold|run` — readiness, skeleton, build/run+crash-report loop.
- `tools/crash_report.py` — guest-PC → NSO → generated-C explanation.
- `tests/test_onboard.py`, `tests/test_crash_report.py`.

Gaps still open:

- **Generic port build in CI**: `build_port.sh` only knows FE3H/EoW. A template build is in CI; add a synthetic port
  build once the scaffold is exercised more.
- **Import/stub semantics**: unresolved imports (e.g. `glslc*` in Tomodachi) are bound to host stubs. The check reports
  them as warnings; a title that actually calls one will fail at the first call. Add a runtime log/counter for stub
  calls and a way to mark an import as "must implement".
- **Automatic title id**: `main.npdm` parsing is intentionally left to the frontend; scaffold takes `--title-id`.
- **Crash tool**: reports the function but not a source-line/instruction offset (generated C is not built with `-g`);
  add a `--disassemble-function`/address-window mode if needed.

## Tomodachi Life status

Port: `/Volumes/FE3HDev/tomo`, input structurally complete.

```
python3 tools/onboard.py check --port /Volumes/FE3HDev/tomo
# all required checks OK; 779 imports, 771 resolved by sdk, 8 glslc* unresolved (WARN)
```

The port reaches shader/pipeline setup and then faults in guest code. `crash_report` maps the failure to
`main +0x178b00`, function `f_7000178b00`; the guest frames are
`f_7000178b00 <- 0x700017871c <- 0x70001775a4 <- ...`. A debug-symbol rebuild (`GENFLAGS` with `-g`, then `atos`)
maps the host PC (`f_7000178b00+0x38`) to `main_023.c:7233`, the first generated statement:

```
{ uint64_t ea = c->sp - 96; ST64(ea, c->x[29]); ... }
```

The initial debug mapping showed this prologue store, which looked like a guest `sp` of `0x61`; that was misleading.
The `NXRT_SP_CHECK` entry guard later showed valid SP at every entry. The real fault is the same small address
appearing as a **pointer argument** in the vector helper (`f_700223302c`, `x1 = 1`), described below. The 8 unresolved
`glslc*` imports are only warnings; a title that calls them will need HLE implementations.

I added the reusable `NXRT_SP_CHECK` guard to generated function entry (`FN_ENTER`). A clean Tomodachi build with
`FNPROF=1` shows **every entry on this path has a valid `c->sp`**, so the SP guard ruled the stack hypothesis out.
The same instrumented run then faulted in the vector helper with `x1 = 1`, which `crash_report` now flags directly
(`x1 equals the fault address`). At function entry `x1` is the guest stack pointer; it becomes `1` inside
`f_7000178b00`. The candidate path is `L_700017a13c` (`c->x[1] = 1`) -> `L_700017a5d4` -> `L_700017b384`; the last
site only sets `x0 = x25` and calls the vtable target `f_700223302c`, which immediately reads a Vector2 through `x1`.
So either the virtual target/object (`x25` vtable) is wrong, or `x1` must be replaced before `0x700017b384`. A temporary
probe that set `x1 = x25 + 0x80` (a valid Vector2) did **not** fix it: the crash moved to another bad object address
(`0xfffffe1e00000023`), so the object/state feeding this dispatch is already wrong before the call. The 8 unresolved
`glslc*` imports remain warnings only.

A debug-symbol crash run is reproducible with `GENFLAGS='-O1 -g ...'` and
`atos -o <port>/build-dbg/<id>game.dylib -l <slide> <host pc>`.
