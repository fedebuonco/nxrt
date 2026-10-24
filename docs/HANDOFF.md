# Handoff: nxrt on Steam Deck

**Branch:** `steam_deck` (local, not pushed)
**Commit:** `0802684` — `steam_deck: target Linux/Vulkan only`
**Base:** `master` at `387f71b` / tag `v0.4.0`

This branch is a deliberate platform cut: macOS/Metal and Windows are gone, Linux/Vulkan remains. It **does not build
yet**. That is expected; the first job on the new machine is to make it build and run one port.

The old macOS/Metal handoff was replaced by this file. Historical notes in `docs/` that mention Metal or macOS are
marked as historical.

---

## What already works

- The host test suite still passes: **13/13** (`tests/run.sh`).
- The Linux-only Makefiles parse (`make -n runtime-objs`).
- The shared runtime, recompiler, game hooks and Vulkan backend sources remain.
- `STEAM_DECK.md` records the cut and the known gaps.

## What was removed

- macOS platform (`platform/macos.c`), Metal renderer (`runtime/nvn/metal.m`, `runtime/nvn/renderer.m`) and the Metal
  UI backend (`ui/backends/metal.mm`, `nx_ui_metal.h`).
- Windows platform and packaging (`platform/windows.*`, MSYS2/zig packaging scripts, PE CFG helper).
- The macOS `ui-host` example, Metal-only tests/tools, Metal ImGui backends and Apple ImGui examples.
- macOS/Windows branches in `nxrt.mk` and `nx.mk`.

## Target

Steam Deck / SteamOS, **Linux x86-64, Vulkan**, Game Mode. 800p class display, 60/90 Hz, Mesa RADV. One hardware
family is the point: fewer drivers, one performance target, prebuilt pipelines, console-style QA.

---

## 0. Get the branch onto the new machine

The branch is local and has not been pushed. Either push it before switching machines:

```bash
git push -u origin steam_deck
```

or copy the repository including `.git` and check out `steam_deck`. Verify:

```bash
git status
git log -1 --oneline
```

## 1. Build environment

On SteamOS, use a writable development environment (distrobox/container or a Deck with a writable toolchain). The
read-only root does not carry the build dependencies.

Needed: `clang`, `lld`, `make`, `python3`, `pkg-config`, SDL3, FFmpeg, Vulkan loader/headers, `shaderc` (or a shaderc
build), and the usual C/C++ development tools.

Quick check:

```bash
clang --version
pkg-config --modversion sdl3
pkg-config --modversion libavformat
pkg-config --modversion vulkan
```

Then run the host tests once:

```bash
tests/run.sh
```

## 2. The first job: make the branch build

### 2a. Add the Vulkan `NxUiBackend`

`runtime/main.c` already does:

```c
extern const NxUiBackend g_ui_backend_vulkan;
nxui_set_backend(&g_ui_backend_vulkan);
```

but nothing defines `g_ui_backend_vulkan`. The Dear ImGui Vulkan glue exists in
`runtime/nvn/vulkan/imgui_glue.cpp`:

- `vk_imgui_backend_init()`
- `vk_imgui_backend_shutdown()`
- `vk_imgui_texture_rgba()`

What is missing is the `NxUiBackend` wrapper. Add `runtime/nvn/vulkan/ui_backend.cpp` (it is picked up by the
`BACKEND_SRC` wildcard in `nxrt.mk`):

```cpp
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "nx_ui.h"      // RTINC already has -I ui
#include "vk.h"
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
}

static SDL_Window *ui_window(void) { return vk_window(); }
static bool ui_init(void) { return vk_imgui_backend_init(); }
static void ui_shutdown(void) { vk_imgui_backend_shutdown(); }
static void ui_new_frame(void *frame) { (void)frame; ImGui_ImplVulkan_NewFrame(); }
static void ui_render(void *frame) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)frame);
}
static void *ui_frame_begin(const float clear[4]) {
    if (!vk_frame_begin(clear)) return NULL;
    return vk_frame_cmd();
}
static void ui_frame_end(void *frame, const char *shot_path) {
    (void)frame;
    vk_frame_end(shot_path);
}
static uint64_t ui_texture_rgba(const uint8_t *rgba, int w, int h) {
    return vk_imgui_texture_rgba(rgba, w, h);
}

extern "C" const NxUiBackend g_ui_backend_vulkan = {
    ui_window, ui_init, ui_shutdown, ui_new_frame, ui_render,
    ui_frame_begin, ui_frame_end, ui_texture_rgba,
};
```

`device.c` keeps its SDL window private; add a getter:

```c
SDL_Window *vk_window(void) { return window; }
```

and declare it in `runtime/nvn/vulkan/vk.h`.

Important details:

- `vk_frame_begin()` already begins dynamic rendering into the window image. Do **not** begin another render pass.
- `vk_frame_cmd()` drains the recorder and returns the window image's command buffer; pass that as the `frame` token.
- `menu.mm` calls `g_backend->new_frame(frame)`, then `ImGui::NewFrame()`, then `render(frame)`. `new_frame` must only
  call `ImGui_ImplVulkan_NewFrame()`; do not call `ImGui::NewFrame()` there.
- `frame_end` calls `vk_frame_end`, which ends rendering, presents, and optionally saves a PNG.
- `texture_rgba` is used by the start screen's background.

### 2b. Decouple `runtime/nvn/vulkan/exec.c` from LAS

`exec.c` was a port of the removed Metal renderer and still calls LAS-only code. At minimum:

```bash
grep -n 'recolour\|pot_\|texpatch\|escape_bottle\|pets_' runtime/nvn/vulkan/exec.c
```

It includes `../recolour.h`, which does not exist in this tree, and calls:

- `recolour_add`, `recolour_class_of`, `recolour_tint_draw`
- `pot_level_active`, `pot_tint`, `pot_glow_active`, `pot_instance_gold`, `pot_gold_tint`
- `texpatch_is_item_cursor`, `texpatch_bottle_kind`, `texpatch_heart_kind`, `texpatch_heart_tint`,
  `texpatch_hint_icon`
- `escape_bottle_tint`
- `pets_hide_item_cursor`

For the first build, remove those blocks from `exec.c` (or guard them behind a clearly named feature flag). Do not
recreate `recolour.h` in `runtime/`: that is LAS's feature, not the generic runtime. The generic texture hooks in
`runtime/nxgame.h` (`tex_identify`, `tex_generation`, `tex_apply`, `tex_replace`, `draw_shader`, `draw_textures`) are
the long-term replacement.

`screen_filter.h` is header-only and fine. Keep `draw_shader`/`draw_textures` and the generic texture path.

### 2c. Build

```bash
cd games/<id>
make -j"$(nproc)" CLANG=clang
```

Use `games/eow` for the first smoke test: its Vulkan path is the least feature-dependent. Expect further errors after
the two above; fix them one at a time.

The game code library is built from the player's own `extracted/exefs` by `make gen`. On a Deck, the setup's game-code
builder needs a compiler: the branch keeps `tools/fetch_zig.sh` (Linux x86-64 is pinned) but removed the Windows-only
`zig_slim.sh`; a system clang also works. Package whichever the port's setup expects.

### 2d. Run

```bash
NXRT_SETTINGS=/tmp/eowdata/eowrecomp.ini NXRT_DATA_ROOT=/tmp/eowdata \
  ./build/eowrecomp --play
```

Useful while bringing up:

```bash
NXRT_WINDOW=1280x800
NXRT_VK_TRACE=1
NXRT_SLOW_FRAME_MS=50
NXRT_HANG_DUMP=15
```

## 3. Definition of done for this stage

- `make` completes on the Deck.
- The start screen appears and the menu opens.
- One port boots past the title screen with Vulkan validation not screaming.
- `tests/run.sh` still passes.

Do not move to performance work before that.

## 4. After it builds

1. **Deck profile:** 800p, 60/90 Hz, FSR, frame cap, TDP, suspend/resume, Game Mode launch, controller/trackpad
   mapping.
2. **Pipeline cache:** build Vulkan pipelines for the Deck GPU/Mesa driver at setup; key the cache by driver, game
   build and translator. This is the main stutter lever.
3. **Shader precomputation:** `docs/SHADER_PRECOMP.md` has the full plan (updated for Vulkan). The EoW recording
   harness and tour checklist live in `games/eow/tools/` (`record_shader_tour.sh`, `SHADER_TOUR.md`) if that work was
   copied along.
4. **Benchmark against an emulator with a warm cache on the same Deck**, not a cold-cache one. Measure 1% lows, TDP,
   startup and suspend.

## 5. Traps

- This is a **Linux/C++** build. The `.mm` files under `ui/` and `runtime/` are compiled as C++ (`-x c++`); do not add
  Objective-C or Metal.
- `runtime/main.c` no longer has a Metal branch; `g_ui_backend_vulkan` must be defined or the link fails.
- `ui/backends/` is empty until the Vulkan backend is added; `nx.mk` expects no backend there.
- `runtime/gamebuild.c` can use zig or a system clang; the old Windows slim-zig packaging is gone.
- The branch is intentionally non-building; do not merge it into `master` until a Deck run works.
- Do not put LAS `recolour`/`pot`/`texpatch` code back into `runtime/`; move those features behind the generic
  `nx_game` hooks or into `games/las/`.

## 6. Files to read first

| File | Why |
|---|---|
| [`STEAM_DECK.md`](../STEAM_DECK.md) | the cut and the known gaps |
| [`runtime/nvn/vulkan/vk.h`](../runtime/nvn/vulkan/vk.h) | Vulkan device/window/frame API |
| [`runtime/nvn/vulkan/device.c`](../runtime/nvn/vulkan/device.c) | `vk_frame_begin`, `vk_frame_end`, `vk_frame_cmd` |
| [`runtime/nvn/vulkan/imgui_glue.cpp`](../runtime/nvn/vulkan/imgui_glue.cpp) | ImGui Vulkan init/texture helpers |
| [`ui/nx_ui.h`](../ui/nx_ui.h) | `NxUiBackend` contract |
| [`runtime/main.c`](../runtime/main.c) | where the backend is registered |
| [`docs/SHADER_PRECOMP.md`](SHADER_PRECOMP.md) | shader/pipeline precomputation plan |
