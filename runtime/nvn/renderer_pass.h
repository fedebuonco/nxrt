// Render passes: geometry, deferred clears and begin/end (renderer_pass.m).
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>
#include "nvn.h"
#include "maxwell/shader.h"
#include "renderer_state.h"

void renderer_begin_pass(void);
void renderer_end_pass(void);
void renderer_stat_clear_pass(void);
void renderer_pass_size(int *w, int *h);
void renderer_apply_scissor(void);

typedef struct RendererTexDefer {
    NvnTexture *t;
    void *backend;
    bool clear_on, clear_color, clear_depth, clear_stencil;
    double cc[4], dval;
    uint32_t sval;
} RendererTexDefer;

bool renderer_td_defer_on(void);
int renderer_td_pending(void);
void renderer_td_pending_dec(void);
RendererTexDefer *renderer_td_get(NvnTexture *t, bool create);
void renderer_td_forget(NvnTexture *t);
void renderer_td_flush(NvnTexture *t);
void renderer_td_flush_targets(const RtState *rt);
void renderer_td_record(NvnTexture *t, bool is_color, int kind, const void *color, bool depth, float dval, bool stencil,
                        uint32_t sval);
void renderer_td_reset(void);
void renderer_texture_will_read(NvnTexture *t);
void renderer_td_flush_shader_textures(int stage, ShaderInfo *si);
