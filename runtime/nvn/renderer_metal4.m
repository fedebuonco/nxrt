// Metal 4 bases and per-draw specialisation (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>

#include "renderer_metal4.h"
#include "renderer_context.h"
#include "rt.h"

// ------------------------------------------------------------------ Metal 4 bases and specialisation
// NXRT_METAL4=1 and macOS 26+: one unspecialised render pipeline per program/vertex layout, then a microsecond
// specialisation per colour state (nxnative's base + specialise). Metal 4 pipelines work with the ordinary
// MTLRenderCommandEncoder (verified by tests/manual/test_mtl4.m). Any failure falls back to the Metal 3 path.
static id<MTL4Compiler> g_m4;
static int g_m4_state = -1;
static NSMutableDictionary<NSString *, id> *g_m4_bases;
static dispatch_once_t g_m4_once;

static void metal4_init(void) {
    const char *e = getenv("NXRT_METAL4");
    g_m4_state = e && atoi(e) ? 1 : 0;
    if (!g_m4_state) return;
    NSError *err = nil;
    g_m4 = [renderer_context()->dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    if (!g_m4) {
        LOG("[metal4] no compiler: %s\n", err.localizedDescription.UTF8String);
        g_m4_state = 0;
        return;
    }
    g_m4_bases = [NSMutableDictionary new];
}

bool renderer_metal4_on(void) {
    dispatch_once(&g_m4_once, ^{ metal4_init(); });  // (pipeline builds run on several queues)
    return g_m4_state == 1;
}

static NSString *pipeline_base_key(MTLRenderPipelineDescriptor *pd, Prog *pr) {
    NSMutableString *k = [NSMutableString stringWithFormat:@"%p/%p/%lu/%d", pr->vs, pr->fs,
                          (unsigned long)pd.rasterSampleCount, pd.alphaToCoverageEnabled];
    MTLVertexDescriptor *vd = pd.vertexDescriptor;
    if (pr->vi)
        for (int i = 0; i < 32; i++)
            if (pr->vi->attr_mask & (1u << i))
                [k appendFormat:@" a%d:%lu/%lu/%lu", i, (unsigned long)vd.attributes[i].format,
                 (unsigned long)vd.attributes[i].offset, (unsigned long)vd.attributes[i].bufferIndex];
    for (int s = 18; s < 31; s++)
        [k appendFormat:@" l%lu/%lu/%lu", (unsigned long)vd.layouts[s].stride, (unsigned long)vd.layouts[s].stepFunction,
         (unsigned long)vd.layouts[s].stepRate];
    for (int i = 0; i < 8; i++) [k appendFormat:@" c%lu", (unsigned long)pd.colorAttachments[i].pixelFormat];
    return k;
}

id<MTLRenderPipelineState> renderer_metal4_pipeline(MTLRenderPipelineDescriptor *pd, Prog *pr, NSError **err) {
    if (!renderer_metal4_on() || !pr->vlib || !pr->flib) return nil;
    // Start with the states the M4 path has been validated for: one colour target, no depth/stencil, single-sample.
    // Anything else goes through the Metal 3 path (FE3H gameplay faults the GPU with a depth/MRT M4 pipeline).
    if (pd.depthAttachmentPixelFormat != MTLPixelFormatInvalid || pd.stencilAttachmentPixelFormat != MTLPixelFormatInvalid)
        return nil;
    int colors = 0;
    for (int i = 0; i < 8; i++) {
        if (pd.colorAttachments[i].pixelFormat == MTLPixelFormatInvalid) continue;
        colors++;
        if (pd.colorAttachments[i].blendingEnabled) return nil;  // (the M4 blend state is not right yet: FE3H's menu text gets opaque black quads)
    }
    if (colors > 1 || pd.rasterSampleCount > 1) return nil;
    NSString *bk = pipeline_base_key(pd, pr);
    id base = nil;
    @synchronized(g_m4_bases) {  // (draws and the preparation build in parallel)
        base = g_m4_bases[bk];
        if (!base) {
        MTL4RenderPipelineDescriptor *bd = [MTL4RenderPipelineDescriptor new];
        MTL4LibraryFunctionDescriptor *vf = [MTL4LibraryFunctionDescriptor new];
        vf.name = @"main0";
        vf.library = pr->vlib;
        MTL4LibraryFunctionDescriptor *ff = [MTL4LibraryFunctionDescriptor new];
        ff.name = @"main0";
        ff.library = pr->flib;
        bd.vertexFunctionDescriptor = vf;
        bd.fragmentFunctionDescriptor = ff;
        bd.vertexDescriptor = pd.vertexDescriptor;
        bd.rasterSampleCount = pd.rasterSampleCount ? pd.rasterSampleCount : 1;
        bd.alphaToCoverageState = pd.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
        for (int i = 0; i < 8; i++)
            if (pd.colorAttachments[i].pixelFormat != MTLPixelFormatInvalid) {
                bd.colorAttachments[i].pixelFormat = pd.colorAttachments[i].pixelFormat;  // format is in the base
                bd.colorAttachments[i].blendingState = MTL4BlendStateUnspecialized;
            }
        base = [g_m4 newRenderPipelineStateWithDescriptor:bd dynamicLinkingDescriptor:nil compilerTaskOptions:nil error:err];
        if (base) g_m4_bases[bk] = base;
        }
    }
    if (!base) return nil;
    MTL4RenderPipelineDescriptor *sd = [MTL4RenderPipelineDescriptor new];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *src = pd.colorAttachments[i];
        if (src.pixelFormat == MTLPixelFormatInvalid) continue;
        MTL4RenderPipelineColorAttachmentDescriptor *dst = sd.colorAttachments[i];
        dst.pixelFormat = src.pixelFormat;
        dst.writeMask = src.writeMask;
        dst.blendingState = src.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
        if (src.blendingEnabled) {
            dst.sourceRGBBlendFactor = src.sourceRGBBlendFactor;
            dst.destinationRGBBlendFactor = src.destinationRGBBlendFactor;
            dst.sourceAlphaBlendFactor = src.sourceAlphaBlendFactor;
            dst.destinationAlphaBlendFactor = src.destinationAlphaBlendFactor;
            dst.rgbBlendOperation = src.rgbBlendOperation;
            dst.alphaBlendOperation = src.alphaBlendOperation;
        }
    }
    NSError *serr = nil;
    id<MTLRenderPipelineState> spec = [g_m4 newRenderPipelineStateBySpecializationWithDescriptor:sd pipeline:base error:&serr];
    static int logged;
    if (getenv("NXRT_METAL4_LOG") && logged < 8) {
        logged++;
        unsigned f0 = (unsigned)pd.colorAttachments[0].pixelFormat, f1 = (unsigned)pd.colorAttachments[1].pixelFormat;
        LOG("[metal4] base %s: %s; spec: %s; fmt %u/%u mask %lx blend %d\n", bk.UTF8String, base ? "ok" : "nil",
            spec ? "ok" : (serr ? serr.localizedDescription.UTF8String : "nil"), f0, f1,
            (unsigned long)pd.colorAttachments[0].writeMask, pd.colorAttachments[0].blendingEnabled);
    }
    if (serr && err) *err = serr;
    return spec;
}

