# Logging in nxrt

The runtime and the generator write one line per event, with a level, a category and (when the manager starts the
process) a run id. The manager's machine protocol on **stdout** (`::step`, `nxrt_progress`, `nxrt_setup`, `nxrt_status`,
`nxrt_describe`) is untouched: logs go to stderr and, optionally, a file.

```
[2026-10-26T01:45:12.345Z] [INFO] [app] [run=1792977374] nxrt runtime started
[2026-10-26T01:45:12.412Z] [INFO] [settings] [run=…] loaded /…/nx_app.ini
[2026-10-26T01:45:13.001Z] [WARN] [hle] [run=…] unimplemented: _ZN2nn2oe18GetDesiredLanguageEv
```

## Plan

- One logging core, shared by the C runtime and the Python generator.
- Levels and categories, with per-category levels (`input=trace,gen=debug`).
- A run id passed by the manager, so a manager log line and the game's lines line up.
- Keep the stdout protocol and the generator's progress lines byte-for-byte.
- Instrument the startup/error paths first (settings, input, saves, mods, generator); hot HLE/renderer paths stay at
  debug/trace and can adopt it next.
- Tests for the core and the existing host-test suite; no logging in a test's output unless it asks.

Status: the core, the runtime wiring, the generator, the first modules and the tests are in this patch. The migration
of the remaining `fprintf(stderr, …)` calls (HLE dispatch, crash text, watch tracing) is deliberately incremental:
crash text must not take a logging mutex.

## Levels

| Level | Use |
| --- | --- |
| `error` | the operation cannot continue (a missing file, a failed setup step) |
| `warn` | something is wrong but the game can continue (an unimplemented HLE call, a bad optional setting) |
| `info` | the lifecycle: settings loaded, mods scanned, a backup written, the generator's phases |
| `debug` | what a port developer wants when a step misbehaves |
| `trace` | per-event detail (input, HLE); opt-in only |

Default: `info`. A message at a higher level than the selected one is not formatted (the macros check first).

## Categories

`app`, `pkg`, `romfs`, `nso`, `gen`, `hle`, `input`, `settings`, `save`, `mods`, `gpu`, `audio`, `ui`, `sys`, `crash`.

## Environment

| Variable | Meaning |
| --- | --- |
| `NXRT_LOG` | a level (`error` … `trace`), or `category=level` pairs: `debug`, `warn,input=trace,gen=debug`, `input=off` |
| `NXRT_LOG_FILE` | append the same lines to this file as well as stderr |
| `NXRT_RUN_ID` | the manager's id for this run; shown as `[run=<id>]` (sanitized) |

Examples:

```sh
NXRT_LOG=debug build/eowrecomp --play
NXRT_LOG=input=trace,gen=debug NXRT_LOG_FILE=/tmp/nxrt.log build/fethrecomp --play
NXRT_RUN_ID=$(date +%s) NXRT_LOG=info build/eowrecomp --play
```

The manager sets `NXRT_RUN_ID` for every run; `NXRT_GAME_LOG` (nxrt-frontend) is passed as `NXRT_LOG`, so a verbose
manager session is a verbose game too.

## The C API

`runtime/nx_log.h`:

```c
NX_LOGE(NX_LOG_SETTINGS, "cannot write %s", path);
NX_LOGW(NX_LOG_HLE, "unimplemented: %s", name);
NX_LOGI(NX_LOG_SAVE, "backup of %s: %d files", src, n);
NX_LOGD(NX_LOG_MODS, "mod #%d: %s", i, name);
NX_LOGT(NX_LOG_INPUT, "key %s", SDL_GetScancodeName(sc));
if (NX_LOG_ENABLED(NX_LOG_TRACE, NX_LOG_INPUT)) { /* expensive inspection */ }
```

`nx_log_init()` is called first thing in `runtime/main.c`. `rt_log` (the runtime's old `LOG`) now forwards to
`nx_log_v(NX_LOG_INFO, NX_LOG_APP, …)`, so every existing `LOG` in the runtime gains the timestamp, level, category and
run id. `nx_log_set_sink()` lets a host test capture lines; `nx_log_reset()` forgets the environment, the file and the
sink.

## The generator

`tools/nxlog.py` writes the same format for `tools/gen.py` (`[gen]`). It reads `NXRT_LOG`, `NXRT_LOG_FILE` and
`NXRT_RUN_ID`. `report.py`'s progress lines are **not** logged: the start screen's setup reads them, so they keep their
exact format.

## What is instrumented now

- runtime startup and lifecycle (`rt_log` → `app`), settings load/save, input and bindings, save backups, mods scans;
- the generator's start, completion and a traceback on failure;
- the tests (`tests/test_log.c`).

Next, as those paths are touched: HLE dispatch misses, the renderer/NVN init, package/RomFS readers, the crash reporter
(without taking the log lock), and rate-limited trace logs for per-frame paths.

## Tests

```sh
sh tests/run.sh test_log
```

The test covers levels, per-category filters, `category=off`, the line format, the run id and the sink. The settings,
mods, save and bindings tests link the logger because their modules now log.
