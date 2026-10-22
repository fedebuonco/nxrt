// A minimal host on the nx layers (no nx-runtime): an SDL window drawn with Metal, the nx-ui start screen and menu,
// nx-input's pad shown live, settings in demo.ini, saves resolved by nx-save. What a runtime other than nxrt (its own
// CPU, kernel and renderer) wires up to get the same menu, controls and save handling.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "menu.h"
#include "nx_app.h"
#include "nx_input.h"
#include "nx_save.h"
#include "nx_settings.h"
#include "nx_ui.h"
#include "nx_ui_metal.h"
#include "paths.h"
#include "platform.h"
}

// ---- the host's own setting (a section of the same ini)
static bool g_demo_flag = true;
static bool demo_key(const char *k, const char *v) {
    if (strcmp(k, "demo_flag")) return false;
    g_demo_flag = atoi(v) != 0;
    return true;
}
static void demo_write(FILE *f) { fprintf(f, "demo_flag = %d\n", g_demo_flag); }

// ---- a page of its own, and an overlay
static bool demo_tab(float scale) {
    opt_begin("##demo", scale);
    opt_section("Host");
    opt_toggle("Demo flag", &g_demo_flag, true, "A setting of the host's own, saved in the same file as nx-ui's.");
    char save[300];
    snprintf(save, sizeof save, "Saves go to %s/%s.", rt_data_root(), nx_save_dir());
    opt_action("Save folder", nx_save_dir(), save, false);
    opt_end();
    return false;
}
static NxPad g_pad;
static bool overlay_wanted(void) { return true; }
static void overlay_draw(float w, float, void *font) {
    char t[160];
    snprintf(t, sizeof t, "pad %04llx  L %+6d %+6d  R %+6d %+6d", (unsigned long long)g_pad.buttons, g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry);
    ImGui::GetForegroundDrawList()->AddText((ImFont *)font, 28, ImVec2(w * 0.04f, 30), IM_COL32(255, 255, 255, 255), t);
}

// ---- the start screen: no files to set up
static void files_status(char *out, size_t cap) { snprintf(out, cap, "%s", ""); }

int main(void) {
    NxAppInfo app = {"uihost", "uihost", "nx-ui host", "org.example.uihost", "uihost", "demo.ini", "v0.1.0"};
    nx_app_init(&app);
    plat_init();
    rt_paths_init();
    static const NxSettingsSection demo = {"demo", NULL, demo_key, NULL, demo_write};
    nxin_init();
    nxui_init();
    nx_save_init();
    nx_settings_register(&demo);
    settings_load();
    fprintf(stderr, "[host] saves: %s, backup: %d files\n", nx_save_dir(), nx_save_backup());

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return 1;
    SDL_WindowFlags fl = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("NXRT_HIDDEN")) fl |= SDL_WINDOW_HIDDEN;
    SDL_Window *win = SDL_CreateWindow(nx_app.title, 1280, 720, fl);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(SDL_Metal_CreateView(win));
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    layer.device = dev;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    nxui_metal_setup(win, (__bridge void *)layer, (__bridge void *)dev);
    nxui_set_backend(&g_ui_backend_metal);

    static const NxUiTab tab = {"Host", 50, 10, false, false, demo_tab};
    nxui_add_tab(&tab);
    nxui_add_overlay(overlay_wanted, overlay_draw);
    NxUiGameFiles files = {};
    files.status = files_status;
    files.ready_note = "No game: the nx layers on their own";
    nxui_set_game_files(&files);
    NxUiStartInfo si = {"nx-ui host", NULL, NULL};
    nxui_set_start_info(&si);

    launcher_run();  // until PLAY

    // the host's own frames: a colour from the pad, the menu (F1) and the overlay on top
    id<MTLCommandQueue> q = [dev newCommandQueue];
    long frames = getenv("NXRT_HOST_FRAMES") ? atol(getenv("NXRT_HOST_FRAMES")) : -1;
    for (uint64_t frame = 0; frames < 0 || (long)frame < frames; frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { settings_save(); return 0; }
            if (!menu_handle_event(&e)) input_handle_event(&e);
        }
        menu_apply_window_requests(win);
        nxin_frame(frame, NULL);
        nxin_read(&g_pad, menu_input_captured());
        nxin_dpad_walk(&g_pad, menu_input_captured(), false);
        nxin_stick_buttons(&g_pad);
        @autoreleasepool {
            int w, h;
            SDL_GetWindowSizeInPixels(win, &w, &h);
            layer.drawableSize = CGSizeMake(w, h);
            id<CAMetalDrawable> d = [layer nextDrawable];
            if (!d) continue;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = d.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(g_pad.buttons & 1 ? 0.5 : 0.1, 0.12, 0.2, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            MtlUiFrame uf = {(__bridge void *)cb, (__bridge void *)d.texture, NULL};
            menu_render(&uf, w, h);
            [cb presentDrawable:d];
            [cb commit];
        }
    }
    settings_save();
    fprintf(stderr, "[host] ran %ld frames\n", frames);
    return 0;
}
