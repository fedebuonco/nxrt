// nx-ui's drawing backend on Vulkan (Steam Deck / SteamOS): the start screen's frames into the window's swapchain,
// and the in-game menu into the frame the renderer already began (device.c vk_frame_begin / vk_frame_cmd).
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_vulkan.h"
extern "C" {
#include "nx_ui.h"
#include "vk.h"
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
bool vk_frame_begin(const float clear[4]);
void *vk_frame_cmd(void);
void vk_frame_end(const char *shot_path);
}

static SDL_Window *ui_window(void) { return vk_ui_window(); }
static bool vk_ui_init(void) { return vk_imgui_backend_init(); }
static void vk_ui_shutdown(void) { vk_imgui_backend_shutdown(); }
static void vk_ui_new_frame(void *) { ImGui_ImplVulkan_NewFrame(); }
static void vk_ui_render(void *frame) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)frame);
}
static void *vk_ui_frame_begin(const float clear[4]) {
    if (!vk_frame_begin(clear)) return NULL;
    return vk_frame_cmd();
}
static void vk_ui_frame_end(void *, const char *shot_path) { vk_frame_end(shot_path); }
static uint64_t vk_ui_texture_rgba(const uint8_t *rgba, int w, int h) {
    return vk_imgui_texture_rgba(rgba, w, h);
}
extern "C" const NxUiBackend g_ui_backend_vulkan = {ui_window, vk_ui_init, vk_ui_shutdown, vk_ui_new_frame, vk_ui_render,
                                         vk_ui_frame_begin, vk_ui_frame_end, vk_ui_texture_rgba};
