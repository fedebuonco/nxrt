// Shader texture lookup and debug capture (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "nvn.h"
#include "renderer.h"
#include "renderer_bind.h"
#include "nxgame.h"
#include "renderer_samplers.h"
#include "tegra.h"
#include "renderer_context.h"
#include "rt.h"

#define st (renderer_context()->st)
#define dev (renderer_context()->dev)
void renderer_save_texture(id<MTLTexture> tex, const char *path);
extern char g_capture_dir[512];

NvnTexture *renderer_shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

uint32_t renderer_buffer_tex_slots(int stage, ShaderInfo *si) {
    uint32_t m = 0;
    for (int i = 0; i < si->ntex; i++) {
        NvnTexture *t = renderer_shader_texture_object(stage, &si->tex[i]);
        int slot = ((int)si->tex[i].handle_offset - 0x20) / 8;
        if (t && t->target == 10 && slot >= 0 && slot < 32) m |= 1u << slot;
    }
    return m;
}

void renderer_capture_texture(NvnTexture *t, id<MTLTexture> tex) {
    static NSMutableSet *saved;
    if (!saved) saved = [NSMutableSet new];
    NSNumber *k = @(t->addr ^ ((uint64_t)t->format << 56));
    if ([saved containsObject:k] || saved.count >= 400) return;
    [saved addObject:k];
    if (tex.textureType == MTLTextureTypeTextureBuffer || tex.width > 4096 || tex.height > 4096) return;
    char path[700];
    snprintf(path, sizeof path, "%s/%llx_%ux%u_f%x.png", g_capture_dir, (unsigned long long)t->addr, t->width, t->height, t->format);
    renderer_save_texture(tex, path);  // (its own command buffer: the contents as the GPU last left them)
    // and its alpha as grey (text is often in alpha only, the colour plain white): through a swizzled view, so
    // compressed formats work too
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleAlpha, MTLTextureSwizzleAlpha,
                                                                 MTLTextureSwizzleAlpha, MTLTextureSwizzleOne);
    id<MTLTexture> av = tex.textureType == MTLTextureType2D ?
        [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw] : nil;
    if (av) {
        snprintf(path, sizeof path, "%s/%llx_%ux%u_f%x_alpha.png", g_capture_dir, (unsigned long long)t->addr, t->width, t->height, t->format);
        renderer_save_texture(av, path);
    }
}

static int cbuf_base = 3;
#define st (renderer_context()->st)
#define dev (renderer_context()->dev)
#define enc (renderer_context()->enc)
id<MTLBuffer> renderer_ring_buffer(void);
uint64_t renderer_ring_alloc(const void *data, uint64_t size);
void renderer_bind_cbufs(int stage, ShaderInfo *si) {
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        if (k == 0) {
            // driver constants: storage buffer bindings {addr_lo, addr_hi, size, 0} in a 0x100-byte table
            // per hardware stage at 0x110 + 0x100 * stage (vertex 0, fragment 4)
            uint32_t drv[0x800 / 4] = {0};
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
            uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
            uint64_t off = renderer_ring_alloc(drv, size);
            if (stage == 0) [enc setVertexBuffer:renderer_ring_buffer() offset:off atIndex:0];
            else [enc setFragmentBuffer:renderer_ring_buffer() offset:off atIndex:0];
            continue;
        }
        if (k == 1) {  // compiler constants stored after the shader code
            NvnShaderStage *ss = &st.program->stages[stage];
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
            uint64_t off = renderer_ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
            if (stage == 0) [enc setVertexBuffer:renderer_ring_buffer() offset:off atIndex:1];
            else [enc setFragmentBuffer:renderer_ring_buffer() offset:off atIndex:1];
            continue;
        }
        int idx = k - cbuf_base;
        Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
        uint64_t size = si->cbuf_size[k];
        if (!r.addr) {
            static const uint8_t zeros[0x10000];
            uint64_t off = renderer_ring_alloc(zeros, size ? size : 256);
            if (stage == 0) [enc setVertexBuffer:renderer_ring_buffer() offset:off atIndex:k];
            else [enc setFragmentBuffer:renderer_ring_buffer() offset:off atIndex:k];
            continue;
        }
        if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;  // indexed: whole buffer
        if (size > r.size && r.size) size = r.size;
        if (size < 16) size = 16;
        uint64_t off = renderer_ring_alloc((void *)(uintptr_t)r.addr, size);  // snapshot, as later CPU writes must not leak in
        if (stage == 0) [enc setVertexBuffer:renderer_ring_buffer() offset:off atIndex:k];
        else [enc setFragmentBuffer:renderer_ring_buffer() offset:off atIndex:k];
    }
}

#define st (renderer_context()->st)
#define dev (renderer_context()->dev)
bool renderer_is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

#include "renderer_snapshots.h"
#include "renderer_textures.h"
void renderer_end_pass(void);
id<MTLCommandBuffer> renderer_command_buffer(void);
id<MTLTexture> renderer_texture(NvnTexture *t, id<MTLBlitCommandEncoder> blit, bool for_render);

void renderer_feedback_resolve(Prog *pr) {
    RendererBindings *b = renderer_bindings();
    b->nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("NXRT_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = renderer_shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!renderer_is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < b->nfeedback; k++) have |= b->feedback_tex[k] == t;
            if (have || b->nfeedback == 4) continue;
            if (!b->snap_copies) b->snap_copies = [NSMutableDictionary new];
            NSMutableDictionary *copies = b->snap_copies;
            NSValue *key = [NSValue valueWithPointer:t];
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("NXRT_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (renderer_snapshot_texture(k) == t) si = k;
            // A draw that writes the same depth buffer it samples can't depend on what it reads (that is undefined on the
            // Switch too): seen in the shadow-map pass, whose material shader still declares the shadow map from the
            // main pass. Any earlier snapshot does; no copy, no pass break.
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            if (!nosnap && si >= 0 && (renderer_snapshot_ok(si) || self_write) && copies[key]) {  // unchanged since the last snapshot
                b->feedback_tex[b->nfeedback] = t;
                b->feedback_copy[b->nfeedback++] = copies[key];
                continue;
            }
            b->feedback_copies++;
            static int logfb = -1;
            if (logfb < 0) logfb = getenv("NXRT_LOG_FEEDBACK") != NULL;
            if (logfb && b->feedback_copies % 50 == 1)
                LOG("[feedback] copy %s %ux%u fmt %#x stage %d fs %016llx (last invalidated by %s) ds %d/%d/%d stencil %d ncolor %d c0 %ux%u fmt %#x mask %x\n", t == st.rt.depth ? "DEPTH" : "COLOR",
                    t->width, t->height, t->format, sg, (unsigned long long)b->cur_fs_hash, si >= 0 ? renderer_snapshot_why(si) : "new",
                    st.ds[0], st.ds[1], st.ds[2], st.ds[3], st.rt.ncolor, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0, st.channel_mask);
            renderer_end_pass();
            id<MTLTexture> src = renderer_texture(t, nil, true);
            id<MTLTexture> dst = copies[key];
            if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width
                                                                                           height:src.height mipmapped:NO];
                d.textureType = src.textureType == MTLTextureType2DMultisample ? MTLTextureType2DMultisample : MTLTextureType2D;
                d.sampleCount = src.sampleCount;
                d.usage = MTLTextureUsageShaderRead;  // (only same-format views of it: compressible)
                d.storageMode = MTLStorageModePrivate;
                dst = [dev newTextureWithDescriptor:d];
                copies[key] = dst;
            }
            id<MTLBlitCommandEncoder> blit = [renderer_command_buffer() blitCommandEncoder];
            [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            b->feedback_tex[b->nfeedback] = t;
            b->feedback_copy[b->nfeedback++] = dst;
            if (si < 0) { static int rr; si = rr++ & 7; }
            renderer_snapshot_take(si, t);
            st.rt_dirty = true;  // the pass resumes (loading) after the copy
        }
}

static RendererBindings g_b;
RendererBindings *renderer_bindings(void) { return &g_b; }
int renderer_feedback_count(void) { return g_b.nfeedback; }
NvnTexture *renderer_feedback_texture(int i) { return i >= 0 && i < g_b.nfeedback ? g_b.feedback_tex[i] : NULL; }
id<MTLTexture> renderer_feedback_copy(int i) { return i >= 0 && i < g_b.nfeedback ? g_b.feedback_copy[i] : nil; }
uint64_t renderer_feedback_copies(void) { return g_b.feedback_copies; }
uint64_t renderer_snap_copy_count(void) { return g_b.snap_copies ? g_b.snap_copies.count : 0; }
uint64_t renderer_cur_fs_hash(void) { return g_b.cur_fs_hash; }
void renderer_set_cur_fs_hash(uint64_t h) { g_b.cur_fs_hash = h; }
void renderer_feedback_forget(NvnTexture *t) { if (g_b.snap_copies) [g_b.snap_copies removeObjectForKey:[NSValue valueWithPointer:t]]; }
void renderer_feedback_reset(void) {
    g_b.nfeedback = 0;
    for (int i = 0; i < 4; i++) { g_b.feedback_tex[i] = NULL; g_b.feedback_copy[i] = nil; }
    [g_b.snap_copies removeAllObjects];
    g_b.snap_copies = nil;
    g_b.cur_fs_hash = 0;
    g_b.feedback_copies = 0;
}

#define st (renderer_context()->st)
#define enc (renderer_context()->enc)
#define tex_sc (renderer_context()->tex_sc)
#define tex_fr (renderer_context()->tex_fr)
#define tex_wh (renderer_context()->tex_wh)
#define draw_watched (renderer_context()->draw_watched)
extern bool g_debug_frame;
extern char g_capture_dir[512];
int renderer_texpatch_of(NvnTexture *t);
bool renderer_capture_watches(uint64_t a);
void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);
void renderer_save_game_texture(NvnTexture *t, id<MTLTexture> tex, const char *path, const char *alpha_path);
void renderer_save_texture(id<MTLTexture> tex, const char *path);
void renderer_save_texture_alpha(id<MTLTexture> tex, const char *path);
bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_bind_textures(int stage, ShaderInfo *si) {
    if (stage == 1) {
    }
    bool mapped = si->ntex > MSL_MAX_SAMPLERS;  // (fragment only: the translator refuses such vertex shaders)
    __strong id<MTLSamplerState> msmp[64];
    NvnSamplerDesc *mdesc[64];
    for (int i = 0; i < si->ntex; i++) {
        ShaderTexture *st_ = &si->tex[i];
        int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
        uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
        tex_sc[stage][i][0] = tex_sc[stage][i][1] = 1;
        if (i < 64) { tex_wh[stage][i][0] = tex_wh[stage][i][1] = 0; tex_fr[stage][i][0] = tex_fr[stage][i][1] = 1; }
        if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            if (e->tex) tex = renderer_sampled_texture(e->tex, &e->view, e->has_view, st_->type);
            if (e->tex && tex && g_debug_frame && g_capture_dir[0]) renderer_capture_texture(e->tex, tex);
            else if (e->dead_format) nvn_dead_slot_sampled(e, tic);
            if (e->tex && stage == 1 && NX_HOOK(tex_replace)) {
                extern int renderer_texpatch_of(NvnTexture *t);
bool renderer_capture_watches(uint64_t a);
                NvnTexture *rep = NX_HOOK(tex_replace)(renderer_texpatch_of(e->tex));
                if (rep) { static NvnTextureView none; tex = renderer_sampled_texture(rep, &none, false, st_->type); }
            }
            if (e->tex && i < 64) {
                renderer_texture_scale(e->tex, &tex_sc[stage][i][0], &tex_sc[stage][i][1]);
                renderer_texture_fraction(e->tex, &tex_fr[stage][i][0], &tex_fr[stage][i][1]);
                tex_wh[stage][i][0] = e->tex->width; tex_wh[stage][i][1] = e->tex->height;
            }
            for (int k = 0; k < renderer_feedback_count(); k++)
                if (e->tex == renderer_feedback_texture(k)) {
                    id<MTLTexture> fc = renderer_feedback_copy(k);
                    MTLTextureType want = renderer_mtl_type_for(st_->type);
                    if (fc.textureType != want && want == MTLTextureType2DArray)
                        fc = [fc newTextureViewWithPixelFormat:fc.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
                    tex = fc;
                }
            if (g_debug_frame && g_capture_dir[0] && e->tex) {  // (a capture: which texture each draw samples, light)
                LOG("[dtex] slot %d tex %ux%u f%#x addr %llx mtl %p %lux%lu pf %lu sc %g,%g fr %g,%g view %d\n", i, e->tex->width,
                    e->tex->height, e->tex->format, (unsigned long long)e->tex->addr, (__bridge void *)tex, (unsigned long)tex.width,
                    (unsigned long)tex.height, (unsigned long)tex.pixelFormat, i < 64 ? tex_sc[stage][i][0] : 1, i < 64 ? tex_sc[stage][i][1] : 1,
                    i < 64 ? tex_fr[stage][i][0] : 1, i < 64 ? tex_fr[stage][i][1] : 1, e->has_view);
                if (renderer_capture_watches(e->tex->addr)) draw_watched = true;
            }
            if (g_debug_frame && !g_capture_dir[0] && e->tex && e->tex->addr) {
                uint64_t total = tegra_total_size(&e->tex->layout), nz = 0;
                const uint8_t *p = (const uint8_t *)(uintptr_t)e->tex->addr;
                for (uint64_t k = 0; k < total; k++) nz += p[k] != 0;
                if (e->tex->width == 1024 && e->tex->height == 2048) {
                    static int once;
                    if (!once++ || g_debug_frame) {
                        id<MTLTexture> base = renderer_texture(e->tex, nil, false);
                        LOG("[dtex] glyph cache target %u depth %u layers %u levels %u stride %u flags %#x base type %lu arrlen %lu\n",
                            e->tex->target, e->tex->depth, e->tex->layout.layers, e->tex->levels, e->tex->stride, e->tex->flags,
                            (unsigned long)base.textureType, (unsigned long)base.arrayLength);
                        LOG("[dtex] glyph view: has %d mask %#x levels %u+%u layers %u+%u fmt %#x target %u\n", e->has_view, e->view.setmask,
                            e->view.base_level, e->view.num_levels, e->view.min_layer, e->view.num_layers, e->view.format, e->view.target);
                        LOG("[dtex] glyph cache: mtl fmt %lu type %lu swizzle tex %d %d %d %d view(%d) %d %d %d %d, tex.swz %08x\n",
                            (unsigned long)base.pixelFormat, (unsigned long)tex.textureType, e->tex->swizzle[0], e->tex->swizzle[1],
                            e->tex->swizzle[2], e->tex->swizzle[3], e->has_view && (e->view.setmask & 8), e->view.swizzle[0],
                            e->view.swizzle[1], e->view.swizzle[2], e->view.swizzle[3],
                            (unsigned)(tex.swizzle.red | tex.swizzle.green << 8 | tex.swizzle.blue << 16 | tex.swizzle.alpha << 24));
                        renderer_save_texture(base, "build/glyph_gpu.png");
                        NvnSamplerDesc *sd = (h && st.spool && tsc < (uint32_t)st.spool->count) ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                        if (sd) { const uint32_t *w = (const uint32_t *)sd; LOG("[dtex] glyph sampler words %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
                    }
                }
                if (e->tex->width == 2 && e->tex->height == 2 && e->tex->depth == 6) {
                    static int once2;
                    if (!once2++) {
                        FILE *rf = fopen("build/raw_cube2.bin", "wb");
                        if (rf) { fwrite(p, 1, total, rf); fclose(rf); }
                        LOG("[dtex] cube2 levels %u layers %u bh %u layer_size %llu\n", e->tex->levels, e->tex->layout.layers,
                            e->tex->layout.block_height_log2, tegra_layer_size(&e->tex->layout));
                    }
                }
                if ((e->tex->flags & 0x10) && !getenv("NO_RAW")) {
                    static int nraw;
                    if (nraw++ < 2) {
                        char rp[64];
                        snprintf(rp, sizeof rp, "build/raw_%llx.bin", e->tex->addr);
                        FILE *rf = fopen(rp, "wb");
                        if (rf) { fwrite(p, 1, total, rf); fclose(rf); }
                        LOG("[dtex] linear texture stride %u -> %s\n", e->tex->stride, rp);
                    }
                }
                LOG("[dtex] slot %d tex %ux%u f%#x addr %llx size %llu nonzero %llu gpu_owned %d\n", i, e->tex->width,
                    e->tex->height, e->tex->format, e->tex->addr, total, nz, renderer_is_gpu_owned(e->tex));
            }
        }
        NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
        if (sd) smp = renderer_sampler_for(sd);
        if (st_->shadow) tex = renderer_shadow_slot_texture(tex, st_, i, h);
        if (!tex) tex = renderer_dummy_texture(st_->type);
        if (!smp) smp = renderer_default_sampler();
        if (g_debug_frame && !g_capture_dir[0] && tex.textureType == MTLTextureType2D && stage == 1) {  // (each draw's
                                                                    // textures: a capture saves each texture once instead)
            static int dn;
            char path[128], apath[128];
            snprintf(path, sizeof path, "build/dtex_%03d_%d.png", dn++, i);
            snprintf(apath, sizeof apath, "build/dtex_%03d_%d_a.png", dn - 1, i);
            NvnTexture *gt = h && st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
            if (gt) renderer_save_game_texture(gt, tex, path, apath);
            else { renderer_save_texture(tex, path); renderer_save_texture_alpha(tex, apath); }
        }
        if (stage == 0) { [enc setVertexTexture:tex atIndex:i]; [enc setVertexSamplerState:smp atIndex:i]; }
        else if (mapped) { [enc setFragmentTexture:tex atIndex:i]; msmp[i] = smp; mdesc[i] = sd; }
        else { [enc setFragmentTexture:tex atIndex:i]; [enc setFragmentSamplerState:smp atIndex:i]; }
    }
    if (mapped && stage == 1) {
        __strong id<MTLSamplerState> slot[MSL_MAX_SAMPLERS];
        uint32_t map[64];
        int ns = renderer_sampler_slots(si->ntex, msmp, mdesc, slot, map);
        for (int k = 0; k < MSL_MAX_SAMPLERS; k++) [enc setFragmentSamplerState:k < ns ? slot[k] : renderer_default_sampler() atIndex:k];
        [enc setFragmentBytes:map length:(NSUInteger)si->ntex * 4 atIndex:SMAP_BUFFER_INDEX];
    }
}
