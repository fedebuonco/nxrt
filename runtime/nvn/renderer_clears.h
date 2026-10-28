// The clear path (renderer_clears.m): clear quads, recorded clears and their pipeline formats.
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>
#include "shader_rec_norm.h"

void renderer_begin_pass(void);
void renderer_td_record(NvnTexture *t, bool is_color, int kind, const void *color, bool depth, float dval, bool stencil, uint32_t sval);
int renderer_debug_pass_no(void);
void renderer_clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil,
                    uint32_t sval, uint32_t smask);
void renderer_attachment_formats(MTLRenderPipelineDescriptor *pd);
void renderer_attachment_formats_rec(MTLRenderPipelineDescriptor *pd, const ShaderRec *r);
