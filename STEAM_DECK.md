# Steam Deck branch

**Target:** Steam Deck / SteamOS, Linux x86-64, Vulkan, Game Mode. The template runtime now builds on x86-64 Linux with
the Vulkan backend and UI; per-game ports and Deck integration are the remaining work.

## What was removed

- macOS platform (`platform/macos.c`), Metal renderer (`runtime/nvn/metal.m`, `runtime/nvn/renderer.m`) and the Metal
  UI backend (`ui/backends/metal.mm`, `nx_ui_metal.h`).
- Windows platform and packaging (`platform/windows.*`, MSYS2/zig packaging scripts, PE CFG helper).
- The macOS UI host example and Metal-only tests/tools.
- Metal-only Dear ImGui backends and examples.
- macOS/Windows branches in `nxrt.mk` and `nx.mk`.

## What is done

1. **Vulkan `NxUiBackend`** (`ui/backends/vulkan.cpp`): window, frame begin/end, ImGui rendering, screenshots and
   texture uploads through the existing Vulkan device.
2. **Linux platform layer**: `platform/linux.c`, `power_linux.c`, `sleep_linux.c`, `display_linux.c`; no-op power,
   suspend and display-mode hooks where the compositor owns the mode.
3. **Linux template build**: `examples/game-template` compiles and links on Ubuntu 25.04 x86-64 with SDL3, FFmpeg,
   Vulkan, shaderc and VMA. The generic hooks for LAS-specific cosmetics (`runtime/recolour.h`, `runtime/texpatch.h`,
   `runtime/nvn/game_hooks_stub.c`) are no-ops; a game overrides them.
4. **CI**: `.github/workflows/linux.yml` builds the template and smoke-tests `--describe`.

## What is still missing

1. **A real game port** built and run through the Vulkan backend on a Deck; the template only proves the build.
2. **SteamOS integration.** Game Mode launch, suspend/resume, controller/trackpad mapping, 800p/60/90 Hz profiles,
   TDP and frame-cap settings.
3. **Deck shader pipeline cache.** Build Vulkan pipelines for the Deck's GPU/driver and key the cache by Mesa/driver,
   game build and translator version.
4. **Draw-time hooks.** The Vulkan draw path still has generic no-op stubs for LAS's cosmetic hooks
   (`game_hooks_stub.c`, `texpatch_stub.c`); texture identification/apply and presentation already use `NxGameHooks`.
   See [`docs/LASRECOMP_GENERALIZATION.md`](docs/LASRECOMP_GENERALIZATION.md).
5. **Testing.** Validate on a Deck or a SteamOS container; the template has no game files or GPU here.

## Build prerequisites

clang, lld, make, Python 3, SDL3, FFmpeg, Vulkan loader/headers, shaderc, and the usual Linux development tools.

## Rule

Do not merge this branch into `master` until it runs at least one port on a Deck. The template builds; the branch is
not a working release yet.

## After it builds

[`docs/DECK_SPECIALIZATION.md`](docs/DECK_SPECIALIZATION.md) lays out the Deck specialization axes (codegen, threads,
UMA, RADV/pipelines, pacing, power, storage, suspend, input), what to borrow from nxnative's product model, and the
phased roadmap with gates.
