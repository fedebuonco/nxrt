// Metal 4 render-pipeline bases and per-draw specialisation (renderer_metal4.m).
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include "renderer_prog.h"

bool renderer_metal4_on(void);
id<MTLRenderPipelineState> renderer_metal4_pipeline(MTLRenderPipelineDescriptor *pd, Prog *pr, NSError **err);
