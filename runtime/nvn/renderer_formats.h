// NVN format/state to Metal conversion (renderer_formats.m).
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>

int renderer_attr_kind(uint32_t fmt);
MTLVertexFormat renderer_vertex_format(uint32_t f);
bool renderer_is_int_format(uint32_t f);
MTLBlendFactor renderer_blend_factor(uint8_t f);
MTLBlendOperation renderer_blend_op(uint8_t e);
MTLCompareFunction renderer_compare_fn(uint8_t f);
MTLStencilOperation renderer_stencil_op(uint8_t o);
int renderer_rt_kind(uint32_t f);
