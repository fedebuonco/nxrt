// Texture views and their cache (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>

#include "nvn.h"
#include "renderer.h"
#include "renderer_context.h"
#include "renderer_textures.h"
#include "rt.h"
#define dev (renderer_context()->dev)

// ------------------------------------------------------------------ textures with views
// Texture view cache: views are made per bind otherwise (an allocation per draw). Keyed by the base texture and
// the view parameters; a cached view retains its base, so a key's base pointer can't be reused while cached.
#define VIEW_TAB 16384
static struct { uint64_t key; void *base; void *view; } view_tab[VIEW_TAB];
static int g_views_live;

// Views keep their base texture alive: when the game frees a texture its views must go too (they piled up -
// render targets of a 32:9 window held long after the game dropped them - and a new texture at the same address
// could get a stale view). Entries are tombstoned (key ~0) so probe chains stay intact.
int renderer_views_live(void) { return g_views_live; }
void renderer_views_reset(void) {
    for (int i = 0; i < VIEW_TAB; i++) {
        if (view_tab[i].view) CFRelease(view_tab[i].view);
        view_tab[i].view = view_tab[i].base = NULL;
        view_tab[i].key = 0;
    }
    g_views_live = 0;
}
void renderer_views_forget(void *base) {
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].base == base && view_tab[i].view) {
            CFRelease(view_tab[i].view);
            view_tab[i].view = NULL;
            view_tab[i].base = NULL;
            view_tab[i].key = ~0ull;
            g_views_live--;
        }
}

id<MTLTexture> renderer_view_cached(id<MTLTexture> base, uint64_t params, id<MTLTexture> (^make)(void)) {
    uint64_t k = ((uint64_t)(uintptr_t)(__bridge void *)base * 0x9E3779B97F4A7C15ull) ^ (params * 0xC2B2AE3D27D4EB4Full);
    if (!k) k = 1;
    if (k == ~0ull) k = 2;
    // up to 8 probes; a miss is stored in the first free or tombstoned slot of those, else over the first one.
    // (It used to go to the slot past the 8 probes - never found again - and tombstones were never reused: on the
    // overworld, where textures stream in and out, more and more binds made a new view every time, the slowdown
    // that grew with play time until a level change - user report 2026-10-08, 32:9.)
    uint32_t start = (uint32_t)(k ^ (k >> 32)) & (VIEW_TAB - 1), slot = start;
    int free_slot = -1;
    for (int probe = 0; probe < 8; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        if (view_tab[slot].key == k && view_tab[slot].base == (__bridge void *)base) return (__bridge id<MTLTexture>)view_tab[slot].view;
        if (!view_tab[slot].key || view_tab[slot].key == ~0ull) {
            if (free_slot < 0) free_slot = (int)slot;
            if (!view_tab[slot].key) break;  // (end of the chain)
        }
    }
    slot = free_slot >= 0 ? (uint32_t)free_slot : start;
    id<MTLTexture> v = make();
    if (!v) return base;
    if (ENV("NXRT_LOG_VIEWCREATE")) {  // (debug: what keeps making new views - overworld soak, views pile up)
        extern uint64_t renderer_present_count(void);
        static int n;
        if (renderer_present_count() > (uint64_t)atol(ENV("NXRT_LOG_VIEWCREATE")) && n++ < 400)
            LOG("[viewnew] f%llu base %p %s %lux%lu fmt %lu type %lu params %#llx live %d\n", (unsigned long long)renderer_present_count(),
                (__bridge void *)base, base.label.UTF8String, (unsigned long)base.width, (unsigned long)base.height,
                (unsigned long)base.pixelFormat, (unsigned long)base.textureType, (unsigned long long)params, g_views_live);
    }
    if (view_tab[slot].view) { CFRelease(view_tab[slot].view); g_views_live--; }  // evict (8 probes all taken)
    g_views_live++;
    view_tab[slot].key = k;
    view_tab[slot].base = (__bridge void *)base;
    view_tab[slot].view = (void *)CFBridgingRetain(v);
    return v;
}

// Metal format views: an ordinary colour format can be viewed as another one of the same size, a compressed one only
// as itself or its sRGB / linear twin, depth and stencil formats only as themselves. Anything else is a Metal
// assertion (not only with API validation: newTextureViewWithPixelFormat checks it in the driver and aborts).
static int mtl_color_bytes(MTLPixelFormat f) {  // texel size of an ordinary colour format, 0 = not one
    switch (f) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint:
    case MTLPixelFormatR8Sint: case MTLPixelFormatA8Unorm:
        return 1;
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
    case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint: case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm:
    case MTLPixelFormatABGR4Unorm: case MTLPixelFormatBGR5A1Unorm:
        return 2;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float: case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatBGR10A2Unorm: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
        return 4;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA16Float:
        return 8;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float:
        return 16;
    default:
        return 0;
    }
}
static MTLPixelFormat mtl_linear_twin(MTLPixelFormat f) {  // sRGB format -> its linear one
    switch (f) {
    case MTLPixelFormatR8Unorm_sRGB: return MTLPixelFormatR8Unorm;
    case MTLPixelFormatRG8Unorm_sRGB: return MTLPixelFormatRG8Unorm;
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    case MTLPixelFormatBC1_RGBA_sRGB: return MTLPixelFormatBC1_RGBA;
    case MTLPixelFormatBC2_RGBA_sRGB: return MTLPixelFormatBC2_RGBA;
    case MTLPixelFormatBC3_RGBA_sRGB: return MTLPixelFormatBC3_RGBA;
    case MTLPixelFormatBC7_RGBAUnorm_sRGB: return MTLPixelFormatBC7_RGBAUnorm;
    default:
        if (f >= MTLPixelFormatASTC_4x4_sRGB && f <= MTLPixelFormatASTC_12x12_sRGB)
            return f - MTLPixelFormatASTC_4x4_sRGB + MTLPixelFormatASTC_4x4_LDR;
        return f;
    }
}
static bool mtl_view_compatible(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return true;
    int bb = mtl_color_bytes(base), vb = mtl_color_bytes(view);
    if (bb || vb) return bb == vb;
    return mtl_linear_twin(base) == mtl_linear_twin(view);  // (depth / stencil formats have no twin)
}
// A view in another format than the sRGB / linear twin needs MTLTextureUsagePixelFormatView on the texture (metal.m
// create_backend_texture: given only to textures the game views that way, as the flag stops lossless compression).
bool renderer_view_needs_format_flag(MTLPixelFormat base, MTLPixelFormat view) {
    return base != view && mtl_linear_twin(base) != mtl_linear_twin(view);
}

id<MTLTexture> renderer_view_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render,
                                   id<MTLBlitCommandEncoder> blit) {
    id<MTLTexture> base = renderer_texture(t, blit, render);
    if (!has_view || !v->setmask) return base;
    MTLPixelFormat pf = (v->setmask & 4) ? renderer_pixel_format(v->format) : base.pixelFormat;
    if (!mtl_view_compatible(base.pixelFormat, pf) ||  // (see mtl_view_compatible: the base format instead)
        (renderer_view_needs_format_flag(base.pixelFormat, pf) && !(base.usage & MTLTextureUsagePixelFormatView))) {
        static int nlog;
        if (!nlog++)
            LOG("[view] texture %ux%u fmt %#x (mtl %lu) can't be viewed as nvn %#x (mtl %lu): its own format instead "
                "(logged once)\n", t->width, t->height, t->format, (unsigned long)base.pixelFormat, v->format, (unsigned long)pf);
        pf = base.pixelFormat;
    }
    NSUInteger lv0 = (v->setmask & 1) ? v->base_level : 0;
    NSUInteger nlv = (v->setmask & 1) && v->num_levels ? v->num_levels : base.mipmapLevelCount - lv0;
    NSUInteger ly0 = (v->setmask & 2) ? v->min_layer : 0;
    NSUInteger nly = (v->setmask & 2) && v->num_layers ? v->num_layers : base.arrayLength - ly0;
    // (sampled, a cube is all 6 faces; rendered into, the faces it names - one face of a cube or a cube array is a 2D
    // view of it, and Metal wants cube views in whole cubes: part of a cube array is a 2D array)
    if (base.textureType == MTLTextureTypeCube && (!render || !(v->setmask & 2))) nly = 6;
    MTLTextureType type = base.textureType;
    if (render && nly == 1 &&
        (type == MTLTextureType2DArray || type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray))
        type = MTLTextureType2D;
    else if ((type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) && nly % 6)
        type = MTLTextureType2DArray;
    if (lv0 + nlv > base.mipmapLevelCount) nlv = base.mipmapLevelCount - lv0;
    if (pf == base.pixelFormat && lv0 == 0 && nlv == base.mipmapLevelCount && ly0 == 0 && type == base.textureType)
        return base;
    static int vlog;  // NXRT_LOG_VIEWS: texture views that change the pixel format
    if (pf != base.pixelFormat && vlog < 20 && getenv("NXRT_LOG_VIEWS")) {
        vlog++;
        LOG("[view] tex fmt %#x (mtl %lu) viewed as nvn %#x (mtl %lu)\n", t->format, (unsigned long)base.pixelFormat, v->format, (unsigned long)pf);
    }
    NSUInteger ns = type == MTLTextureType2D ? 1 : nly;
    uint64_t params = (uint64_t)pf | (uint64_t)type << 16 | (uint64_t)lv0 << 24 | (uint64_t)nlv << 32 | (uint64_t)ly0 << 40 | (uint64_t)ns << 52;
    id<MTLTexture> got = renderer_view_cached(base, params, ^{
        return [base newTextureViewWithPixelFormat:pf textureType:type levels:NSMakeRange(lv0, nlv) slices:NSMakeRange(ly0, ns)];
    });
    if (got == base && pf != base.pixelFormat) {
        static int nfail;
        if (nfail++ < 10) LOG("[view] FAILED %lu -> %lu (nvn %#x -> %#x)\n", (unsigned long)base.pixelFormat, (unsigned long)pf, t->format, v->format);
    }
    return got;
}



id<MTLTexture> renderer_dummy_texture(int type) {
    static id<MTLTexture> t[10];
    if (!t[type]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.width = d.height = 1;
        d.textureType = type == TEXTYPE_CUBE ? MTLTextureTypeCube : type == TEXTYPE_3D ? MTLTextureType3D
                      : type == TEXTYPE_2D_ARRAY ? MTLTextureType2DArray : type == TEXTYPE_1D ? MTLTextureType1D
                      : MTLTextureType2D;
        t[type] = [dev newTextureWithDescriptor:d];
    }
    return t[type];
}

MTLTextureType renderer_mtl_type_for(int t) {
    switch (t) {
    case TEXTYPE_1D: return MTLTextureType1D;
    case TEXTYPE_3D: return MTLTextureType3D;
    case TEXTYPE_CUBE: return MTLTextureTypeCube;
    case TEXTYPE_2D_ARRAY: return MTLTextureType2DArray;
    case TEXTYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
    case TEXTYPE_1D_ARRAY: return MTLTextureType1DArray;
    case TEXTYPE_BUFFER: return MTLTextureTypeTextureBuffer;
    default: return MTLTextureType2D;
    }
}

static id<MTLTexture> renderer_dummy_depth_texture(int type) {
    static id<MTLTexture> t[10];
    if (type < 0 || type >= 10) type = TEXTYPE_2D;
    if (type != TEXTYPE_2D && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE && type != TEXTYPE_CUBE_ARRAY)
        return renderer_dummy_texture(type);  // (3D has no shadow form; 1D depth textures don't exist)
    if (!t[type]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = MTLPixelFormatDepth32Float;
        d.width = d.height = 1;
        d.textureType = renderer_mtl_type_for(type);
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:d];
        if (!tex) return renderer_dummy_texture(type);
        NSUInteger slices = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY ? 6 : 1;
        id<MTLCommandBuffer> cb = [renderer_queue() commandBuffer];  // (committed now: runs before the current one)
        for (NSUInteger s = 0; s < slices; s++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.depthAttachment.texture = tex;
            rp.depthAttachment.slice = s;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = 1.0;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        [cb commit];
        t[type] = tex;
    }
    return t[type];
}

static bool renderer_mtl_depth_comparable(id<MTLTexture> t) {
    switch (t.pixelFormat) {
    case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatDepth32Float_Stencil8:
        return true;  // (depth targets are never D24S8 here)
    default: return false;
    }
}

id<MTLTexture> renderer_shadow_slot_texture(id<MTLTexture> tex, const ShaderTexture *st_, int i, uint64_t h) {
    if (tex && renderer_mtl_depth_comparable(tex) && tex.textureType != MTLTextureType2DMultisample) return tex;
    static bool logged[3];
    int why = !h ? 0 : !tex ? 1 : 2;
    if (!logged[why]) {
        logged[why] = true;
        static const char *names[3] = {"handle 0 (nothing bound)", "no texture (unregistered / finalized / type mismatch)",
                                       "not a comparable depth texture"};
        LOG("[mtl] shadow slot t%d (type %d): %s - handle %#llx, pixel format %lu: 1x1 depth dummy instead (once per cause)\n",
            i, st_->type, names[why], (unsigned long long)h, tex ? (unsigned long)tex.pixelFormat : 0ul);
    }
    return renderer_dummy_depth_texture(st_->type);
}

id<MTLTexture> renderer_apply_swizzle(id<MTLTexture> tex, NvnTexture *t, const NvnTextureView *v, bool has_view) {
    const uint8_t *sw = (has_view && (v->setmask & 8)) ? v->swizzle : t->swizzle;
    uint8_t c[4];
    for (int i = 0; i < 4; i++) c[i] = sw[i] <= 5 ? sw[i] : (uint8_t)(2 + i);
    if (!c[0] && !c[1] && !c[2] && !c[3]) return tex;  // never set: identity
    if (renderer_format_alpha_one(t->format))
        for (int i = 0; i < 4; i++) if (c[i] == MTLTextureSwizzleAlpha) c[i] = MTLTextureSwizzleOne;
    if (c[0] == 2 && c[1] == 3 && c[2] == 4 && c[3] == (renderer_format_alpha_one(t->format) ? 1 : 5)) return tex;
    if (tex.sampleCount > 1 || renderer_format_is_depth(t->format)) return tex;
    uint32_t swz = c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
    MTLTextureSwizzleChannels chans = MTLTextureSwizzleChannelsMake(c[0], c[1], c[2], c[3]);
    id<MTLTexture> view = renderer_view_cached(tex, 0x5000000000000000ull | swz, ^{
        NSUInteger slices = tex.textureType == MTLTextureType3D ? 1 : tex.arrayLength * (tex.textureType == MTLTextureTypeCube || tex.textureType == MTLTextureTypeCubeArray ? 6 : 1);
        return [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType
                                           levels:NSMakeRange(0, tex.mipmapLevelCount) slices:NSMakeRange(0, slices)
                                          swizzle:chans];
    });
    return view;
}

id<MTLTexture> renderer_sampled_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_type) {
    // uploads (if the CPU-side contents changed) go into their own command buffer, committed before
    // the current one so they land first
    if (t->target == 10) {  // a buffer texture: the view of guest memory itself (no format views or swizzles)
        id<MTLTexture> b = renderer_texture(t, nil, false);
        return b.textureType == MTLTextureTypeTextureBuffer ? b : nil;
    }
    id<MTLTexture> tex = renderer_view_texture(t, v, has_view, false, nil);
    tex = renderer_apply_swizzle(tex, t, v, has_view);
    MTLTextureType want = renderer_mtl_type_for(want_type);
    if (tex.textureType != want && tex.textureType != MTLTextureType2DMultisample) {
        id<MTLTexture> src = tex;
        if (want == MTLTextureType2D && (tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube))
            tex = renderer_view_cached(src, 0x6000000000000000ull | 1, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 1)]; });
        else if (want == MTLTextureType2DArray && tex.textureType == MTLTextureType2D)
            tex = renderer_view_cached(src, 0x6000000000000000ull | 2, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 1)]; });
        else if (want == MTLTextureTypeCube && tex.textureType == MTLTextureType2DArray && tex.arrayLength >= 6)
            tex = renderer_view_cached(src, 0x6000000000000000ull | 3, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureTypeCube levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 6)]; });
        else {
            static int nlog;
            if (nlog++ < 30) LOG("[tex] type mismatch: want %lu have %lu (%ux%ux%u target %u fmt %#x arr %lu)\n", (unsigned long)want,
                                 (unsigned long)tex.textureType, t->width, t->height, t->depth, t->target, t->format, (unsigned long)tex.arrayLength);
            return renderer_dummy_texture(want_type);
        }
    }
    return tex;
}
