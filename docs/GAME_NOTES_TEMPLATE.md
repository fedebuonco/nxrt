# <Game> — Mac port notes

Copy this file to `games/<id>/README.md` (or keep the port's notes beside its `nx_game.info`). One section per fact;
link a shared finding instead of repeating it.

## Identity

- Title, title ID, base build, update tested, `nx_game.info` module layout.
- The game files used for the measurements (region, version, DLC), never their contents.

## Profile

- `mac.tsv`: scale, frame rate, cap, upscaler, fullscreen, refresh, TDP recommendation, and why each differs from the
  default.
- Shader facts (`shader_facts.tsv`): the hashes pinned and the evidence for each.

## Bring-up

- What the game needed beyond a plain recompilation: HLE services, hooks, patches, boot profile.
- The first frames: shader preparation coverage (`NXRT_SHADER_STRICT=1`), first visits, skipped draws.

## Graphics

- API paths used (NVN variants, compute, movies), shader containers, pipeline counts.
- Metal 4 on/off (`NXRT_METAL4`), archive on/off, any pipeline that still builds at first use.
- Visual checks done: scene, expected picture, screenshot path.

## Performance

- Route (`bench.tsv`), window, frame p50/p95/p99, GPU time, energy, wakeups (`tools/bench.py`).
- Sleep/wake: cycles run, audio, GPU, saves.
- Open regressions and the measurement that would show them.

## Open

- What is not understood, with the log line or symptom and the step that reproduces it.
