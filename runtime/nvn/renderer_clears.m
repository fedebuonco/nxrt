// Clear path: full-screen quads and recorded clears (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "renderer.h"
#include "renderer_cache.h"
#include "renderer_clears.h"
#include "renderer_context.h"
#include "renderer_pass.h"
#include "renderer_textures.h"
#include "shader_rec_norm.h"
#include "rt.h"

#define st (renderer_context()->st)
#define enc (renderer_context()->enc)
#define cache (renderer_context()->cache)
#define dev (renderer_context()->dev)
#define vp_sx (renderer_context()->vp_sx)
#define vp_sy (renderer_context()->vp_sy)
#define vp_ox (renderer_context()->vp_ox)
#define vp_oy (renderer_context()->vp_oy)
#define pass_sx (renderer_context()->pass_sx)
#define pass_sy (renderer_context()->pass_sy)
#define TIMED_COMPILE(expr) (expr)
extern bool g_debug_frame;
void renderer_td_record(NvnTexture *t, bool is_color, int kind, const void *color, bool depth, float dval, bool stencil, uint32_t sval);
int renderer_debug_pass_no(void);

// ------------------------------------------------------------------ clears (full-screen triangle)
static const char *clear_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 &d [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, d.x, 1); return o; }\n"
    "struct OF { float4 c [[color(INDEX)]]; };\n"
    "fragment OF fs_f(V in [[stage_in]], constant float4 &c [[buffer(0)]]) { OF o; o.c = c; return o; }\n"
    "struct OI { int4 c [[color(INDEX)]]; };\n"
    "fragment OI fs_i(V in [[stage_in]], constant int4 &c [[buffer(0)]]) { OI o; o.c = c; return o; }\n"
    "struct OU { uint4 c [[color(INDEX)]]; };\n"
    "fragment OU fs_u(V in [[stage_in]], constant uint4 &c [[buffer(0)]]) { OU o; o.c = c; return o; }\n"
    "fragment void fs_none(V in [[stage_in]]) {}\n";

static id<MTLLibrary> clear_lib(int index) {
    static id<MTLLibrary> libs[8];
    if (index < 0 || index > 7) {  // (a target index past the 8 NVN has: the shader's INDEX only names its destination)
        static int logged;
        if (!logged) logged = 1, LOG("[metal] clear target index %d (clamped to 0)\n", index);
        index = 0;
    }
    if (!libs[index]) {
        NSString *src = [@(clear_src) stringByReplacingOccurrencesOfString:@"INDEX" withString:[NSString stringWithFormat:@"%d", index]];
        NSError *err;
        libs[index] = TIMED_COMPILE([dev newLibraryWithSource:src options:nil error:&err]);
        if (!libs[index]) FATAL("clear shader: %s\n", err.localizedDescription.UTF8String);
    }
    return libs[index];
}

void renderer_attachment_formats(MTLRenderPipelineDescriptor *pd) {
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) pd.colorAttachments[i].pixelFormat = renderer_view_texture(st.rt.color[i], &st.rt.cview[i], st.rt.has_cview[i], true, nil).pixelFormat;
    if (st.rt.depth) {
        MTLPixelFormat pf = renderer_pixel_format(st.rt.depth->format);
        if (renderer_format_is_depth(st.rt.depth->format) && st.rt.depth->format != 0x31) pd.depthAttachmentPixelFormat = pf;
        if (renderer_format_has_stencil(st.rt.depth->format)) pd.stencilAttachmentPixelFormat = pf;
    }
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && st.rt.color[i]->samples > 1) pd.rasterSampleCount = st.rt.color[i]->samples;
}

// (shader preparation) attachment_formats from a variant record: the formats the draw's targets would have
void renderer_attachment_formats_rec(MTLRenderPipelineDescriptor *pd, const ShaderRec *r) {
    bool any = false;
    for (int i = 0; i < r->ncolor && i < 8; i++)
        if (r->color[i]) { pd.colorAttachments[i].pixelFormat = renderer_pixel_format(r->cview[i] ? r->cview[i] : r->color[i]); any = true; }
    if (r->depth) {
        MTLPixelFormat pf = renderer_pixel_format(r->depth);
        if (renderer_format_is_depth(r->depth) && r->depth != 0x31) pd.depthAttachmentPixelFormat = pf;
        if (renderer_format_has_stencil(r->depth)) pd.stencilAttachmentPixelFormat = pf;
    }
    if (any && r->samples > 1) pd.rasterSampleCount = r->samples;
}

// A full clear (whole target, every channel) while no pass is open on the current targets: recorded on the texture for
// its next pass (see TexDefer). Partial clears, view clears and clears into an open pass stay quads.
static bool record_clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil,
                         uint32_t sval, uint32_t smask) {
    static int on = -1;
    if (on < 0) { const char *e = ENV("NXRT_MTL_LOAD_CLEAR"); on = !e || atoi(e); }
    if (!on || (enc && !st.rt_dirty)) return false;
    NvnTexture *t = index >= 0 ? (index < st.rt.ncolor ? st.rt.color[index] : NULL) : st.rt.depth;
    if (!t) return false;
    const int32_t *sc = st.scissor[0];
    if (sc[0] != 0 || sc[1] != 0 || sc[2] < (int32_t)t->width || sc[3] < (int32_t)t->height) return false;
    if (index >= 0 ? mask != 0xF : (!depth && !stencil) || (stencil && smask != 0xFF)) return false;
    // (a view of one level or layer: the quad clears just that; a format view covers the whole texture)
    const NvnTextureView *v = index >= 0 ? (st.rt.has_cview[index] ? &st.rt.cview[index] : NULL) : (st.rt.has_dview ? &st.rt.dview : NULL);
    if (v && (((v->setmask & 1) && v->base_level) || ((v->setmask & 2) && v->min_layer))) return false;
    renderer_td_record(t, index >= 0, kind, color, depth, dval, stencil, sval);
    if (g_debug_frame) LOG("[clear] recorded idx %d depth %d (%g) stencil %d on texture %p @%llx\n", index, depth, dval, stencil, (void *)t,
                           (unsigned long long)(t ? t->addr : 0));
    return true;
}

void renderer_clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil,
                  uint32_t sval, uint32_t smask) {
    if (record_clear(index, kind, color, mask, depth, dval, stencil, sval, smask)) return;
    renderer_begin_pass();
    if (g_debug_frame) {
        int pw, ph;
        renderer_pass_size(&pw, &ph);
        LOG("[clear] pass %d idx %d depth %d (%g) stencil %d | pass %dx%d scissor %d,%d %dx%d\n", renderer_debug_pass_no(), index, depth,
            dval, stencil, pw, ph, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    id<MTLLibrary> lib = clear_lib(index < 0 ? 0 : index);
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    static NSString *const fs[] = {@"fs_f", @"fs_i", @"fs_u"};
    if (kind < 0 || kind > 2) {  // (a clear whose colour kind the recorder didn't set: float)
        static int logged_kind;
        if (!logged_kind) logged_kind = 1, LOG("[metal] clear kind %d idx %d (using float)\n", kind, index);
        kind = 0;
    }
    pd.fragmentFunction = [lib newFunctionWithName:index >= 0 ? fs[kind] : @"fs_none"];
    renderer_attachment_formats(pd);
    for (int i = 0; i < 8; i++) pd.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
    if (index >= 0) {
        MTLColorWriteMask m = 0;
        if (mask & 1) m |= MTLColorWriteMaskRed;
        if (mask & 2) m |= MTLColorWriteMaskGreen;
        if (mask & 4) m |= MTLColorWriteMaskBlue;
        if (mask & 8) m |= MTLColorWriteMaskAlpha;
        pd.colorAttachments[index].writeMask = m;
    }
    NSError *err;
    NSString *key = [NSString stringWithFormat:@"clear %d %d %u %d %d %lu %lu %lu", index, kind, mask, depth, stencil,
                     (unsigned long)pd.colorAttachments[index < 0 ? 0 : index].pixelFormat,
                     (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat];
    id<MTLRenderPipelineState> pipe = renderer_cache_get(key);
    if (!pipe) {
        pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&err]);
        if (!pipe) { LOG("[metal] clear pipeline: %s\n", err.localizedDescription.UTF8String); return; }
        renderer_cache_set(key, pipe);
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = depth;
    if (stencil) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.writeMask = smask;
        dd.frontFaceStencil = sd;
        dd.backFaceStencil = sd;
    }
    [enc setRenderPipelineState:pipe];
    [enc setDepthStencilState:[dev newDepthStencilStateWithDescriptor:dd]];
    [enc setStencilReferenceValue:sval];
    [enc setCullMode:MTLCullModeNone];
    int w, h;
    renderer_pass_size(&w, &h);
    [enc setViewport:(MTLViewport){0, 0, w, h, 0, 1}];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    renderer_apply_scissor();
    float d4[4] = {dval, 0, 0, 0};
    [enc setVertexBytes:d4 length:16 atIndex:0];
    if (index >= 0) [enc setFragmentBytes:color length:16 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

