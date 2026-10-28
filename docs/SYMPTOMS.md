# Symptoms (macOS)

Search this page for the log line or the symptom. Each entry says what it means, what to check, and the fix. It is for
the Mac line (`macos-core`); entries are added when a failure is understood, not when it is first seen.

## Build

### `fatal error: 'stdio.h' file not found` / `no such sysroot directory: ...`

**Cause:** `clang` on `PATH` is Homebrew's, which does not find the macOS SDK by itself.
**Fix:** build with Apple's clang: `make CLANG=/usr/bin/clang` (or `SDKROOT=$(xcrun --show-sdk-path)`).

### `make: *** No rule to make target 'all'`

**Cause:** run from the repository root; the root has no Makefile.
**Fix:** build an app: `make -C examples/game-template`, or a port's `games/<id>` folder.

## Metal and shaders

### `[metal] shader archive not saved`

**Cause:** Metal's archive writer failed. On macOS 26 the known case is #52: `serializeToURL` crashes or errors when a
queued entry lacks a part. The archive is off by default for this reason.
**Check:** `NXRT_METAL_ARCHIVE` (1 enables it), `cache/shaders.metalarchive*`, and the log line before the failure.
**Fix:** leave it off while the work is prepared at the start screen; the archive is being replaced by compiled
functions/libraries plus the Metal 4 path (B1/B2 in [`MAC_PLAN.md`](MAC_PLAN.md)).

### `[metal] shader archive unreadable` / `from another build / GPU / macOS`

**Cause:** the archive's tag (build, translator, GPU, macOS) does not match this run, or the file is damaged.
**Fix:** delete `cache/shaders.metalarchive` and `cache/shaders.metalarchive.tag`; the next start rebuilds it. Do not
copy an archive between Macs.

### `[shader] first visit` / a hitch the first time a scene appears

**Cause:** a shader/pipeline the preparation did not cover; it was translated and built during play.
**Check:** `NXRT_SHADER_PREP` (0 disables the start-screen preparation), the prepared/seen list, and `[slow]`/`[pace]`
lines around the hitch.
**Fix:** add the state to the game's record list (`NXRT_RECORD_KEYS`), or accept it as an uncovered variant. The strict
gate (B3) turns this into a failure instead of a log line.

### `[shader] prepared pipeline failed` / `pipeline build failed`

**Cause:** the translated MSL did not build a pipeline: a wrong key kind (integer attribute/render target), a missing
attachment format, or a translator bug.
**Check:** `NXRT_SAVE_MSL=1` for the generated source, the shader's hash, and the game's `shader_facts.tsv` (A3).
**Fix:** pin the attribute/RT kinds in the facts table; if the source is wrong, the translator is the place to fix it.

### The game window stays black while the log is busy

**Cause:** the start-screen preparation is still compiling variants, and PLAY is waiting on it.
**Check:** `[shader] prepare` lines, CPU use, and whether the game has a shipped variant list.
**Fix:** let it finish once; on later starts the caches should make it short. `NXRT_SHADER_PREP=0` isolates the
preparation from a rendering bug.

## Display and frame pacing

### A 30 fps game looks uneven on a 120/144/165 Hz display, or fullscreen changes the rate

**Cause:** the display mode and the game's step do not match; the Mac line has no refresh matching yet (C2).
**Check:** the display's current rate, `[pace]` lines, and whether the window is fullscreen.
**Fix:** set the display to a multiple of the game's rate (60/120) or run windowed; refresh matching is planned.

### Frames miss 16.7 ms although the GPU looks idle

**Cause:** macOS clocks the GPU to the lowest state that meets each frame; a heavier frame can miss before the clock
rises. This is measured, not assumed (`powermetrics`).
**Check:** `NXRT_LOG_PACE=1`, `NXRT_GPU_PASSES=1`, `powermetrics --samplers gpu_power`.
**Fix:** the measured options are in RESTRUCTURE (C5/C6/C8). Do not "fix" it with a shorter present deadline; that was
measured and reverted.

## Sleep, audio, hangs

### Audio is silent after the Mac sleeps and wakes

**Cause:** the Mac line has no sleep/wake handling yet (A9); the audio device is not restarted.
**Check:** the log around the wake, and whether the game resumes at all.
**Fix:** restart the app for now. A9 adds pause/drain/restore.

### A `[hang]` dump or a frozen window

**Cause:** a guest thread is waiting on something the host never signals.
**Check:** the `[hang]` block (every guest thread and its wait), the last `[svc]` lines with `NXRT_TRACE_SVC=1`, and
whether it follows a save, a load or a sleep.
**Fix:** identify the wait kind and address; add a test to `tests/` before changing the kernel (A4 covers the known
lost-wakeup case).

### A crash alert instead of a window

**Cause:** a host exception; the Mac line writes `lasrecomp.log` (five backups) and shows the explanation in an alert.
**Check:** the newest log in the data folder, the crash report in Console, and the last `[metal]`/`[shader]` line.
**Fix:** the alert names the fault; a Metal validation error needs `MTL_DEBUG_LAYER=1` for the call that failed.
