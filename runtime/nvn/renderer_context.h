// The renderer's explicit state (renderer_context.m): the device, the current command buffer/encoder, the clear
// pipelines and the pipeline cache. renderer.m accesses it through short macros while the modules are extracted.
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
#include "renderer_state.h"

typedef struct RendererContext {
    id<MTLDevice> dev;
    id<MTLCommandBuffer> cmdbuf;
    id<MTLRenderCommandEncoder> enc;
    id<MTLRenderPipelineState> clear_pipes[64];
    NSMutableDictionary<NSString *, id> *cache;
    RendererState st;
    int passes_in_cb;
    float pass_sx, pass_sy;
    int pass_rt_w, pass_rt_h;
    float vp_sx, vp_sy, vp_ox, vp_oy;
    RtState pass_rt;
    bool opening_pass;
    float tex_sc[2][64][2];
    float tex_fr[2][64][2];
    uint32_t tex_wh[2][64][2];
    bool draw_watched;
} RendererContext;

RendererContext *renderer_context(void);
void renderer_context_init(id<MTLDevice> dev);
void renderer_context_release(void);  // (tests / a headless renderer: drop the retained Metal objects)
