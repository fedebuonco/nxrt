# nxrt and nxnative

Two stacks that run Nintendo Switch games natively, compared as engineering. They share one premise — do not emulate the
guest instruction by instruction — and make opposite bets about how to avoid it. That bet determines their platforms,
their compatibility lists, and most of their code.

- **nxrt** (this repository) is a runtime **library** and recompiler. The game's ARM64 is translated to C, compiled for
  the host, and linked into the app. The player's machine builds the game code from their own files at setup.
- **nxnative** ([github.com/seedds/nxnative](https://github.com/seedds/nxnative)) is a complete **product** for Apple
  platforms. The game's ARM64 is rewritten in place and runs directly on the CPU, with a host standing in for Horizon
  and NVN. A build script turns the player's NSPs into a sideloadable IPA or a Mac app.

Neither ships game code, keys or assets; both require the player's own copy. Everything below is from the two
codebases and their documentation. There is no head-to-head benchmark: the comparison is architectural.

## At a glance

| | nxrt | nxnative |
|---|---|---|
| What it is | Runtime library + recompiler, consumed as source | End-to-end product pipeline + host |
| Guest code at run time | Recompiled C, compiled to host machine code | The game's own ARM64, rewritten in place |
| Host ISA | Any clang target; today Linux **x86-64** | **ARM64** only (Apple silicon) |
| Graphics | NVN → **Vulkan** | NVN → **Metal**, plus Vulkan and OpenGL games through layers |
| OS | Linux / SteamOS (macOS/Metal existed before this branch) | iOS / iPadOS 26+, macOS |
| Devices | Steam Deck / SteamOS, Linux x86-64 + Vulkan | iPhone 15 Pro or newer, iPad M1 or newer, Apple silicon Macs |
| Player gets | Source tree; game code built at first-run setup | An IPA to sideload or a Mac app, built from their NSPs |
| Game support | 3 ports (LAS, Sky, EoW) + Tunic in progress | 76 profiles, 74 described as playable |
| Per-game unit | `games/<id>/`: descriptor, C hooks, patches, menu pages | `dm/games/<id>/`: profile TSV, shader tables, `patches.py`, notes |
| Per-game code allowed | Yes, that is the point | Discouraged: profile data, build-gated cuts, and a host branch only where no general rule works |
| Status | `steam_deck` branch is a target cut; it does not build yet | Shipping, CI in GitHub Actions, tested on devices |
| Licence | Unlicense (public domain); ports MIT | MIT |

## 1. The core difference: where the ARM64 code goes

```
nxnative
  NSO (ARM64) ──patch_nso.py──▶ patched ARM64 image ──embedded──▶ app ──▶ runs on the CPU
                 ▲ x18 / tpidr_el0 / SVC gates / SP alignment / spin loops
                 ▲ per-build cuts and frame-step stubs, asserted byte by byte

nxrt
  NSO (ARM64) ──gen.py + lifter.py──▶ generated C ──clang──▶ <id>game.so ──dispatcher──▶ host CPU
                 ▲ HLE symbol table redirects SDK calls to the host
                 ▲ patches.py replaces individual instructions, asserted word by word
```

nxnative keeps the guest ISA. Its rewriter
([`patch_nso.py`](https://github.com/seedds/nxnative/blob/main/dm/tools/patch_nso.py)) changes only what Apple's kernel
forbids or what a host must not busy-wait on: `x18` and `tpidr_el0`/`tpidrro_el0` become thread slots, `svc` and
`cntpct_el0` become calls to a host gate, misaligned stack pointers are rounded, and provably self-contained spin loops
are copied behind a gate that lets the host yield. Everything else — including exclusive loads/stores, memory ordering,
and unusual instructions — executes as Nintendo's compiler emitted it. Per-build cuts are keyed by NSO build ID, so a
cut cannot run on a build it was not found in; function-matched cuts are the verified exception for unregistered builds.

nxrt gives up the ISA. [`tools/gen.py`](../tools/gen.py) disassembles the module and `lifter.py` emits C per function;
the C is compiled by clang into `<id>game.so`, and the runtime dispatches guest addresses into it. An instruction the
lifter does not know becomes a trap at that address, not a silently wrong translation. SDK imports listed in
`hle_symbols.txt` are redirected to host implementations instead of lifted.

### What follows from that choice

| Property | nxnative | nxrt |
|---|---|---|
| Host ISA reach | ARM64 only; the approach cannot produce x86 code | Any clang target; x86-64 is why the Deck port exists |
| Instruction fidelity | Exact by construction | Every instruction must be lifted and modelled correctly |
| Failure mode of a bad change | A wrong cut crashes loudly, gated by build ID | A wrong lift can compute quietly wrong; the differential tests are the guard |
| Guest code that writes code | Runs as on the Switch | A static recompiler has to treat it as a special case |
| Atomics and exclusives | Native instructions, exact semantics | Emulated (LDXR/STXR pairs, CAS/CASP, LSE atomics); x86's stronger ordering helps |
| Work happens | At pack time, on the Mac | At first-run setup, on the player's machine |
| Cost of that work | 30–60 min for a first IPA, about a minute incrementally | A few minutes per game-code build (LAS: 324 s after a recompiler change), reused until the recompiler or ABI changes |
| Optimisation surface | Host code only; guest code is fixed | `-march` (x86-64-v3 now, znver2 on the Deck plan); the generated C is ordinary clang input, so LTO and specialisation are available |
| Build artifact | A patched binary inside the app bundle | Generated C compiled into a `.so` beside the game files |
| Per-title fix | An instruction cut or stub, in data | A C patch or hook, in code |

The last row is also a philosophical difference. nxnative's rule is that everything specific to a title lives in its
profile or in build-gated cuts, and a per-game branch in the host is a last resort. nxrt's `games/<id>/` is a C program —
it may add a randomizer, an overlay, 120 fps timing or a texture restyler. nxnative's aim is to preserve the game as it
was, with deliberate per-build patches on top; nxrt can extend it. That is a product decision, not an accident of either
architecture.

## 2. Graphics and shaders

| | nxnative | nxrt |
|---|---|---|
| APIs | NVN → Metal; Vulkan → Metal; OpenGL → Metal | NVN → Vulkan only on this branch |
| Shader translation | **Offline at pack time**: Maxwell → SPIR-V (Ryujinx's translator) → MSL (SPIRV-Cross) → Metal libraries | **At run time**: Maxwell → GLSL (C translator) → SPIR-V (shaderc), cached on disk |
| Pipeline creation | Metal 4 bases built once, specialised in microseconds; archive of bases | RADV pipelines built on demand, ahead of time only for prepared variants; PSO archive/GPL planned, not implemented |
| Ahead-of-time strategy | `pso_manifest.tsv` + `pso_user.tsv`, a prepare step before the first frame, offline prebuild shipped in the app | `shader_prep.c`: a shipped variant list (`NX_SHADER_LIST`), built at setup and prepared at boot; seen shaders prepared on later runs |
| A never-seen pipeline | Hybrid "pulling" stand-in draws now, the real pipeline builds in the background | Compiles at first use (asynchronously); first-visit cost remains |
| Known gaps | Conditional render always drawn; `MultiDraw*IndirectCount` waits on the CPU; no geometry shaders; TBDR off | Vulkan renderer unfinished on this branch |

The shader difference is the clearest consequence of product shape. nxnative can translate and compile every shader for
a known GPU at pack time because it ships one finished app per game, per Apple GPU. Its offline prebuild puts pipeline
bases in the app, so a cold start has stand-ins for draws it has never seen. nxrt has the same opportunity — it knows
the exact game build — but its translation runs on the player's machine and its pipeline archive is still a plan.

Both cache, but they cache different things. nxrt keeps SPIR-V in `cache/spirv/` keyed by source hash and stage, and
prepares variants before play; it ships no shader code and rebuilds every shader from the player's files. nxnative ships
the Metal libraries it compiled from those shaders at pack time, plus a pipeline archive and two prewarm lists. Neither
treats a pipeline archive as portable: nxnative's is per GPU architecture and per Metal compiler, and nxrt's would be
per Mesa/RADV version. On both, the cache is local state, not an asset.

## 3. The host layer

Both stand in for Horizon: kernel SVCs, threads and synchronisation, memory (heap, alias, stacks), file system, HID,
audio, applet/AM, and the display/VI path. Beyond that the surfaces differ with the games each has met.

| Area | nxnative (`dm/host`) | nxrt (`runtime/`) |
|---|---|---|
| Kernel / SVCs | `hle_svc.c`, guest memory regions, maps, heap accounting | `kernel.c`, `ipc.c`, loader, dispatcher, TLS |
| Services | 36 `hle_*.c` units: account, friends, net, Mii, codec, movie (VP9/Vorbis), haptics, lifecycle, software keyboard | 8 service modules (`fs`, `hid`, `audio`, `am`, `acc`, `time`, `nfp`, `misc`) + HLE units for libc, pl, vi, hid, movie, swkbd, heaps, NVN boot |
| File system | Transactional saves; NSP/RomFS read at pack time, then plain files | RomFS/`.nsp`/`.xci` read in place or extracted; LayeredFS mods; texture packs |
| Audio | Core Audio; the SDK's audren in-process, or AudioOut direct | `audren`/`audout` HLE; SDL3 audio output |
| Input | Touch gamepad + physical controllers on iOS; keyboard/controllers on Mac; gyro shake; touch gestures | SDL3 controllers/keyboard/mouse, rebinding, dead zones, prompt style |
| App surface | SwiftUI launcher, tested defaults, frame-rate counter, logs | Dear ImGui start screen, in-game menu, on-screen keyboard, save backups, update check |

nxrt's host is split into layers with one-way dependencies — platform ← settings ← input/save ← ui ← runtime ← game —
and the build enforces it. nxnative's host is one program per game, configured by a generated profile header; the game
is chosen at compile time (`GAME=<id>`), not at run time.

## 4. Product and distribution

| | nxnative | nxrt |
|---|---|---|
| Unit the player installs | IPA / Mac app containing the rewritten game | An app (the port) plus game files the player points it at |
| When the game is prepared | Before install, on a Mac with Xcode | At first run, on the player's machine |
| Front end | iOS launcher; Mac app opens the game directly | Start screen and in-game menu from the `ui/` layer |
| Updates | Rebuild and re-sideload; saves and `pso_user.tsv` can be carried over | App update; game code rebuilds only when `GAMELIB_ID` changes |
| What the developer ships | The whole pipeline and per-game profiles | A library, a game descriptor and the port's own code |
| What does not exist | A library API for another frontend | A Deck front end (`nxdeck` is proposed, not built) |

This is the least symmetric part: nxnative is a product with a packaging story, and nxrt is an engine without one for
its new target. The `steam_deck` branch ships a start screen and a menu but no Game Mode integration, no installer, no
per-title tested defaults, and no build/bench pipeline for Deck.

## 5. Compatibility strategy: breadth vs depth

nxnative's list is 76 profile folders. Its compatibility work is data: constants, shader tables, prewarm lists, build
IDs, and byte-verified instruction cuts. That is why it can cover 74 titles: the rewriter does not need to understand a
game's code to run it, and a new title starts from a survey and a provisional profile.

nxrt's list is three ports (LAS, Sky, EoW) plus Tunic in progress. Its compatibility work is code: the recompiler must
lift the game correctly, and each port carries hooks, menu pages and patches. Its strength is depth — a port can add
features a general host cannot, and fixes in the shared runtime reach every port at once.

Neither number is a scoreboard. They measure different products: an execution engine that must accept unknown code, and
a porting kit that produces a tailored app.

## 6. Performance work

Both projects spend most of their performance effort on work the Switch never did. The mechanisms differ.

| Concern | nxnative | nxrt |
|---|---|---|
| Guest waits | Yield back-off: after 2 ms of consecutive yields, sleep an eighth of the wait so far (10–250 µs); pure spin loops behind a gate | Waits block on condition variables; a zero-length `SleepThread` is a plain `sched_yield()` ([`kernel.c`](../runtime/kernel.c)), with no guest-yield back-off |
| Pure spin loops | Rewriter copies provably self-contained loops behind a spin gate; first 2 ms poll, then sleep | No equivalent: a guest spin loop compiles into a C loop that spins on a host core |
| Frame pacing | Frame-step hold with probes to step back to 60; present-lag tests; real-time QoS for the presenting thread | `pace.c`: measures busy time, holds a game in its 30 fps mode when frames do not fit, drives present interval; 120 fps mode |
| CPU scheduling | QoS classes per guest priority, core classes, lock lending | No QoS equivalent on Linux; Vulkan worker counts follow the process's affinity mask |
| Pipeline cost | Metal 4 bases, archive, prebuild, hybrid stand-ins, background compiles | SPIR-V disk cache, boot-time preparation; RADV archive still planned |
| CPU replay cost | Cached memory-pool addresses, draw state, vertex bindings, residency set | Not applicable in the same form: guest code is compiled C, not replayed |
| Power | Power line every 300 frames, thermal state, thread yield accounting | Deck TDP/refresh/battery plan in [`DECK_SPECIALIZATION.md`](DECK_SPECIALIZATION.md), not implemented |

The most transferable single idea is the yield back-off: a zero-length `SleepThread` in nxrt is a bare `sched_yield()`,
which is the busy spin nxnative measured and fixed, and it is a host-side change, independent of the execution model.

## 7. Codebase shape

Source lines, headers included, counted from each tree. Approximate, and not a quality metric.

| Area | nxrt | nxnative |
|---|---|---|
| Runtime / host | `runtime/` 33.6k (loader, kernel, IPC, HLE, NVN, Vulkan) | `dm/host/` 14.8k (SVC + HLE) |
| Graphics | inside `runtime/nvn/` | `dm/gfx/` 36.8k (Metal, Vulkan layer, GL layer, texture, tools) |
| Platform / app shell | `platform/` 1.8k Linux | `dm/ios/` 2.4k (with Swift), `dm/mac/` 0.4k |
| App layers | `ui/` 2.9k, `input/` 1.0k, `settings/` + `save/` 0.3k | launcher in `dm/ios`, no separate layers |
| Toolchain | `tools/` 5.5k | `dm/tools/` 8.0k |
| Vendored third party | small (imgui, SMAA, xxhash, ReShade headers, font) | 189k: 124k C/C++/ObjC/Metal plus 65k C# (Ryujinx's translator) |
| Game data | outside the repository (`games/<id>/`) | in the repository: 76 profiles, about 1.6M lines of TSV tables |

Two asymmetries explain most of it. nxnative vendors a shader translation chain (a .NET tool built from Ryujinx's
translator) because it must run offline and deterministically at pack time; nxrt writes a 2.5k-line C translator and
calls the system's shaderc at run time. And nxnative's compatibility knowledge is largely data — 1.6M lines of shader
tables and profiles — where nxrt's ports are code that lives outside this repository.

## 8. Testing and CI

nxnative treats the game list as its real test suite, with unit tests underneath: host tests for memory, audio, file
system, HID, condition variables and heap sizing (with ASan/UBSan/TSan targets), CPU-side graphics tests, about two
dozen Python tool tests, Mac builds for NVN/Vulkan/OpenGL profiles, and an iOS compile job. CI is GitHub Actions.

nxrt has a host-side suite that builds and runs without a game, GPU or window — settings, saves, mods, NSO/RomFS/package
parsers, texture layout, shared hashes — compiled with ASan/UBSan; 13/13 pass on this branch. The recompiler has a
differential test against Unicorn, and the renderer has its own translation tests. What the tree does not have is CI
configuration, and the branch does not build on a device yet.

## 9. Licensing and provenance

nxrt is public domain (Unlicense); the three ports are MIT with their upstream notices. Its bundled dependencies are
ImGui, SMAA, xxHash, ReShade headers and a font; SDL3, FFmpeg, Vulkan and shaderc are installed by the player. Unicorn
(GPL) is a development tool only and is not linked into anything shipped.

nxnative is MIT, © 2026 seedds. It started from d2r-mobile (MIT) and vendors Ryujinx's shader translator (MIT),
SPIRV-Cross and glslang, deko3d's texture-layout algorithm (zlib), libvpx, stb_vorbis, the Vulkan headers and NVIDIA
FLIP; NSP extraction uses LibHac.

Both ship no game data and no keys, and both depend on the player's own copy. Static recompilation of console games is
legally contested in several jurisdictions; a permissive licence on the code does not settle that.

## 10. What transfers, and what does not

The prioritized plan — what nxrt already has, what each import costs, and the order with gates — is in
[`NXNATIVE_IMPORTS.md`](NXNATIVE_IMPORTS.md) (macOS).

The valuable part of nxnative for a Deck product is its **precomputation discipline**, not its code:

- offline shader translation and a pipeline archive, with a prepare step before the first frame;
- per-game profiles with tested defaults, keyed by build ID;
- a build pipeline with content-addressed stages and a fast incremental path;
- benchmark routes and a regression harness;
- the yield back-off and spin-wait analysis.

What does not transfer: the ARM-native execution model (the Deck is x86-64), the Metal renderer, the per-title profiles
(they encode a different GPU, driver and OS), and the breadth-first compatibility goal. A RADV pipeline archive is tied
to the Mesa build, the GPU and the compiler, so it cannot be shipped as a universal download the way nxnative ships a
Metal archive for a known Apple GPU.

nxrt's transferable asset is the opposite: a layered, embeddable runtime with a narrow game interface and a recompiler
that reaches any host clang can target — which is the only reason a Deck product is possible at all.

## 11. Bottom line

- On Apple silicon, nxnative is the more complete and more compatible stack, and its execution model is more faithful by
  construction: the game's own instructions run.
- On x86-64 and SteamOS, nxrt is the only one of the two that can exist. Its cost is a recompiler that must be right
  about every instruction, a compile step at setup, and a renderer that is not finished yet.
- They are engines for different products: nxnative preserves a game; nxrt is built to host a port that may add to it.
- For the Deck, the target is not to port nxnative but to reproduce its cold-start and stutter results inside Vulkan and
  RADV constraints. That is what [`DECK_SPECIALIZATION.md`](DECK_SPECIALIZATION.md) plans, and it is why the useful
  imports are the pipeline archive, the profiles and the prepare step — not the loader.

## References

- nxnative: <https://github.com/seedds/nxnative> — internals in
  [`docs/development.md`](https://github.com/seedds/nxnative/blob/main/docs/development.md), profiles in
  [`dm/games`](https://github.com/seedds/nxnative/blob/main/dm/games/README.md).
- nxrt: [`docs/ARCHITECTURE.md`](ARCHITECTURE.md), [`docs/INTEGRATING.md`](INTEGRATING.md),
  [`docs/DECK_SPECIALIZATION.md`](DECK_SPECIALIZATION.md), [`STEAM_DECK.md`](../STEAM_DECK.md).
