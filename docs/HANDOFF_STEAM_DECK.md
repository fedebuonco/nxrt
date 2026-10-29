# Steam Deck handoff

**Branch:** `steam_deck`
**Base:** `macos-core` @ `c41d913` (includes the EH-frame and 32-bit ARM detection fixes)
**Remote:** `origin/steam_deck`
**State:** Linux x86-64 / aarch64 template builds; Vulkan UI and draw path are hook-based. No Steam Deck or SteamOS
runtime test yet.

## What was done

### 1. Rebase on the common macOS line

`steam_deck` was rebased onto `macos-core` so the shared recompiler/runtime fixes are present. The target cut keeps
only Linux/Vulkan:

- macOS/Metal and Windows platform files remain deleted.
- Vulkan backend (`runtime/nvn/vulkan/*`) and `platform/linux.c` were restored where the macOS cut had removed them.
- `nx.mk` / `nxrt.mk` are Linux-only.

### 2. Linux/Vulkan foundation

- `ui/backends/vulkan.cpp` — `NxUiBackend`: window, ImGui init/new frame/render, start-screen frames and screenshots,
  RGBA texture upload.
- Linux platform hooks: `platform/power_linux.c` (power counters skipped), `platform/sleep_linux.c` (suspend
  notification no-op), `platform/display_linux.c` (compositor owns the mode), thread name/core/QoS helpers.
- x86-64 fixes: `dispatch.c` inline-cache atomic store, ELF `__start_*`/`__stop_*` sections, `cpu.h` FPCR/FPSR state,
  software AES in `nxpkg.c` when not AArch64, Linux RomFS copy (no `clonefile`), SDL3_image loading.
- `examples/game-template` builds and links on Ubuntu 25.04 x86-64.

Build packages used:

```sh
clang lld make python3 pkg-config libsdl3-dev libsdl3-image-dev \
libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev
```

### 3. Generalize game hooks (the macOS pattern)

LASRecomp `main` (`build/LASRecomp-main`, clone `b945481`, nxrt submodule `387f71b`) was the reference. It is already
split as "game folder + `NxGame`/`NxGameHooks`", with all LAS code in `runtime/`, `runtime/nvn/texpatch.c` and
`runtime/ui/`.

The Vulkan backend now calls `NxGameHooks` instead of LAS symbols:

- texture `tex_identify`, `tex_generation`, `tex_apply`, `tex_replace`
- `draw_shader`, `draw_textures`, `draw_tint`
- `draw_mirror`, `draw_effect_stretch`
- `instances_tinted`, `instance_tint`
- `present_wide`, `present_crop`, `present_shape`, `present_frame_end`, `present_tick`
- `scene_name`

The temporary LAS stubs (`game_hooks_stub.c`, `texpatch_stub.c`, `recolour.h`, `texpatch.h`) were removed. See
[`LASRECOMP_GENERALIZATION.md`](LASRECOMP_GENERALIZATION.md).

## Current local test setup

A real game port was prepared from the Tomodachi Life recomp:

```
/Users/fede/mastersw/build/tlltd-linux
```

It is a copy of the frontend's `tlltd` game folder (`data/extracted` has `exefs/main`, `sdk` and `romfs.src`). It builds
a Linux binary with:

```sh
make -j2 NXRT=/Users/fede/mastersw B=build-linux
```

producing `build-linux/tlltdrecomp`.

Generation was attempted but is **not complete**:

- The x86-64 container runs under QEMU on Apple silicon; `gen.py` for `main` used ~4 GB RAM and 20+ minutes without
  producing chunks.
- `generated/main` is empty; the process was stopped.
- The Linux `tools/venv` exists in the copied folder.

## How to finish the visual test

Use a **native aarch64** container (not `--arch amd64`) so generation and the runtime are not under QEMU. The Podman VM
is aarch64.

```sh
podman machine set --memory 8192 --cpus 4
podman machine start
```

Build an aarch64 image with the same packages plus `xvfb mesa-vulkan-drivers vulkan-tools`:

```sh
podman run --name nxdeck-arm ubuntu:25.04 bash -lc '
  apt-get update -qq &&
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    clang lld make python3 python3-venv python3-pip pkg-config \
    libsdl3-dev libsdl3-image-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev \
    xvfb mesa-vulkan-drivers vulkan-tools
'
podman commit nxdeck-arm nxdeck-arm:latest
```

Then generate and build Tomodachi natively:

```sh
cd /Users/fede/mastersw/build/tlltd-linux
rm -rf generated build-linux
podman run --rm -v /Users/fede/mastersw:/src:Z -w /src/build/tlltd-linux nxdeck-arm:latest bash -lc '
  python3 -m venv tools/venv
  tools/venv/bin/pip install -q --upgrade capstone lz4 unicorn
  NXRT_GEN_JOBS=1 make gen NXRT=/src B=build-linux
  make -j2 NXRT=/src B=build-linux
'
```

Run with software Vulkan (lavapipe) and Xvfb:

```sh
podman run --rm -v /Users/fede/mastersw:/src:Z -w /src/build/tlltd-linux nxdeck-arm:latest bash -lc '
  export VK_ICD_FILENAMES=$(ls /usr/share/vulkan/icd.d/*lvp*.json | head -1)
  export SDL_VIDEODRIVER=x11
  export NXRT_NO_LAUNCHER=1
  export NXRT_SHOT_AT=120
  export NXRT_EXIT_AFTER_SHOT=1
  xvfb-run -a -s "-screen 0 1280x720x24" \
    ./build-linux/tlltdrecomp extracted --play
'
```

That should write a frame screenshot and exit. If it does not, inspect the log under `data/`/`NXRT_DATA_ROOT` and fix
the first missing service or renderer call. A start-screen-only check can use `NXRT_LAUNCHER_SHOT` instead of
`NXRT_NO_LAUNCHER`.

## Known gaps

- No actual Deck/SteamOS run yet; lavapipe is only a smoke test.
- Tomodachi generation/build and first boot are unverified on Linux.
- Draw hooks are wired, but real-game output (tiles, HUD, widescreen, mirror) needs a visual check.
- No Linux CI (removed on purpose; the workflow runner was unreliable for this branch).
- The macOS frontend stays on `macos-core`; do not point its `nxrt` submodule at this branch.

## Relevant commits

```
4c83718 vk: no game-specific names in the texture cache
ffdc0cf docs: local Linux build instructions after CI removal
2ebbd59 steam_deck: drop the Linux CI workflow
f360bfb steam_deck: fix the texture replace scope
3ba1cef steam_deck: route Vulkan draws through NxGameHooks
1063257 steam_deck: use NxGameHooks for textures and presentation
db5a836 steam_deck: Vulkan UI backend and a Linux template build
51c3933 docs: Steam Deck specialization and shader-plan notes
c41d913 recomp: detect 32-bit ARM modules and ELF32 metadata
```
