// Dear ImGui's Vulkan backend on our device (start screen and in-game menu; both draw inside the window image's
// dynamic rendering begun by vk_frame_begin).
#ifdef _WIN32
#include "../../../third_party/reshade/include/reshade.hpp"
#endif
#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "vk.h"
#include "../../settings.h"
}

#ifdef _WIN32
static void reshade_sync(reshade::api::effect_runtime *runtime) {
    if (runtime->get_effects_state() != g_settings.reshade) {
        runtime->set_effects_state(g_settings.reshade);
        LOG("[reshade] effects %s\n", g_settings.reshade ? "on" : "off");
    }
}
#endif

extern "C" void vk_reshade_init(void) {
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"ReShade64.dll");
    if (!module) return;
    if (!reshade::register_addon(GetModuleHandleW(NULL), module)) {
        LOG("[reshade] effect control unavailable; changes require closing and reopening the application\n");
        return;
    }
    reshade::register_event<reshade::addon_event::init_effect_runtime>(reshade_sync);
    reshade::register_event<reshade::addon_event::reshade_present>(reshade_sync);
    LOG("[reshade] menu effect control ready\n");
#endif
}

// Per ImGui context (the start screen and the in-game menu each have one, and the start screen comes back after an
// in-place reboot while the menu's context lives on): the backend keeps its data in the current context.
extern "C" bool vk_imgui_backend_init(void) {
    if (ImGui::GetIO().BackendRendererUserData) return true;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_vk.inst;
    ii.PhysicalDevice = g_vk.phys;
    ii.Device = g_vk.dev;
    ii.QueueFamily = g_vk.qfam;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = 2;
    ii.ImageCount = (g_vk.sc_count > 3 ? g_vk.sc_count : 3) + 1;  // vertex buffers rotate per image: cover frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineCache = g_vk.pcache;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk.sc_format;
    bool ok = ImGui_ImplVulkan_Init(&ii);
    if (!ok) LOG("[vk] ImGui Vulkan backend init failed\n");
    return ok;
}

extern "C" void vk_imgui_backend_shutdown(void) {
    if (!ImGui::GetIO().BackendRendererUserData) return;
    vkDeviceWaitIdle(g_vk.dev);
    ImGui_ImplVulkan_Shutdown();
}

// A static RGBA8 image for ImGui (start screen background). Returns the ImTextureID.
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return 0;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSpan s = vk_transient_copy(rgba, (uint64_t)w * h * 4, 16);
    VkBufferImageCopy rg = {};
    rg.bufferOffset = s.off;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &view) != VK_SUCCESS) return 0;
    return (uint64_t)ImGui_ImplVulkan_AddTexture(vk_default_sampler(true), view, VK_IMAGE_LAYOUT_GENERAL);
}

// ------------------------------------------------------------------ nx-ui's drawing backend (nx_ui.h NxUiBackend)
// frame: the command buffer of the window image being rendered (the presenter's, device.c, or a start screen frame's)
extern "C" {
#include "nx_ui.h"
#include "platform.h"
#include <SDL3/SDL.h>
SDL_Window *ui_window(void);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
}

static void vkui_new_frame(void *) { ImGui_ImplVulkan_NewFrame(); }
static void vkui_render(void *frame) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)frame); }

static void *vkui_frame_begin(const float clear[4]) {
    // one frame per refresh of the window's display: the swapchain presents without waiting (mailbox, for the
    // game's own pacing: device.c), so the start screen alone would draw as fast as the GPU goes
    static uint64_t next_frame;
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(ui_window()));
    uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
    bool on_time = next_frame > now && next_frame - now <= period;  // (else late, or the rate changed: start over)
    if (on_time) plat_sleep_until_ns(next_frame);
    next_frame = (on_time ? next_frame : now) + period;
    return vk_frame_begin(clear) ? vk_frame_cmd() : NULL;
}

static void vkui_frame_end(void *, const char *shot_path) { vk_frame_end(shot_path); }
static uint64_t vkui_texture_rgba(const uint8_t *rgba, int w, int h) { return vk_imgui_texture_rgba(rgba, w, h); }

extern "C" const NxUiBackend g_ui_backend_vulkan = {ui_window, vk_imgui_backend_init, vk_imgui_backend_shutdown,
                                                    vkui_new_frame, vkui_render, vkui_frame_begin, vkui_frame_end,
                                                    vkui_texture_rgba};
