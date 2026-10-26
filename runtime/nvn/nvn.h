// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include "../hle.h"
#include "tegra.h"

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
// A guest NVN object stays the game's object: its vtable and fields are not ours. The host object behind it lives in
// this registry, keyed by the guest pointer. (Writing the host pointer at offset 0 used to overwrite a game vtable:
// Tomodachi's texture object then resolved a vtable through that host pointer.)
void *nvn_host_get(uint64_t guest);
void nvn_host_set(uint64_t guest, void *host);
#define HOST(T, guest) ((T *)nvn_host_get((uint64_t)(guest)))
#define SET_HOST(guest, p) nvn_host_set((uint64_t)(guest), (void *)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
} NvnPool;

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    NvnPool *pool;
    uint64_t guest_pool;  // the guest's NVNmemoryPool object (what nvnTextureGetMemoryPool returns)
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    uint32_t pool_refs;  // texture pool entries that name it (cleared when it is finalized: runtime/nvn/objects.c)
    bool format_views;   // named through a view in a format the backend can only give with a flag (Metal: PixelFormatView)
    char label[64];
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
} NvnSamplerDesc;

typedef struct {
    uint32_t setmask;  // which fields are overridden
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;  // a view as the renderer uses it (the guest's NVNtextureView holds an NvnGuestView)

// What nxrt keeps in the guest's NVNtextureView, opaque to the game: 16 bytes, as the view is 40 bytes in NVN 53 and 16
// in NVN 55 (Tomodachi Life keeps one in 16 bytes of its stack frame, its saved registers right after - clearing 40
// overwrote its frame pointer). Swizzles are 3 bits each (NVN_TEXTURE_SWIZZLE_ZERO..A: 0..5).
typedef struct {
    uint8_t setmask, target, ds_mode, pad;
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint16_t format, swizzle;
} NvnGuestView;
_Static_assert(sizeof(NvnGuestView) == 16, "NvnGuestView must fit NVN 55's NVNtextureView");

static inline void nvn_view_read(NvnTextureView *out, const void *guest) {
    const NvnGuestView *g = (const NvnGuestView *)guest;
    *out = (NvnTextureView){.setmask = g->setmask, .base_level = g->base_level, .num_levels = g->num_levels,
                            .min_layer = g->min_layer, .num_layers = g->num_layers, .format = g->format,
                            .target = g->target, .ds_mode = g->ds_mode};
    for (int i = 0; i < 4; i++) out->swizzle[i] = (g->swizzle >> (3 * i)) & 7;
}

// A view naming a texture (pool entry, render target, clear, copy): one in another format is told to the backend before
// the texture is used through it.
void backend_texture_view_format(NvnTexture *tex, uint32_t format);  // (runtime/nvn/metal.m)
static inline void nvn_texture_note_view(NvnTexture *t, const NvnTextureView *v) {
    if (t && v && (v->setmask & 4) && v->format != t->format) backend_texture_view_format(t, v->format);
}

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
    uint32_t dead_format;  // the texture registered here was finalized (NVN format + 1; 0 = no): tex is NULL
} NvnTexEntry;

// A draw named a texture pool slot whose texture was finalized (runtime/nvn/objects.c): logged, a few times
void nvn_dead_slot_sampled(const NvnTexEntry *e, uint32_t slot);

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
} NvnShaderStage;

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;

// ---- command recording (the records live in a host shadow of the game's command memory: cmdbuf.c)
typedef struct {
    uint64_t mem;    // current command memory chunk (guest address)
    uint64_t size;   // chunk size (guest bytes)
    uint64_t host;   // the chunk's shadow, where the records go
    uint64_t hcap;   // the shadow's size (size * CMD_SHADOW)
    uint64_t used;   // bytes used in the shadow
    uint64_t start;  // first record of the list being recorded
    uint64_t callback, callback_data;
    bool recording;
    uint64_t guest;  // the guest NVNcommandBuffer*
} NvnCmdBuf;

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint32_t flags;     // CMD_DEFERRED: extra[] is refilled from src when the list is translated
    uint32_t pad;
    uint64_t src;       // guest address a deferred bind's data is read from
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;
enum { CMD_DEFERRED = 1u };

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
