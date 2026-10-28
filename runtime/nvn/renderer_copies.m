// Texture copies (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "renderer.h"
#include "renderer_cache.h"
#include "renderer_context.h"
#include "renderer_formats.h"
#include "renderer_pass.h"
#include "renderer_textures.h"
#include "renderer_copies.h"
#include "rt.h"

#define st (renderer_context()->st)
#define dev (renderer_context()->dev)
#define enc (renderer_context()->enc)
#define cmdbuf (renderer_context()->cmdbuf)
#define TIMED_COMPILE(expr) (expr)
id<MTLCommandBuffer> renderer_command_buffer(void);
void renderer_stat_pass_add(void);
id<MTLTexture> renderer_sampled_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_type);
extern bool g_debug_frame;
void renderer_snapshots_invalidate(const char *why);
void renderer_flush(void);
void renderer_mark_gpu_owned(NvnTexture *t);
bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);

// ------------------------------------------------------------------ texture copies


static const char *copy_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);\n"
    "  o.uv = r.xy + p * r.zw; return o; }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, in.uv); }\n"
    "struct D { float d [[depth(any)]]; };\n"
    "fragment D fs_depth(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  D o; o.d = t.sample(s, in.uv); return o; }\n";

// Bytes per block of the Metal texture backing an NVN format (RGB8 is expanded to RGBA8 on the host).
static uint64_t renderer_pixel_format_bytes(MTLPixelFormat pf, uint64_t nvn_bpb) {
    (void)pf;
    return nvn_bpb == 3 ? 4 : nvn_bpb;
}

// Linear guest memory <-> texture region. Row/image strides of 0 mean tightly packed.
static void copy_region_dims(NvnTexture *t, const RendererTexRef *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

void renderer_copy_buffer_to_texture(uint64_t src, const RendererTexRef *dst) {
    renderer_snapshots_invalidate("buf2tex");
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    renderer_td_flush(t);  // (a recorded clear happens before the upload)
    renderer_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    static int nlog;
    if (nlog++ < 20) LOG("[copy] buf %llx -> tex %ux%u f%#x region %d,%d,%d %dx%dx%d row %llu lvl %d\n", src, t->width, t->height, t->format, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], row, level);
    id<MTLBlitCommandEncoder> b = [renderer_command_buffer() blitCommandEncoder];
    id<MTLTexture> tex = renderer_texture(t, b, false);  // make sure prior CPU contents are uploaded first
    renderer_mark_gpu_owned(t);
    bool array = tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint64_t mtl_bpb = renderer_pixel_format_bytes(tex.pixelFormat, fi->bpb);
    for (int z = 0; z < (array ? rg[5] : 1); z++) {
        const uint8_t *s = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img);
        uint64_t out_row = (uint64_t)wb * mtl_bpb;
        uint8_t *data = malloc(out_row * hb * (array ? 1 : rg[5]));
        for (int d = 0; d < (array ? 1 : rg[5]); d++)
            for (uint32_t y = 0; y < hb; y++) {
                const uint8_t *sr = s + (uint64_t)d * img + (uint64_t)y * row;
                uint8_t *dr = data + ((uint64_t)d * hb + y) * out_row;
                if (mtl_bpb == fi->bpb) memcpy(dr, sr, out_row);
                else if (fi->bpb == 3 && mtl_bpb == 4)
                    for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
                else memset(dr, 0, out_row);
            }
        id<MTLBuffer> staging = [dev newBufferWithBytes:data length:out_row * hb * (array ? 1 : rg[5])
                                                options:MTLResourceStorageModeShared];
        free(data);
        [b copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:out_row sourceBytesPerImage:out_row * hb
               sourceSize:MTLSizeMake(rg[3], rg[4], array ? 1 : rg[5]) toTexture:tex
         destinationSlice:array ? rg[2] + z : 0 destinationLevel:level
        destinationOrigin:MTLOriginMake(rg[0], rg[1], array ? 0 : rg[2])];
    }
    [b endEncoding];
}

// Color copy with scaling: draw src rect into dst rect (physical texels).
static id<MTLTexture> copy_raw_view(id<MTLTexture> t) {
    MTLPixelFormat pf = t.pixelFormat;
    MTLPixelFormat raw = pf == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm
                       : pf == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : pf;
    if (raw == pf) return t;
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:raw];
    return v ? v : t;
}

static void draw_copy(id<MTLTexture> s, int sx0, int sy0, int sw, int sh, id<MTLTexture> d, int dx0, int dy0, int dw, int dh,
                      bool linear) {
    static int raw = -1;  // NXRT_COPY_RAW_SRGB: copies move values unchanged (no sRGB decode/encode on either side)
    if (raw < 0) raw = getenv("NXRT_COPY_RAW_SRGB") != NULL;
    if (raw) { s = copy_raw_view(s); d = copy_raw_view(d); }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [renderer_command_buffer() renderCommandEncoderWithDescriptor:rp];
    NSString *key = [NSString stringWithFormat:@"copy %lu", (unsigned long)d.pixelFormat];
    id<MTLRenderPipelineState> pipe = renderer_cache_get(key);
    if (!pipe) {
        static id<MTLLibrary> lib;
        if (!lib) lib = TIMED_COMPILE([dev newLibraryWithSource:@(copy_src) options:nil error:nil]);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:nil]);
        renderer_cache_set(key, pipe);
    }
    static id<MTLSamplerState> lin, near;
    if (!lin) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        near = [dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        lin = [dev newSamplerStateWithDescriptor:sd];
    }
    float r[4] = {(float)sx0 / s.width, (float)sy0 / s.height, (float)sw / s.width, (float)sh / s.height};
    [e setRenderPipelineState:pipe];
    [e setViewport:(MTLViewport){dx0, dy0, dw, dh, 0, 1}];
    [e setVertexBytes:r length:16 atIndex:0];
    [e setFragmentTexture:s atIndex:0];
    [e setFragmentSamplerState:linear ? lin : near atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    renderer_stat_pass_add();
}

void renderer_copy_texture_to_buffer(const RendererTexRef *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    renderer_td_flush(t);
    renderer_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    {
        static int nlog;
        if (nlog++ < 12 || getenv("NXRT_LOG_READBACK"))
            LOG("[readback] tex %ux%u f%#x level %d region %d,%d %dx%d -> %llx row %llu\n", t->width, t->height,
                t->format, level, rg[0], rg[1], rg[3], rg[4], dst, row);
    }
    id<MTLTexture> tex = renderer_texture(t, nil, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat width:t->width
                                                                                      height:t->height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> small = [dev newTextureWithDescriptor:td];
        draw_copy(tex, 0, 0, (int)lroundf(t->width * tsx), (int)lroundf(t->height * tsy), small, 0, 0, t->width, t->height, true);
        tex = small;
    }
    id<MTLBlitCommandEncoder> b = [renderer_command_buffer() blitCommandEncoder];
    if (renderer_pixel_format_bytes(tex.pixelFormat, fi->bpb) != fi->bpb) { [b endEncoding]; return; }
    bool array = tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube;
    for (int z = 0; z < (array ? rg[5] : 1); z++)
        [b copyFromTexture:tex sourceSlice:array ? rg[2] + z : 0 sourceLevel:level
              sourceOrigin:MTLOriginMake(rg[0], rg[1], array ? 0 : rg[2])
                sourceSize:MTLSizeMake(rg[3], rg[4], array ? 1 : rg[5]) toBuffer:g_guest_buffer
         destinationOffset:dst - HEAP_BASE + (uint64_t)z * img destinationBytesPerRow:row destinationBytesPerImage:img];
    [b endEncoding];
    renderer_flush();  // the CPU may read the result right after the fence
}

void renderer_copy_texture(const RendererTexRef *src, const RendererTexRef *dst, uint64_t flags) {
    renderer_snapshots_invalidate("tex2tex");
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    renderer_td_flush(st_); renderer_td_flush(dt);  // (recorded clears happen before the copy)
    renderer_end_pass();
    id<MTLTexture> s = renderer_sampled_texture(st_, &src->view, src->has_view, TEXTYPE_2D);
    renderer_mark_gpu_owned(dt);
    id<MTLTexture> d = renderer_view_texture(dt, &dst->view, dst->has_view, true, nil);
    // a full copy into a swapchain image: the source is the frame's final framebuffer (UI is drawn there)
    extern void renderer_texture_mark_presented(NvnTexture *t);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height)
        renderer_texture_mark_presented(st_);
    extern bool renderer_texture_first_write(NvnTexture *t);
    renderer_texture_first_write(dt);  // a copy fills the image: no clear afterwards this frame
    extern bool renderer_texture_level_fresh(NvnTexture *t);
    renderer_texture_level_fresh(dt);  // nor a level-change clear
    const int32_t *sr = src->region, *dr = dst->region;
    if (g_debug_frame) {
        static int nl;
        uint64_t total = tegra_total_size(&st_->layout), nz = 0;
        for (uint64_t k = 0; k < total && st_->addr; k++) nz += ((const uint8_t *)(uintptr_t)st_->addr)[k] != 0;
        if (nl < 400) LOG("[copytex] src addr %llx size %llu nonzero %llu tflags %#x stride %u bh %u\n", st_->addr, total, nz, st_->flags, st_->stride, st_->layout.block_height_log2);
        if (nl++ < 400) LOG("[copytex] %ux%u f%#x (%d,%d %dx%d) -> %ux%u f%#x (%d,%d %dx%d) flags %llx src_gpu %d\n", st_->width, st_->height, st_->format,
            sr[0], sr[1], sr[3], sr[4], dt->width, dt->height, dt->format, dr[0], dr[1], dr[3], dr[4], flags, renderer_is_gpu_owned(st_));
    }
    // regions are in guest texels; scaled allocations (output resolution) need physical texels
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)lroundf(st_->width * ssx), sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)lroundf(st_->height * ssy);
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)lroundf(dt->width * dsx), dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)lroundf(dt->height * dsy);
    static int no_depth_scale = -1;
    if (no_depth_scale < 0) no_depth_scale = getenv("NXRT_NO_DEPTH_SCALE") != NULL;
    if (!no_depth_scale && renderer_format_is_depth(dt->format) && (sw != dw || sh != dh) && s.sampleCount == 1 && dt->format != 0x31) {
        // scaled depth copy (the game builds its depth pyramid this way): draw, writing fragment depth
        MTLRenderPassDescriptor *drp = [MTLRenderPassDescriptor renderPassDescriptor];
        drp.depthAttachment.texture = d;
        drp.depthAttachment.loadAction = MTLLoadActionLoad;
        drp.depthAttachment.storeAction = MTLStoreActionStore;
        if (renderer_format_has_stencil(dt->format)) {
            drp.stencilAttachment.texture = d;
            drp.stencilAttachment.loadAction = MTLLoadActionLoad;
            drp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> e = [renderer_command_buffer() renderCommandEncoderWithDescriptor:drp];
        NSString *key = [NSString stringWithFormat:@"copydepth %lu", (unsigned long)d.pixelFormat];
        id<MTLRenderPipelineState> pipe = renderer_cache_get(key);
        if (!pipe) {
            static id<MTLLibrary> dlib;
            NSError *derr = nil;
            if (!dlib) dlib = TIMED_COMPILE([dev newLibraryWithSource:@(copy_src) options:nil error:&derr]);
            if (!dlib) LOG("[copy] depth copy library: %s\n", derr.localizedDescription.UTF8String);
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [dlib newFunctionWithName:@"vs"];
            pd.fragmentFunction = [dlib newFunctionWithName:@"fs_depth"];
            pd.depthAttachmentPixelFormat = d.pixelFormat;
            if (renderer_format_has_stencil(dt->format)) pd.stencilAttachmentPixelFormat = d.pixelFormat;
            NSError *perr = nil;
            pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&perr]);
            if (!pipe) { LOG("[copy] depth copy pipeline: %s\n", perr.localizedDescription.UTF8String); return; }
            renderer_cache_set(key, pipe);
        }
        static id<MTLDepthStencilState> always;
        if (!always) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            always = [dev newDepthStencilStateWithDescriptor:dd];
        }
        static id<MTLSamplerState> dnear;
        if (!dnear) dnear = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLTexture> sdepth = s;
        if (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || s.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8)
            sdepth = [s newTextureViewWithPixelFormat:s.pixelFormat];
        float r[4] = {(float)sx0 / s.width, (float)sy0 / s.height, (float)sw / s.width, (float)sh / s.height};
        [e setRenderPipelineState:pipe];
        [e setDepthStencilState:always];
        [e setViewport:(MTLViewport){dx0, dy0, dw, dh, 0, 1}];
        [e setVertexBytes:r length:16 atIndex:0];
        [e setFragmentTexture:sdepth atIndex:0];
        [e setFragmentSamplerState:dnear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        renderer_stat_pass_add();
        static int dbgc = -1;
        if (dbgc < 0) dbgc = getenv("NXRT_DEBUG_DEPTHCOPY") != NULL;
        if (dbgc) {
            static int nlog;
            if (nlog++ < 6) {
                id<MTLBuffer> rb = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> bb = [renderer_command_buffer() blitCommandEncoder];
                MTLBlitOption opt = (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                [bb copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(s.width / 2, s.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4 options:opt];
                opt = (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                [bb copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(d.width / 2, d.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:16 destinationBytesPerRow:4 destinationBytesPerImage:4 options:opt];
                [bb endEncoding];
                renderer_flush();
                const float *f = rb.contents;
                LOG("[copy] depth %lux%lu fmt %lu -> %lux%lu fmt %lu: centre src %g dst %g\n", (unsigned long)s.width, (unsigned long)s.height,
                    (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, f[0], f[4]);
            }
        }
        return;
    }
    if (renderer_format_is_depth(dt->format) || (s.pixelFormat == d.pixelFormat && sw == dw && sh == dh && s.sampleCount == d.sampleCount)) {
        id<MTLBlitCommandEncoder> b = [renderer_command_buffer() blitCommandEncoder];
        [b copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sx0, sy0, 0)
                sourceSize:MTLSizeMake(MIN(sw, dw), MIN(sh, dh), 1) toTexture:d destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(dx0, dy0, 0)];
        [b endEncoding];
        return;
    }
    draw_copy(s, sx0, sy0, sw, sh, d, dx0, dy0, dw, dh, flags & 1);
}

