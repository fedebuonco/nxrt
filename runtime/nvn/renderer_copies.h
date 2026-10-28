// Texture copies (renderer_copies.m).
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
#include "nvn.h"

typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } RendererTexRef;

void renderer_copy_buffer_to_texture(uint64_t src, const RendererTexRef *dst);
void renderer_copy_texture_to_buffer(const RendererTexRef *src, uint64_t dst);
void renderer_copy_texture(const RendererTexRef *src, const RendererTexRef *dst, uint64_t flags);
