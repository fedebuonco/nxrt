# Steam Deck branch

**Target:** Steam Deck / SteamOS, Linux x86-64, Vulkan, Game Mode. This branch is intentionally non-building while the
Vulkan/Linux target is brought to parity.

## What was removed

- macOS platform (`platform/macos.c`), Metal renderer (`runtime/nvn/metal.m`, `runtime/nvn/renderer.m`) and the Metal
  UI backend (`ui/backends/metal.mm`, `nx_ui_metal.h`).
- Windows platform and packaging (`platform/windows.*`, MSYS2/zig packaging scripts, PE CFG helper).
- The macOS UI host example and Metal-only tests/tools.
- Metal-only Dear ImGui backends and examples.
- macOS/Windows branches in `nxrt.mk` and `nx.mk`.

## What is missing before this branch builds

1. **A Vulkan `NxUiBackend`.** `runtime/main.c` references `g_ui_backend_vulkan`, but nothing defines it. Dear ImGui's
   Vulkan glue exists in `runtime/nvn/vulkan/imgui_glue.cpp`; the `NxUiBackend` wrapper (window, frame begin/end,
   screenshot, texture upload) is still to write.
2. **A Linux build.** The Linux/Vulkan path has never been built here. Expect link errors from LAS-only symbols in
   `runtime/nvn/vulkan/exec.c` (`recolour`, `screen_filter`, `pot_*`) and from the missing UI backend.
3. **SteamOS integration.** Game Mode launch, suspend/resume, controller/trackpad mapping, 800p/60/90 Hz profiles,
   TDP and frame-cap settings.
4. **Deck shader pipeline cache.** Build Vulkan pipelines for the Deck's GPU/driver and key the cache by Mesa/driver,
   game build and translator version.
5. **Testing.** No macOS/Metal fallback remains; validate on a Deck or a SteamOS container.

## Build prerequisites

clang, lld, make, Python 3, SDL3, FFmpeg, Vulkan loader/headers, shaderc, and the usual Linux development tools.

## Rule

Do not merge this branch into `master` until it builds and runs at least one port on a Deck. The branch is a target
cut, not a working release.
