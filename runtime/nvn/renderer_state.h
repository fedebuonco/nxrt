// The renderer's bound guest state (split from renderer.m, held by RendererContext).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "nvn.h"

typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

// Same targets and views (field by field: the views come from guest records, their padding is not ours to compare).
static inline bool rt_same(const RtState *a, const RtState *b) {
    if (a->ncolor != b->ncolor || a->depth != b->depth || a->has_dview != b->has_dview) return false;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return false;
    for (int i = 0; i < a->ncolor; i++) {
        if (a->color[i] != b->color[i] || a->has_cview[i] != b->has_cview[i]) return false;
        if (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i])) return false;
    }
    return true;
}

typedef struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;   // what draws and dispatches use: the program bound for every stage, or a combined one
    NvnProgram *bound[6];  // the program bound for each stage (vertex, fragment, geometry, tess control / eval, compute)
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];  // factor, units, clamp (nvnCommandBufferSetPolygonOffsetClamp)
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} RendererState;
