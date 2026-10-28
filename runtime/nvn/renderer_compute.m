// Compute programs (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "renderer_compute.h"
#include "renderer_context.h"
#include "maxwell/shader.h"
#include "renderer.h"
#include "renderer_bind.h"
#include "renderer_pipelines.h"
#include "renderer_prep.h"
#include "renderer_programs.h"
#include "shader_rec_norm.h"
#include "slowframe.h"
#include "rt.h"

#define dev (renderer_context()->dev)
static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

// ------------------------------------------------------------------ compute
// Compute driver constants (c0): 0x200 work group counts, 0x210 + 16*i uniform buffers (compute has
// few hardware cbuf slots, so the compiler reads the rest through global memory), 0x310 + 16*i
// storage buffers; each table entry is {addr_lo, addr_hi, size, 0}.

static NSMutableDictionary *g_cs_progs;  // code address -> CsProg

void renderer_compute_compile(CsProg *cp) {
    NSError *err = nil;
    double t0 = now_ms();
    NSString *salt = getenv("NXRT_SHADER_SALT") ? [NSString stringWithFormat:@"\n// %s\n", getenv("NXRT_SHADER_SALT")] : @"";
    id<MTLLibrary> lib = [dev newLibraryWithSource:[@(cp->si->msl) stringByAppendingString:salt] options:nil error:&err];
    id<MTLFunction> f = lib ? [lib newFunctionWithName:@"main0"] : nil;
    id<MTLComputePipelineState> pipe = f ? renderer_archive_compute_pipeline(f, &err) : nil;
    renderer_note_compile(now_ms() - t0);
    slow_note_build("pipeline cs", (uint64_t)((now_ms() - t0) * 1e6));
        if (!pipe) {
        LOG("[compute] compile failed: %s\n%s\n", err.localizedDescription.UTF8String, cp->si->msl);
        cp->failed = true;
        return;
    }
    cp->pipe = pipe;
    if (cp->code_addr) LOG("[compute] program %llx: local size %ux%ux%u, cbufs mask %#x\n", cp->code_addr, cp->local[0], cp->local[1],
        cp->local[2], cp->si->cbuf_mask);
    cp->ready = true;
}

uint64_t renderer_compute_content_key(uint64_t code_hash, const uint32_t local[3]) {
    uint64_t h = code_hash;
    for (int i = 0; i < 3; i++) { h ^= local[i]; h *= 0x100000001B3ull; }
    return h;
}

bool renderer_compute_translate(CsProg *cp, const uint8_t *code, size_t max, uint32_t shared_bytes) {
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.shared_bytes = shared_bytes;
    cp->si = slow_translate(code, max, STAGE_COMPUTE, &k);
    if (!cp->si->msl) { LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, cp->code_addr); cp->failed = true; return false; }
    return true;
}

CsProg *renderer_compute_program(NvnProgram *p) {
    NvnShaderStage *ss = &p->stages[5];
    if (!ss->code_addr) return NULL;
    NSValue *key = [NSValue valueWithPointer:(void *)(uintptr_t)ss->code_addr];
    NSMutableDictionary *progs = g_cs_progs;
    if (!progs) progs = g_cs_progs = [NSMutableDictionary new];
    NSValue *v = progs[key];
    if (v) {
        CsProg *cp = v.pointerValue;
        shader_fp_check(&cp->fp, ss, "cs");  // (keyed by code address: shader_prep.h)
        return cp->failed ? NULL : cp;
    }
    uint32_t local[3];
    for (int i = 0; i < 3; i++) {
        local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!local[i]) local[i] = 1;
    }
    if (getenv("NXRT_LOG_CS_CONTROL") && ss->control) {
        const uint32_t *w = (const uint32_t *)(ss->control + 0x700);
        LOG("[cscontrol] %llx local %ux%ux%u:", (unsigned long long)ss->code_addr, local[0], local[1], local[2]);
        for (int i = 0; i < 0x30; i++) LOG(" %x", w[i]);
        LOG("\n");
    }
    if (shader_rec_recording()) {
        ShaderRec rec = {.kind = 'C', .vs = shader_stage_hash(ss), .local = {local[0], local[1], local[2]}};
        shader_rec_note(&rec);
    }
    {   // prepared before the game (the same code and local size)
        uint64_t ch = shader_stage_hash(ss);
        CsProg *pc = NULL;
        if (ch) pc = [renderer_prep_cs(renderer_compute_content_key(ch, local)) pointerValue];
        if (pc) {
            progs[key] = [NSValue valueWithPointer:pc];
            shader_fp_check(&pc->fp, ss, "cs");
            return pc->failed ? NULL : pc;
        }
        shader_first_visit("compute", ch, 0, renderer_compute_content_key(ch, local));
    }
    CsProg *cp = calloc(1, sizeof *cp);
    progs[key] = [NSValue valueWithPointer:cp];
    shader_fp_check(&cp->fp, ss, "cs");  // (takes the fingerprint)
    cp->code_addr = ss->code_addr;
    memcpy(cp->local, local, sizeof local);
    uint32_t shared = ss->control ? *(const uint32_t *)(ss->control + 0x710) : 0;  // (its shared memory, bytes)
    if (!renderer_compute_translate(cp, (const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, shared)) return NULL;
    if (renderer_sync_shaders()) {
        renderer_compute_compile(cp);
        return cp->failed ? NULL : cp;
    }
    // compiled off the render thread, as graphics programs are (get_program): the first dispatches are skipped
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("nxrt.compute", DISPATCH_QUEUE_CONCURRENT);
    dispatch_async(q, ^{ renderer_compute_compile(cp); });
    return cp;
}



void renderer_compute_reset(void) { [g_cs_progs removeAllObjects]; }

#define st (renderer_context()->st)
#define dev (renderer_context()->dev)
#define tex_sc (renderer_context()->tex_sc)
static int cbuf_base = 3;
#define RSCALE_BUFFER_INDEX 18
id<MTLBuffer> renderer_ring_buffer(void);
uint64_t renderer_ring_alloc(const void *data, uint64_t size);
void renderer_stat_dispatch_add(void);
#define tex_fr (renderer_context()->tex_fr)
extern bool g_debug_frame;
extern char g_capture_dir[512];
id<MTLCommandBuffer> renderer_command_buffer(void);
void renderer_skip_note(int line);
void renderer_end_pass(void);
void renderer_td_flush_shader_textures(int stage, ShaderInfo *si);
void renderer_snapshots_invalidate(const char *why);
void renderer_capture_texture(NvnTexture *t, id<MTLTexture> tex);
void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);
id<MTLTexture> renderer_sampled_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_type);
id<MTLTexture> renderer_dummy_texture(int type);
id<MTLTexture> renderer_shadow_slot_texture(id<MTLTexture> tex, const ShaderTexture *st_, int i, uint64_t h);
MTLTextureType renderer_mtl_type_for(int t);
id<MTLSamplerState> renderer_sampler_for(NvnSamplerDesc *d);
id<MTLSamplerState> renderer_default_sampler(void);
int renderer_sampler_slots(int n, __strong id<MTLSamplerState> *smp, NvnSamplerDesc *const *desc, __strong id<MTLSamplerState> *slot, uint32_t *map);
void renderer_dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    renderer_snapshots_invalidate("compute");
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = renderer_compute_program(st.program);
    if (!cp) return;
    if (!cp->ready) { renderer_skip_note(2210); return; }
    static int trace = -1;
    if (trace < 0) trace = getenv("NXRT_TRACE_CS") != NULL;
#define CST(x) do { if (trace) { fprintf(stderr, "[cs] %s\n", x); fflush(stderr); } } while (0)
    CST("end_pass");
    renderer_end_pass();
    renderer_td_flush_shader_textures(5, cp->si);
    CST("encoder");
    id<MTLComputeCommandEncoder> ce = [renderer_command_buffer() computeCommandEncoder];
    CST("pipeline");
    [ce setComputePipelineState:cp->pipe];
    ShaderInfo *si = cp->si;
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        uint64_t off;
        if (k == 0) {
            uint32_t drv[0x500 / 4] = {0};  // tables end at 0x310 + 16 * 16
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                if (i < 18) { u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size; }
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
            off = renderer_ring_alloc(drv, sizeof drv);
        } else if (k == 1) {
            NvnShaderStage *ss = &st.program->stages[5];
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
            off = renderer_ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
        } else {
            int idx = k - cbuf_base;
            Range r = (idx >= 0 && idx < 18) ? st.ubuf[5][idx] : (Range){0, 0};
            uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
            if (r.addr) {
                if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
                if (size > r.size && r.size) size = r.size;
                if (size < 16) size = 16;
                off = renderer_ring_alloc((void *)(uintptr_t)r.addr, size);
            } else {
                static const uint8_t zeros[0x10000];
                off = renderer_ring_alloc(zeros, size);
            }
        }
        [ce setBuffer:renderer_ring_buffer() offset:off atIndex:k];
    }
    CST("gmem");
    if (si->uses_gmem) [ce setBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    CST("textures");
    bool cmapped = si->ntex > MSL_MAX_SAMPLERS;
    __strong id<MTLSamplerState> msmp[64];
    NvnSamplerDesc *mdesc[64];
    for (int i = 0; i < si->ntex; i++) {
        ShaderTexture *st_ = &si->tex[i];
        int slot = ((int)st_->handle_offset - 0x20) / 8;
        uint64_t h = (slot >= 0 && slot < 32) ? st.tex[5][slot] : 0;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
        tex_sc[0][i][0] = tex_sc[0][i][1] = 1;
        if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            if (e->tex) tex = renderer_sampled_texture(e->tex, &e->view, e->has_view, st_->type);
            if (e->tex && tex && g_debug_frame && g_capture_dir[0]) renderer_capture_texture(e->tex, tex);
            else if (e->dead_format) nvn_dead_slot_sampled(e, tic);
            if (e->tex && i < 64) { renderer_texture_scale(e->tex, &tex_sc[0][i][0], &tex_sc[0][i][1]); renderer_texture_fraction(e->tex, &tex_fr[0][i][0], &tex_fr[0][i][1]); }
            for (int k = 0; k < renderer_feedback_count(); k++)
                if (e->tex == renderer_feedback_texture(k)) {
                    id<MTLTexture> fc = renderer_feedback_copy(k);
                    MTLTextureType want = renderer_mtl_type_for(st_->type);
                    if (fc.textureType != want && want == MTLTextureType2DArray)
                        fc = [fc newTextureViewWithPixelFormat:fc.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
                    tex = fc;
                }
        }
        NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
        if (sd) smp = renderer_sampler_for(sd);
        if (!smp) smp = renderer_default_sampler();
        if (st_->shadow) tex = renderer_shadow_slot_texture(tex, st_, i, h);
        [ce setTexture:tex ? tex : renderer_dummy_texture(st_->type) atIndex:i];
        if (cmapped) { msmp[i] = smp; mdesc[i] = sd; }
        else [ce setSamplerState:smp atIndex:i];
    }
    if (cmapped) {  // (more textures than sampler slots: as bind_textures)
        __strong id<MTLSamplerState> slot[MSL_MAX_SAMPLERS];
        uint32_t map[64];
        int ns = renderer_sampler_slots(si->ntex, msmp, mdesc, slot, map);
        for (int k = 0; k < MSL_MAX_SAMPLERS; k++) [ce setSamplerState:k < ns ? slot[k] : renderer_default_sampler() atIndex:k];
        [ce setBytes:map length:(NSUInteger)si->ntex * 4 atIndex:SMAP_BUFFER_INDEX];
    }
    if (si->uses_rscale) {
        float rs[65][4] = {{1, 1, 0, 0}};
        for (int i = 0; i < si->ntex && i < 64; i++) {
            rs[1 + i][0] = tex_sc[0][i][0]; rs[1 + i][1] = tex_sc[0][i][1]; rs[1 + i][2] = tex_fr[0][i][0]; rs[1 + i][3] = tex_fr[0][i][1];
        }
        [ce setBytes:rs length:(NSUInteger)(1 + si->ntex) * 16 atIndex:RSCALE_BUFFER_INDEX];
    }
    CST("dispatch");
    extern bool g_debug_frame;
    if (g_debug_frame) {
        LOG("[dispatch] cs=%llx groups %ux%ux%u ssbo:", st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) LOG(" %d(%llx,%llu)", i, st.sbuf[5][i].addr, st.sbuf[5][i].size);
        LOG(" ubo:");
        for (int i = 0; i < 18; i++) if (st.ubuf[5][i].addr) LOG(" %d(%llx,%llu)", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
        LOG(" gmem %d\n", si->uses_gmem);
    }
    [ce dispatchThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerThreadgroup:MTLSizeMake(cp->local[0], cp->local[1], cp->local[2])];
    [ce endEncoding];
    CST("done");
    renderer_stat_dispatch_add();
}
