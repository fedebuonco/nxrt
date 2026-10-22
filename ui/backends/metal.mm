// nx-ui's drawing backend on Metal (nx_ui.h NxUiBackend, nx_ui_metal.h): the start screen's frames into the window's
// layer, and the menu drawn into the host's own frame (the runtime: metal.m backend_present passes a MtlUiFrame).
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <vector>

#include "imgui.h"
#include "imgui_impl_metal.h"

extern "C" {
#include "nx_ui.h"
#include "nx_ui_metal.h"
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);
}

static SDL_Window *g_window;
static void *g_layer, *g_device;
void nxui_metal_setup(SDL_Window *window, void *layer, void *device) { g_window = window, g_layer = layer, g_device = device; }
static SDL_Window *ui_window(void) { return g_window; }
static void *ui_metal_layer(void) { return g_layer; }
static void *ui_metal_device(void) { return g_device; }

// (test) BGRA texture -> PNG (the game renderer's saver isn't up yet before the game boots)
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL when the file can't be created, e.g. its folder isn't there: CFRelease(NULL) traps)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    } else fprintf(stderr, "[ui] can't write %s\n", path);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}

static id<MTLDevice> device(void) { return (__bridge id<MTLDevice>)ui_metal_device(); }

static bool mtl_init(void) { return ImGui_ImplMetal_Init(device()); }
static void mtl_shutdown(void) { ImGui_ImplMetal_Shutdown(); }

static void mtl_new_frame(void *frame) {
    MtlUiFrame *f = (MtlUiFrame *)frame;
    if (!f->rp) {  // (the presenter's frame: drawn over the game image)
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = (__bridge id<MTLTexture>)f->target;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        f->rp = (__bridge_retained void *)rp;
    }
    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor *)f->rp);
}

static void mtl_render(void *frame) {
    MtlUiFrame *f = (MtlUiFrame *)frame;
    MTLRenderPassDescriptor *rp = (__bridge_transfer MTLRenderPassDescriptor *)f->rp;
    f->rp = NULL;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)f->cb;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
}

// ---- the start screen's own frames
static MtlUiFrame g_frame;
static void *g_pool;
static id<MTLCommandQueue> g_queue;
static id<CAMetalDrawable> g_drawable;

static void *mtl_frame_begin(const float clear[4]) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    if (!g_queue) {
        g_queue = [device() newCommandQueue];
        if (getenv("NXRT_LAUNCHER_SHOT")) layer.framebufferOnly = NO;  // (the shot copies the drawable)
    }
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    layer.drawableSize = CGSizeMake(w, h);
    g_pool = objc_autoreleasePoolPush();
    g_drawable = [layer nextDrawable];
    if (!g_drawable) { objc_autoreleasePoolPop(g_pool); return NULL; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_frame.cb = (__bridge_retained void *)[g_queue commandBuffer];
    g_frame.target = (__bridge void *)g_drawable.texture;
    g_frame.rp = (__bridge_retained void *)rp;
    return &g_frame;
}

static void mtl_frame_end(void *frame, const char *shot_path) {
    MtlUiFrame *f = (MtlUiFrame *)frame;
    id<MTLCommandBuffer> cb = (__bridge_transfer id<MTLCommandBuffer>)f->cb;
    id<MTLTexture> shot = nil;
    if (shot_path) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_drawable.texture.pixelFormat
                                     width:g_drawable.texture.width height:g_drawable.texture.height mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        shot = [device() newTextureWithDescriptor:td];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:g_drawable.texture toTexture:shot];
        [bl endEncoding];
    }
    [cb presentDrawable:g_drawable];
    [cb commit];
    if (shot) {
        [cb waitUntilCompleted];
        save_png(shot, shot_path);
    }
    g_drawable = nil;
    objc_autoreleasePoolPop(g_pool);
}

static uint64_t mtl_texture_rgba(const uint8_t *rgba, int w, int h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    id<MTLTexture> t = [device() newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    return (uint64_t)(uintptr_t)(__bridge_retained void *)t;  // (kept: a background lives as long as the start screen)
}

const NxUiBackend g_ui_backend_metal = {ui_window, mtl_init, mtl_shutdown, mtl_new_frame, mtl_render,
                                        mtl_frame_begin, mtl_frame_end, mtl_texture_rgba};
