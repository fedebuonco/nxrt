// Shader texture lookup and debug capture (renderer_bind.m).
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
#include "nvn.h"
#include "maxwell/shader.h"
#include "renderer_prog.h"

NvnTexture *renderer_shader_texture_object(int stage, ShaderTexture *st_);
uint32_t renderer_buffer_tex_slots(int stage, ShaderInfo *si);
void renderer_capture_texture(NvnTexture *t, id<MTLTexture> tex);
bool renderer_is_attachment(NvnTexture *t);
void renderer_bind_textures(int stage, ShaderInfo *si);
void renderer_bind_cbufs(int stage, ShaderInfo *si);

typedef struct {
    NvnTexture *feedback_tex[4];
    id<MTLTexture> feedback_copy[4];
    int nfeedback;
    NSMutableDictionary *snap_copies;
    uint64_t cur_fs_hash;
    uint64_t feedback_copies;
} RendererBindings;

RendererBindings *renderer_bindings(void);
void renderer_feedback_resolve(Prog *pr);
void renderer_feedback_reset(void);
void renderer_feedback_forget(NvnTexture *t);
int renderer_feedback_count(void);
NvnTexture *renderer_feedback_texture(int i);
id<MTLTexture> renderer_feedback_copy(int i);
uint64_t renderer_feedback_copies(void);
uint64_t renderer_snap_copy_count(void);
uint64_t renderer_cur_fs_hash(void);
void renderer_set_cur_fs_hash(uint64_t h);
