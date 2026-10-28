# CPU codegen evaluation (shadow registers / LTO)

_After the renderer split was stable, 2026-10-27._

## Current model

- Generated guest code is C: `void f(Cpu *CPU_RESTRICT c)`, guest registers are `c->x[]`, `sp`, `pc`, `nzcv`, `v[]`.
- `CPU_RESTRICT` means guest-memory stores cannot alias the `Cpu`, so the compiler may keep fields in host registers across
  a basic block.
- Guest calls are opaque C calls: the caller writes `c->x[30]`/`c->pc`, the callee reloads. That boundary, not the
  1 KB struct itself, is the likely cost as titles scale.

## Measurements

Representative generated module: FE3H `generated/main/main_000.c` (1.22 MB C).

| flags | object size | compile time |
|---|---|---|
| `-O1` (current `GENFLAGS`) | 259,352 B | 1.07 s |
| `-O2` | 265,216 B (+2.3%) | 1.19 s (+11%) |

Runtime evidence on the tested titles:

- FE3H title: ~59.3 fps, `[pace] cost` p50/p95 ~1.4/6.4 ms, pipeline compile 0 ms once warm.
- EoW: full Suthorn Ruins frame; no translation/pipeline wait growth in the steady state.
- The CPU struct is not showing up as a chokepoint in these scenes; the 60 fps cap hides any small gain.

## Options and risk

1. **`-O2` on generated code**: cheap experiment (rebuild one module, +2% code, +11% compile). Low risk, but no measured
   headroom to recover while the GPU cap is 60.
2. **LTO**: the game code is a separate dylib linked with `-dynamiclib -undefined dynamic_lookup`; enabling LTO means
   changing compile/link flags for every generated module and the final dylib, a much larger build and code-size risk.
   It is not justified without a CPU-bound profile.
3. **Shadow registers per function**: requires emitter changes (locals for x0–x30, sync at calls), a `Cpu` ABI audit
   around HLE/signals/threads, and a new validation matrix. Not a drive-by change.

## Recommendation

Profile a CPU-bound scene first (`NXRT_FNPROF`, `tools/bench.py`, `[pace] cost`). If the cost is call-boundary state
traffic, implement shadow registers behind a build flag and A/B it. Only then try per-module `-O2` and an LTO branch.
Do not rewrite the CPU state model based on the 60 fps titles we have.

## How to reproduce

```
# generated-code size/time
clang -O1 ... -c generated/main/main_000.c -o /tmp/o1.o
clang -O2 ... -c generated/main/main_000.c -o /tmp/o2.o

# runtime
python3 tools/smoke.py fe3h
python3 tools/bench.py --from 5 --secs 10 --log run=...
```
