// Compute programs (renderer_compute.m).
#pragma once
#import <Metal/Metal.h>
#include <stdatomic.h>
#include "nvn.h"
#include "maxwell/shader.h"
#include "shader_prep.h"

typedef struct {
    id<MTLComputePipelineState> pipe;
    ShaderInfo *si;
    uint32_t local[3];
    uint64_t code_addr;
    bool failed;
    _Atomic bool ready;
    ShaderFp fp;
} CsProg;

uint64_t renderer_compute_content_key(uint64_t code_hash, const uint32_t local[3]);
bool renderer_compute_translate(CsProg *cp, const uint8_t *code, size_t max, uint32_t shared_bytes);
void renderer_compute_compile(CsProg *cp);
void renderer_dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz);
CsProg *renderer_compute_program(NvnProgram *p);
void renderer_compute_reset(void);
