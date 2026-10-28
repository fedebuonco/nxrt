// Sampler state conversion and slot assignment (renderer_samplers.m).
#pragma once
#import <Metal/Metal.h>
#include "nvn.h"
#include "maxwell/shader.h"

id<MTLSamplerState> renderer_sampler_for(NvnSamplerDesc *d);
id<MTLSamplerState> renderer_default_sampler(void);
int renderer_sampler_slots(int n, __strong id<MTLSamplerState> *smp, NvnSamplerDesc *const *desc,
                           __strong id<MTLSamplerState> *slot, uint32_t *map);
