#import <Foundation/Foundation.h>

#include "renderer_context.h"

static RendererContext g_ctx;

RendererContext *renderer_context(void) { return &g_ctx; }

void renderer_context_init(id<MTLDevice> dev) {
    g_ctx.dev = dev;
    g_ctx.pass_sx = g_ctx.pass_sy = 1;
    g_ctx.vp_sx = g_ctx.vp_sy = 1;
    if (!g_ctx.cache) g_ctx.cache = [NSMutableDictionary new];  // (kept: shader preparation may have run on the start screen)
}

void renderer_context_release(void) {
    g_ctx.cmdbuf = nil;
    g_ctx.enc = nil;
    g_ctx.dev = nil;
    g_ctx.cache = nil;
    for (int i = 0; i < 64; i++) g_ctx.clear_pipes[i] = nil;
}
