// nx-ui's drawing backend on Metal (backends/metal.mm), for any host with an SDL window and a CAMetalLayer: set it
// up once the window exists, then nxui_set_backend(&g_ui_backend_metal). The host's own frames pass a MtlUiFrame to
// menu_render (the in-game overlay, drawn over its image).
#pragma once
#include "nx_ui.h"

typedef struct MtlUiFrame {
    void *cb;      // id<MTLCommandBuffer>
    void *target;  // id<MTLTexture>: the drawable's texture
    void *rp;      // (the backend's: NULL when passed in)
} MtlUiFrame;

#ifdef __cplusplus
extern "C" {
#endif
void nxui_metal_setup(SDL_Window *window, void *layer /* CAMetalLayer */, void *device /* id<MTLDevice> */);
extern const NxUiBackend g_ui_backend_metal;
#ifdef __cplusplus
}
#endif
