// Pipeline creation (renderer_pipelines.m): archive render/compute pipelines and program compilation.
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include "renderer_prog.h"

bool renderer_sync_shaders(void);  // NXRT_SYNC_SHADERS
id<MTLRenderPipelineState> renderer_archive_render_pipeline(MTLRenderPipelineDescriptor *pd, Prog *pr, NSError **err);
id<MTLComputePipelineState> renderer_archive_compute_pipeline(id<MTLFunction> f, NSError **err);
void renderer_compile_program(Prog *pr);
void renderer_note_compile(double ms);  // (renderer.m: stat_compile_ms / stat_compiles)
