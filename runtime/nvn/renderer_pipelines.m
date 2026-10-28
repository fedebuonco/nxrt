// Pipeline creation: archive render/compute pipelines, compiled libraries and program compilation (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "renderer_archive.h"
#include "renderer_context.h"
#include "renderer_metal4.h"
#include "renderer_pipelines.h"
#include "renderer_prog.h"
#include "shader_source_key.h"
#include "slowframe.h"
#include "rt.h"
#include <time.h>

#define dev (renderer_context()->dev)
static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

// A render / compute pipeline through the archive: loaded from it when it's there, else compiled into it (saved
// later) and loaded. Any thread.
static id<MTLRenderPipelineState> archive_render_pipeline_(MTLRenderPipelineDescriptor *pd, Prog *pr, NSError **err) {
    if (renderer_metal4_on()) {
        id<MTLRenderPipelineState> ps = renderer_metal4_pipeline(pd, pr, err);
        if (ps) return ps;
    }
    renderer_archive_open();
    id<MTLBinaryArchive> archive = renderer_archive_get();
    if (!archive) return [dev newRenderPipelineStateWithDescriptor:pd error:err];
    pd.binaryArchives = @[archive];
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                    reflection:nil error:nil];
    if (ps) { renderer_archive_hit(); slow_note_cache_hit(); return ps; }
    bool added = false;  // (not while a save runs: it can take a while - this one goes in a later run)
    if (renderer_archive_try_lock()) {
        added = [archive addRenderPipelineFunctionsWithDescriptor:pd error:nil];
        renderer_archive_unlock();
    }
    if (added) renderer_archive_added();
    return [dev newRenderPipelineStateWithDescriptor:pd error:err];
}

id<MTLRenderPipelineState> renderer_archive_render_pipeline(MTLRenderPipelineDescriptor *pd, Prog *pr, NSError **err) {
    double t0 = now_ms();
    id<MTLRenderPipelineState> ps = archive_render_pipeline_(pd, pr, err);
    renderer_archive_pipeline_us((uint64_t)((now_ms() - t0) * 1000));
    return ps;
}

id<MTLComputePipelineState> renderer_archive_compute_pipeline(id<MTLFunction> f, NSError **err) {
    renderer_archive_open();
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = f;
    id<MTLBinaryArchive> archive = renderer_archive_get();
    if (!archive) return [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:err];
    cd.binaryArchives = @[archive];
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                      reflection:nil error:nil];
    if (ps) { renderer_archive_hit(); slow_note_cache_hit(); return ps; }
    bool added = false;  // (not while a save runs: it can take a while - this one goes in a later run)
    if (renderer_archive_try_lock()) {
        added = [archive addComputePipelineFunctionsWithDescriptor:cd error:nil];
        renderer_archive_unlock();
    }
    if (added) renderer_archive_added();
    return [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:err];
}

bool renderer_sync_shaders(void) {  // NXRT_SYNC_SHADERS: compile on first use, blocking (deterministic tests)
    static int v = -1;
    if (v < 0) v = getenv("NXRT_SYNC_SHADERS") != NULL;
    return v;
}

// A compiled Metal library from the setup cache: cache/shaders/<FNV-1a of the MSL>.metallib (tools/shader_libs.py --cache-dir).
// Loading it skips newLibraryWithSource - the ~300 s MSL compile when macOS's shader cache is cold. Absent: nil, and the
// caller compiles the source exactly as before.
// NXRT_MSL_DUMP=1: keep each translated MSL source in cache/msl/<hash>.metal, the input of the setup step
// (`tools/shader_libs.py cache/msl cache/shaders --cache-dir cache/shaders`). The runtime only writes them; compiling is
// the setup's job, so a player without the Metal toolchain still plays (the source path is the fallback).
static void dump_msl(const char *src) {
    static int on = -1;
    if (on < 0) on = getenv("NXRT_MSL_DUMP") != NULL;
    if (!on || !src) return;
    mkdir("cache", 0755);
    mkdir("cache/msl", 0755);
    char path[160];
    snprintf(path, sizeof path, "cache/msl/%016llx.metal", (unsigned long long)shader_source_key(src));
    if (access(path, R_OK) == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(src, f);
    fclose(f);
}

static id<MTLLibrary> compiled_library(const char *src) {
    char path[160];
    snprintf(path, sizeof path, "cache/shaders/%016llx.metallib", (unsigned long long)shader_source_key(src));
    if (access(path, R_OK) != 0) return nil;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] error:&err];
    static int hits;
    if (lib && __atomic_fetch_add(&hits, 1, __ATOMIC_RELAXED) < 5) LOG("[metal] compiled library: %s\n", path);
    if (!lib) LOG("[metal] compiled library %s: %s\n", path, err.localizedDescription.UTF8String);
    return lib;
}

// A function from the archive when it has one, else the library's own (the archive is optional).
static id<MTLFunction> archived_function(id<MTLBinaryArchive> ar, id<MTLLibrary> lib, NSString *name, NSError **err) {
    if (!ar) return [lib newFunctionWithName:name];
    MTLFunctionDescriptor *fd = [MTLFunctionDescriptor functionDescriptor];
    fd.name = name;
    fd.binaryArchives = @[ar];
    id<MTLFunction> f = [lib newFunctionWithDescriptor:fd error:err];
    return f ? f : [lib newFunctionWithName:name];
}

void renderer_compile_program(Prog *pr) {
    NSError *err = nil;
    renderer_archive_open();
    id<MTLBinaryArchive> ar = renderer_archive_get();
    dump_msl(pr->vi->msl);
    dump_msl(pr->fi->msl);
    static MTLCompileOptions *vopts;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ vopts = [MTLCompileOptions new]; vopts.preserveInvariance = YES; });
    double t0 = now_ms();
    // NXRT_SHADER_SALT=<text>: tag sources so Metal's disk cache misses (measure cold first-use compiles)
    NSString *salt = getenv("NXRT_SHADER_SALT") ? [NSString stringWithFormat:@"\n// %s\n", getenv("NXRT_SHADER_SALT")] : @"";
    id<MTLLibrary> vl = getenv("NXRT_SHADER_SALT") ? nil : compiled_library(pr->vi->msl);
    if (!vl) vl = [dev newLibraryWithSource:[@(pr->vi->msl) stringByAppendingString:salt] options:vopts error:&err];
    if (!vl) { LOG("[shader] VS compile failed: %s\n%s\n", err.localizedDescription.UTF8String, pr->vi->msl); pr->failed = true; return; }
    id<MTLLibrary> fl = getenv("NXRT_SHADER_SALT") ? nil : compiled_library(pr->fi->msl);
    if (!fl) fl = [dev newLibraryWithSource:[@(pr->fi->msl) stringByAppendingString:salt] options:nil error:&err];
    if (!fl) { LOG("[shader] FS compile failed: %s\n%s\n", err.localizedDescription.UTF8String, pr->fi->msl); pr->failed = true; return; }
    renderer_archive_library_us((uint64_t)((now_ms() - t0) * 1000));
    pr->vlib = vl;
    pr->flib = fl;
    pr->vs = archived_function(ar, vl, @"main0", &err);
    pr->fs = archived_function(ar, fl, @"main0", &err);
    if (ar && pr->vs && pr->fs) {  // archive the compiled functions too, not only the pipelines built from them
        MTLFunctionDescriptor *fd = [MTLFunctionDescriptor functionDescriptor];
        fd.name = @"main0";
        bool added = false;
        if (renderer_archive_try_lock()) {
            added |= [ar addFunctionWithDescriptor:fd library:vl error:nil] &&
                     [ar addFunctionWithDescriptor:fd library:fl error:nil];
            renderer_archive_unlock();
        }
        if (added) renderer_archive_added();
    }
    double dt = now_ms() - t0;
    renderer_note_compile(dt);
    slow_note_build("program", (uint64_t)(dt * 1e6));
    pr->ready = true;
}

