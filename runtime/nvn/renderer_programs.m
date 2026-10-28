// Program translation and the per-code pipeline/program cache (renderer_programs.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "renderer_cache.h"
#include "renderer_context.h"
#include "renderer_pipelines.h"
#include "renderer_prog.h"
#include "maxwell/shader.h"
#include "slowframe.h"
#include "renderer_programs.h"
#include "rt.h"

#define PROG_TAB 4096
static struct { uint64_t key; void *prog; } prog_tab[PROG_TAB];
Prog *renderer_prep_find_prog(uint64_t k);
void shader_first_visit(const char *kind, uint64_t vh, uint64_t fh, uint64_t ck);

static void dump_program(NvnProgram *p) {
    if (!getenv("NXRT_DUMP_SHADERS")) return;
    static int n;
    mkdir("build/shaders", 0755);
    for (int s = 0; s < 6; s++) {
        if (!(p->stage_mask & (1u << s))) continue;
        char path[256];
        snprintf(path, sizeof path, "build/shaders/%llx_s%d_code.bin", p->stages[s].code_addr, s);
        FILE *f = fopen(path, "wb");
        fwrite((void *)(uintptr_t)p->stages[s].code_addr, 1, 0x10000, f);
        fclose(f);
        snprintf(path, sizeof path, "build/shaders/%llx_s%d_ctrl.bin", p->stages[s].code_addr, s);
        f = fopen(path, "wb");
        fwrite(p->stages[s].control, 1, 0x4000, f);
        fclose(f);
    }
    n++;
}

bool renderer_translate_program(Prog *pr, const uint8_t *vcode, size_t vmax, const uint8_t *fcode, size_t fmax,
                              ShaderKey *vkey, ShaderKey *fkey, uint64_t vaddr, uint64_t faddr) {
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pr->vi = slow_translate(vcode, vmax, STAGE_VERTEX, vkey);
    if (!pr->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", pr->vi->error, vaddr);
        if (vaddr) {
            char path[128];
            snprintf(path, sizeof path, "build/shaders/fail_%llx_s0_code.bin", vaddr);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(vcode, 1, 0x4000, f); fclose(f); }
        }
        pr->failed = true;
        return false;
    }
    fkey->vs_out_mask = pr->vi->out_mask;
    pr->fi = slow_translate(fcode, fmax, STAGE_FRAGMENT, fkey);
    if (!pr->fi->msl) { LOG("[shader] FS translate failed: %s (code %llx)\n", pr->fi->error, faddr); pr->failed = true; return false; }
    if (getenv("NXRT_SAVE_MSL") && vaddr) {
        mkdir("build/msl", 0755);
        char path[256];
        snprintf(path, sizeof path, "build/msl/%llx_vs.metal", vaddr);
        FILE *f = fopen(path, "w"); fputs(pr->vi->msl, f); fclose(f);
        snprintf(path, sizeof path, "build/msl/%llx_fs.metal", faddr);
        f = fopen(path, "w"); fputs(pr->fi->msl, f); fclose(f);
    }
    return true;
}

uint64_t renderer_prog_content_key(uint64_t vh, uint64_t fh, const ShaderKey *vkey, const ShaderKey *fkey) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t c[2] = {vh, fh};
    const uint8_t *b = (const uint8_t *)c;
    for (size_t i = 0; i < sizeof c; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    b = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (fkey->buffer_tex) { h ^= fkey->buffer_tex; h *= 0x100000001B3ull; }  // (0: the key as before)
    return h;
}

static void prog_fp_check(Prog *pr, NvnProgram *p) {  // (the lookups below are by code address: shader_prep.h)
    shader_fp_check(&pr->fp[0], &p->stages[0], "vs");
    shader_fp_check(&pr->fp[1], &p->stages[1], "fs");
}

Prog *renderer_get_program(NvnProgram *p, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(p->stage_mask & 1) || !(p->stage_mask & 2)) return nil;
    // fast path: hash of the code addresses and keys
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = p->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) {
        Prog *fp = (__bridge Prog *)prog_tab[slot].prog;
        prog_fp_check(fp, p);
        return fp->failed ? nil : fp;
    }
    // (every byte of the keys, in hex: NSData's description shows only the first 16 and last 8 bytes of the 52-byte
    // key, so variants differing in attr_kind[16..], depth_minus_one, flip_y or a later field shared one entry -
    // whichever variant came first served the others)
    char kb[2 * (sizeof *vkey + 8) + 10], *kp = kb;  // (+ "b%08x": buffer_tex)
    for (size_t i = 0; i < sizeof *vkey; i++) kp += snprintf(kp, 3, "%02x", vb[i]);
    for (int i = 0; i < 8; i++) kp += snprintf(kp, 3, "%02x", fkey->rt_kind[i]);
    if (fkey->buffer_tex) kp += snprintf(kp, 10, "b%08x", fkey->buffer_tex);
    NSString *key = [NSString stringWithFormat:@"%llx/%llx/%s", p->stages[0].code_addr, p->stages[1].code_addr, kb];
    Prog *pr = renderer_cache_get(key);
    if (!pr) {  // prepared before the game (the same code and keys)
        uint64_t vh = shader_stage_hash(&p->stages[0]), fh = shader_stage_hash(&p->stages[1]), ck = renderer_prog_content_key(vh, fh, vkey, fkey);
        pr = vh && fh ? renderer_prep_find_prog(ck) : nil;
        if (pr) renderer_cache_set(key, pr);
        else shader_first_visit("program", vh, fh, ck);
    }
    if (pr) {
        prog_tab[slot].key = h;
        prog_tab[slot].prog = (__bridge void *)pr;
        prog_fp_check(pr, p);
        return pr->failed ? nil : pr;
    }
    pr = [Prog new];
    prog_fp_check(pr, p);  // (takes the fingerprint)
    renderer_cache_set(key, pr);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = (__bridge void *)pr;  // retained by the cache dictionary
    dump_program(p);
    {
        // NXRT_DBG_FS=<fs code addr hex>:<pc hex>:<reg>
        static int parsed;
        static unsigned long long dbg_addr;
        static unsigned dbg_pc;
        static int dbg_reg;
        if (!parsed) { parsed = 1; const char *e = getenv("NXRT_DBG_FS"); if (e) sscanf(e, "%llx:%x:%d", &dbg_addr, &dbg_pc, &dbg_reg); }
        maxwell_dbg_code = dbg_addr ? (const uint8_t *)(uintptr_t)dbg_addr : NULL;
        static unsigned long long dbg_vs;
        static int vparsed;
        if (!vparsed) { vparsed = 1; const char *e = getenv("NXRT_DBG_VS"); if (e) dbg_vs = strtoull(e, NULL, 16); }
        maxwell_dbg_vs = dbg_vs ? (const uint8_t *)(uintptr_t)dbg_vs : NULL;
        maxwell_dbg_pc = dbg_pc;
        maxwell_dbg_reg = dbg_reg;
    }
    // the compiler constants follow the code, so they bound the instruction scan
    size_t vmax = p->stages[0].const_offset ? p->stages[0].const_offset : 0x10000;
    size_t fmax = p->stages[1].const_offset ? p->stages[1].const_offset : 0x10000;
    if (!renderer_translate_program(pr, (const uint8_t *)(uintptr_t)p->stages[0].code_addr, vmax,
                           (const uint8_t *)(uintptr_t)p->stages[1].code_addr, fmax, vkey, fkey,
                           p->stages[0].code_addr, p->stages[1].code_addr))
        return nil;
    if (renderer_sync_shaders()) {
        renderer_compile_program(pr);
        return pr->failed ? nil : pr;
    }
    // First-time compiles take ~40 ms each on Metal: build off the render thread; draws using this program are
    // skipped until it's ready (the object appears a few frames later instead of the game stalling).
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("nxrt.shaders", DISPATCH_QUEUE_CONCURRENT);
    dispatch_async(q, ^{ renderer_compile_program(pr); });
    return pr;
}

void renderer_programs_reset(void) { memset(prog_tab, 0, sizeof prog_tab); }
