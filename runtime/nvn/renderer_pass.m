// Render-pass geometry (split from renderer.m).
#import <Foundation/Foundation.h>
#include <math.h>

#include "nvn.h"
#include "renderer.h"
#include "renderer_context.h"
#include "renderer_pass.h"
#include "rt.h"

#define st (renderer_context()->st)
#define pass_sx (renderer_context()->pass_sx)
#define pass_sy (renderer_context()->pass_sy)
#define pass_rt_w (renderer_context()->pass_rt_w)
#define pass_rt_h (renderer_context()->pass_rt_h)
#define vp_sx (renderer_context()->vp_sx)
#define vp_sy (renderer_context()->vp_sy)
#define vp_ox (renderer_context()->vp_ox)
#define vp_oy (renderer_context()->vp_oy)
#define enc (renderer_context()->enc)

void renderer_pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

void renderer_apply_scissor(void) {
    int w, h;
    renderer_pass_size(&w, &h);
    if (!w) return;
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;  // "unbounded" scissors stay unbounded
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (pass_rt_w && x + sw > pass_rt_w) sw = pass_rt_w - x;  // (inside the render area)
    if (pass_rt_h && y + sh > pass_rt_h) sh = pass_rt_h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    [enc setScissorRect:(MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)sw, (NSUInteger)sh}];
}

// Deferred clears (renderer_pass.m): a full clear is recorded on its texture and becomes the load action of the next
// pass that binds it as a target.
#import <Foundation/Foundation.h>
#include <string.h>
#include <stdlib.h>

#include "nvn.h"
#include "renderer.h"
#include "renderer_context.h"
#include "renderer_pass.h"
#include "renderer_textures.h"
#include "rt.h"

#define st (renderer_context()->st)

#define TD_TAB 2048
static RendererTexDefer td_tab[TD_TAB];
static int td_pending_count;  // textures with a recorded clear
static bool td_flushing;

int renderer_td_pending(void) { return td_pending_count; }
void renderer_td_pending_dec(void) { if (td_pending_count > 0) td_pending_count--; }

bool renderer_td_defer_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = ENV("NXRT_MTL_DEFER_CLEAR"); v = !e || atoi(e); }
    return v;
}

RendererTexDefer *renderer_td_get(NvnTexture *t, bool create) {
    if (!t) return NULL;
    uint32_t h = (uint32_t)(((uintptr_t)t >> 4) * 0x9E3779B1u) & (TD_TAB - 1);
    for (int probe = 0; probe < TD_TAB; probe++, h = (h + 1) & (TD_TAB - 1)) {
        RendererTexDefer *e = &td_tab[h];
        if (e->t == t) {
            if (!e->backend) e->backend = t->backend;
            else if (t->backend && e->backend != t->backend) {
                if (e->clear_on) renderer_td_pending_dec();
                memset(e, 0, sizeof *e);
                e->t = t;
                e->backend = t->backend;
            }
            return e;
        }
        if (!e->t) {
            if (!create) return NULL;
            memset(e, 0, sizeof *e);
            e->t = t;
            e->backend = t->backend;
            return e;
        }
    }
    return NULL;
}

void renderer_td_forget(NvnTexture *t) {
    RendererTexDefer *e = renderer_td_get(t, false);
    if (!e) return;
    if (e->clear_on) renderer_td_pending_dec();
    e->t = NULL;
    e->backend = (void *)1;
}

void renderer_td_flush(NvnTexture *t) {
    RendererTexDefer *e = t ? renderer_td_get(t, false) : NULL;
    if (!e || !e->clear_on || td_flushing) return;
    td_flushing = true;
    RtState saved = st.rt;
    bool dirty = st.rt_dirty;
    renderer_end_pass();
    memset(&st.rt, 0, sizeof st.rt);
    if (renderer_format_is_depth(t->format)) st.rt.depth = t;
    else { st.rt.ncolor = 1; st.rt.color[0] = t; }
    st.rt_dirty = true;
    renderer_begin_pass();
    renderer_end_pass();
    renderer_stat_clear_pass();
    st.rt = saved;
    st.rt_dirty = true;
    td_flushing = false;
    (void)dirty;
}

void renderer_td_flush_targets(const RtState *rt) {
    if (!renderer_td_pending()) return;
    for (int i = 0; i < rt->ncolor; i++) renderer_td_flush(rt->color[i]);
    renderer_td_flush(rt->depth);
}

void renderer_td_record(NvnTexture *t, bool is_color, int kind, const void *color, bool depth, float dval, bool stencil,
                        uint32_t sval) {
    renderer_mark_gpu_owned(t);
    RendererTexDefer *e = renderer_td_get(t, true);
    if (!e) return;
    if (!e->clear_on) { td_pending_count++; e->clear_color = e->clear_depth = e->clear_stencil = false; }
    e->clear_on = true;
    if (is_color) {
        e->clear_color = true;
        for (int k = 0; k < 4; k++)
            e->cc[k] = kind == 0 ? ((const float *)color)[k] : kind == 1 ? ((const int32_t *)color)[k] : ((const uint32_t *)color)[k];
    } else {
        if (depth) { e->clear_depth = true; e->dval = dval; }
        if (stencil) { e->clear_stencil = true; e->sval = sval; }
    }
}

void renderer_td_reset(void) {
    memset(td_tab, 0, sizeof td_tab);
    td_pending_count = 0;
    td_flushing = false;
}

#define enc (renderer_context()->enc)
#define opening_pass (renderer_context()->opening_pass)
#define pass_rt (renderer_context()->pass_rt)
#define pass_sx (renderer_context()->pass_sx)
#define pass_sy (renderer_context()->pass_sy)
#define pass_rt_w (renderer_context()->pass_rt_w)
#define pass_rt_h (renderer_context()->pass_rt_h)
#define vp_sx (renderer_context()->vp_sx)
#define vp_sy (renderer_context()->vp_sy)
#define vp_ox (renderer_context()->vp_ox)
#define vp_oy (renderer_context()->vp_oy)
#define g_passes_in_cb (renderer_context()->passes_in_cb)
#define cache (renderer_context()->cache)
#define cmdbuf (renderer_context()->cmdbuf)
void renderer_submit(void);
void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);

void renderer_pass_debug_begin(MTLRenderPassDescriptor *rp);
void renderer_pass_gpu_label(MTLRenderPassDescriptor *rp);
void renderer_pass_debug_end(NvnTexture *c0);
id<MTLCommandBuffer> renderer_command_buffer(void);
bool renderer_gpu_passes(void);
void renderer_stat_pass_add(void);
void renderer_stat_clears_load_add(void);

void renderer_begin_pass(void) {
    if (enc && !st.rt_dirty) return;
    opening_pass = true;  // (the chunk submit's end_pass below must not flush anything into this pass)
    renderer_end_pass();
    st.rt_dirty = false;
    pass_rt = st.rt;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int w = 0, h = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        id<MTLTexture> tex = renderer_view_texture(t, &st.rt.cview[i], st.rt.has_cview[i], true, nil);
        rp.colorAttachments[i].texture = tex;
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        // reshaped output: areas outside the game's 16:9 drawing (interiors, fades) start black each frame
        extern bool renderer_texture_first_write(NvnTexture *t);
        extern bool renderer_texture_level_fresh(NvnTexture *t);
        extern bool renderer_texture_frame_fresh(NvnTexture *t);
        bool fresh = renderer_texture_level_fresh(t) | renderer_texture_frame_fresh(t);
        if (renderer_texture_first_write(t) || fresh) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, fresh ? 0 : 1);
        }
        RendererTexDefer *e = renderer_td_pending() ? renderer_td_get(t, false) : NULL;
        if (e && e->clear_on && e->clear_color) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(e->cc[0], e->cc[1], e->cc[2], e->cc[3]);
            e->clear_on = false;
            renderer_td_pending_dec();
            renderer_stat_clears_load_add();
        }
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (!w) renderer_texture_scale(t, &pass_sx, &pass_sy);
        w = (int)tex.width; h = (int)tex.height;
    }
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        id<MTLTexture> tex = renderer_view_texture(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, nil);
        RendererTexDefer *e = renderer_td_pending() ? renderer_td_get(st.rt.depth, false) : NULL;
        bool has_d = renderer_format_is_depth(st.rt.depth->format) && st.rt.depth->format != 0x31;
        bool has_s = renderer_format_has_stencil(st.rt.depth->format);
        bool clr_d = e && e->clear_on && e->clear_depth, clr_s = e && e->clear_on && e->clear_stencil;
        if (has_d) {
            rp.depthAttachment.texture = tex;
            rp.depthAttachment.loadAction = clr_d ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = clr_d ? e->dval : 1.0;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        if (has_s) {
            rp.stencilAttachment.texture = tex;
            rp.stencilAttachment.loadAction = clr_s ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = clr_s ? e->sval : 0;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (e && e->clear_on) { e->clear_on = false; renderer_td_pending_dec(); renderer_stat_clears_load_add(); }
        if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = (int)tex.width; h = (int)tex.height; }
    }
    pass_rt_w = pass_rt_h = 0;
    if (!w) {
        pass_sx = pass_sy = 1;
        rp.renderTargetWidth = 16; rp.renderTargetHeight = 16;
        rp.defaultRasterSampleCount = 1;
    } else {
        // Dynamic resolution draws into the top-left part of screen-sized targets: the pass's render area is that part.
        // Apple GPUs load and store every tile of the render area (default: the attachments' whole size) at each pass
        // however little is drawn, so a 5120x1440 target cost about the same at 1/3 of it (1707x480) as at all of it.
        int cw = 0, ch = 0, mw = 0, mh = 0;
        for (int i = -1; i < st.rt.ncolor; i++) {
            NvnTexture *t = i < 0 ? st.rt.depth : st.rt.color[i];
            id<MTLTexture> tex = i < 0 ? rp.depthAttachment.texture ?: rp.stencilAttachment.texture : rp.colorAttachments[i].texture;
            if (!t || !tex) continue;
            float fx, fy, sx, sy;
            renderer_texture_fraction(t, &fx, &fy);
            renderer_texture_scale(t, &sx, &sy);
            // (as far as the scissor reaches: pass_size)
            int pw = (int)ceilf(fmaxf(tex.width * fx, lroundf(t->width * sx))), ph = (int)ceilf(fmaxf(tex.height * fy, lroundf(t->height * sy)));
            if (pw > cw) cw = pw;
            if (ph > ch) ch = ph;
            if (!mw || (int)tex.width < mw) mw = (int)tex.width;
            if (!mh || (int)tex.height < mh) mh = (int)tex.height;
        }
        static int whole = -1;  // NXRT_PASS_WHOLE=1: the attachments' whole size, as before (tests)
        if (whole < 0) whole = getenv("NXRT_PASS_WHOLE") && atoi(getenv("NXRT_PASS_WHOLE"));
        if (!whole && cw > 0 && ch > 0 && (cw < mw || ch < mh)) {
            pass_rt_w = cw < mw ? cw : mw, pass_rt_h = ch < mh ? ch : mh;
            rp.renderTargetWidth = (NSUInteger)pass_rt_w;
            rp.renderTargetHeight = (NSUInteger)pass_rt_h;
        }
    }
    // Commit the frame in chunks: the GPU starts on the first passes while the CPU still encodes the rest. The
    // game waits for its previous frame before it submits the next one, so with one command buffer per frame the
    // GPU sat idle during our encoding and a frame cost GPU + encoding time (5120x1440 with the GPU clocked down:
    // ~15.5 + a few ms -> missed vsync -> 30 fps). NXRT_CHUNK_PASSES=<n> (0 = one buffer per frame).
    // 2 since 2026-10-25: measured with LAS on the same scripted walk, every pass in its own buffer (1) and two a
    // buffer (2) both start the GPU soon enough: 2x 38.2 -> 49.0 fps, frames over 20 ms 56.8% -> 22.3%; 1x 57.5 ->
    // 59.1 fps, 4.3% -> 1.5% (1 measured 50.1/48.8 and 59.2: the same within noise, with twice the submissions).
    static int chunk = -1;
    if (chunk < 0) { const char *e = ENV("NXRT_CHUNK_PASSES"); chunk = renderer_gpu_passes() ? 1 : e ? atoi(e) : 2; }
    if (chunk > 0 && cmdbuf && ++g_passes_in_cb >= chunk) renderer_submit();
    renderer_pass_debug_begin(rp);
    renderer_pass_gpu_label(rp);
    enc = [renderer_command_buffer() renderCommandEncoderWithDescriptor:rp];
    opening_pass = false;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    renderer_stat_pass_add();
}

void renderer_end_pass(void) {
    if (!renderer_td_defer_on() && !opening_pass) renderer_td_flush_targets(&st.rt);  // (off mode: as before, the clear gets its pass now)
    if (enc) {
        [enc endEncoding];
        enc = nil;
        renderer_pass_debug_end(pass_rt.ncolor ? pass_rt.color[0] : NULL);
    }
}