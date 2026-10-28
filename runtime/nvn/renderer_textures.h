// Texture views, their cache, and format-view compatibility (renderer_textures.m).
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include "nvn.h"
#include "maxwell/shader.h"

void renderer_views_forget(void *base);
void renderer_views_reset(void);
int renderer_views_live(void);
id<MTLTexture> renderer_view_cached(id<MTLTexture> base, uint64_t params, id<MTLTexture> (^make)(void));
bool renderer_view_needs_format_flag(MTLPixelFormat base, MTLPixelFormat view);
id<MTLTexture> renderer_view_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render,
                                     id<MTLBlitCommandEncoder> blit);
id<MTLTexture> renderer_dummy_texture(int type);
MTLTextureType renderer_mtl_type_for(int t);
id<MTLTexture> renderer_shadow_slot_texture(id<MTLTexture> tex, const ShaderTexture *st_, int i, uint64_t h);
id<MTLTexture> renderer_apply_swizzle(id<MTLTexture> tex, NvnTexture *t, const NvnTextureView *v, bool has_view);
id<MTLTexture> renderer_sampled_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_type);
