# Third-party components

nxrt and the game folders include or link against the components below. Each keeps its own license, held in the file
named here. Nothing in this repository includes Nintendo game code, fonts or keys: players supply their own.

## Bundled source

| Component | Where | License |
|---|---|---|
| Dear ImGui | [`third_party/imgui`](third_party/imgui) | MIT — [`LICENSE.txt`](third_party/imgui/LICENSE.txt), © 2014–2025 Omar Cornut |
| SMAA | [`third_party/smaa`](third_party/smaa) | MIT — [`LICENSE.txt`](third_party/smaa/LICENSE.txt), © 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez |
| xxHash | [`third_party/xxhash`](third_party/xxhash) | BSD 2-Clause — [`LICENSE`](third_party/xxhash/LICENSE), © 2012–2021 Yann Collet |
| ReShade headers | [`third_party/reshade`](third_party/reshade) | BSD 3-Clause — [`LICENSE`](third_party/reshade/LICENSE), © 2014 Patrick Mours |
| Baloo 2 font | [`third_party/fonts/Baloo2`](third_party/fonts/Baloo2) | SIL Open Font License 1.1 — [`OFL.txt`](third_party/fonts/Baloo2/OFL.txt), © 2019 The Baloo 2 Project Authors |

## Build and runtime dependencies

Not bundled with this source release; installed by the player or the developer.

| Dependency | License |
|---|---|
| SDL3 | zlib |
| FFmpeg | LGPL 2.1+ (the macOS app links a build configured for LGPL) |
| Clang / LLVM (the compiler the runtime invokes) | Apache 2.0 with LLVM exceptions |
| zig (optional, used by packaged Windows builds as `zig cc`) | MIT |
| Metal (macOS), Vulkan | Apple SDK terms / Apache 2.0 |

The Python packages the recompiler needs in `tools/venv` (capstone, lz4, unicorn) are installed locally and are not
committed: capstone is BSD 3-Clause, lz4 is BSD 2-Clause, unicorn is GPL 2.0 (used only as a development tool, not
linked into any shipped binary).

## Games and ports

The three ports built on nxrt — Link's Awakening (LASRecomp), Skyward Sword HD (SkyRecomp) and Echoes of Wisdom
(EoWRecomp) — are **not distributed in this repository**. Each lives in its own `games/<id>/` folder with its own
`LICENSE`, and each is MIT with its upstream notices kept:

| Port | License | From |
|---|---|---|
| LAS Recomp | MIT | [LASRecomp](https://github.com/peiikai/LASRecomp), © Peiikai |
| Sky Recomp | MIT, derived from LAS | [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp), © Apolinario-coder |
| EoW Recomp | MIT | [EoWRecomp](https://github.com/Nycolazs/EoWRecomp), © Peiikai and EoWRecomp contributors |

[`examples/game-template`](examples/game-template) is nxrt's own starting point: MIT, like nxrt, with no separate file.
