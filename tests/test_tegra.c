// Tegra X1 texture layout (runtime/nvn/tegra.c, formats.c): the format table, storage sizes for known cases, the mip
// and layer rules, and swizzle -> deswizzle round trips at every level (a wrong GOB offset aliases texels, which a
// round trip catches). Builds with ASan: every write stays inside the layer's size.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "tegra.h"

static uint32_t rng_state = 0x2468ace0u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 17, rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_formats(void) {
    const FormatInfo *f = nvn_format_info(0x25);  // RGBA8
    CHECK_EQ(f->bpb, 4);
    CHECK_EQ(f->bw, 1);
    CHECK_EQ(f->bh, 1);
    CHECK_EQ(f->kind, FMT_COLOR);
    f = nvn_format_info(0x41);  // ASTC 4x4 (linear)
    CHECK_EQ(f->bpb, 8);
    CHECK_EQ(f->bw, 4);
    CHECK_EQ(f->bh, 4);
    CHECK_EQ(f->kind, FMT_COMPRESSED);
    f = nvn_format_info(0x79);  // ASTC 4x4 (sRGB)
    CHECK_EQ(f->bpb, 16);
    CHECK_EQ(f->bw, 4);
    CHECK_EQ(f->bh, 4);
    f = nvn_format_info(0x94);  // ASTC 12x12
    CHECK_EQ(f->bw, 12);
    CHECK_EQ(f->bh, 12);
    CHECK_EQ(nvn_format_info(0x31)->kind, FMT_STENCIL);
    CHECK_EQ(nvn_format_info(0x36)->kind, FMT_DEPTH_STENCIL);
    // Unknown / reserved formats fall back to an opaque 4-byte color format, never to a zero-sized block.
    f = nvn_format_info(0xFFFF);
    CHECK_EQ(f->bpb, 4);
    CHECK_EQ(f->bw, 1);
    CHECK_EQ(f->bh, 1);
    CHECK_EQ(f->kind, FMT_COLOR);
    f = nvn_format_info(0);  // (the table's hole: format 0 is unknown)
    CHECK_EQ(f->bpb, 4);
}

static TexLayout layout(uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels, uint32_t fmt) {
    TexLayout t = {w, h, d, layers, levels, nvn_format_info(fmt), 0, 0};
    tegra_layout_init(&t);
    return t;
}

static uint64_t raw_bytes(const TexLayout *t, uint32_t level) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint64_t wb = (w + t->fmt->bw - 1) / t->fmt->bw, hb = (h + t->fmt->bh - 1) / t->fmt->bh;
    return wb * hb * d * t->fmt->bpb;
}

static void check_layout(const TexLayout *t, uint64_t want_size, const char *what) {
    uint64_t total = tegra_total_size(t), layer = tegra_layer_size(t);
    if (want_size) {
        t_checks++;
        if (layer != want_size) {
            t_failures++;
            fprintf(stderr, "FAIL %s: layer size %llu != %llu\n", what, (unsigned long long)layer,
                    (unsigned long long)want_size);
        }
    }
    CHECK(total > 0);
    CHECK_EQ(total, layer * (t->layers ? t->layers : 1));
    CHECK_EQ(layer % 512, 0);  // every level is whole GOBs
    uint64_t sum = 0, prev_off = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        LevelLayout ll = tegra_level(t, l);
        CHECK(ll.offset >= prev_off);  // levels are packed in order
        CHECK_EQ(ll.offset % 512, 0);
        CHECK(ll.block_height_log2 <= t->block_height_log2);
        CHECK(ll.block_depth_log2 <= t->block_depth_log2);
        // The level's texel dimensions, with the usual floor at 1 for extra mips.
        uint32_t w = t->width >> l, h = t->height >> l, d = t->depth >> l;
        CHECK_EQ(ll.w, w ? w : 1);
        CHECK_EQ(ll.h, h ? h : 1);
        CHECK_EQ(ll.d, d ? d : 1);
        // A level's storage is whole GOBs and covers at least its uncompressed texels.
        CHECK(raw_bytes(t, l) <= layer);
        prev_off = ll.offset;
        sum = ll.offset;
    }
    // The last level's start plus its (unknown-sized) body is what layer_size covers: check the last start is inside.
    CHECK(sum < layer || layer == 0);
    // The whole layer covers level 0's texels.
    LevelLayout l0 = tegra_level(t, 0);
    CHECK(raw_bytes(t, 0) <= layer);
    (void)l0;
}

static void test_known_sizes(void) {
    // Cases whose exact block-linear size is known from the layout: whole GOBs, no padding.
    TexLayout a = layout(64, 64, 1, 1, 1, 0x25);  // RGBA8: 64x64 = 16 KB, exactly
    check_layout(&a, 16384, "RGBA8 64x64");
    TexLayout b = layout(128, 128, 1, 1, 1, 0x25);  // 128x128 = 64 KB, exactly
    check_layout(&b, 65536, "RGBA8 128x128");
    TexLayout c = layout(512, 512, 1, 1, 1, 0x33);  // D32: 1 MB, exactly
    check_layout(&c, 1048576, "D32 512x512");
    TexLayout d = layout(64, 64, 32, 1, 1, 0x25);  // 3D RGBA8: 512 KB, exactly
    check_layout(&d, 524288, "RGBA8 64x64x32");
    // A 1080p RGBA8 target: 1080 lines round up to 9 blocks of 128 rows.
    TexLayout e = layout(1920, 1080, 1, 1, 1, 0x25);
    check_layout(&e, 8847360, "RGBA8 1920x1080");
    // Two layers: each layer is padded to the level-0 block size (4096 here), and the total doubles.
    TexLayout f = layout(64, 64, 1, 2, 1, 0x25);
    check_layout(&f, 16384, "RGBA8 64x64 x2");
    CHECK_EQ(tegra_total_size(&f), 32768);
    // A full mip chain of 256x256 RGBA8: 256K + 64K + 16K + 4K + 1K = 341K.
    TexLayout g = layout(256, 256, 1, 1, 5, 0x25);
    check_layout(&g, 349184, "RGBA8 256x256, 5 levels");
    LevelLayout l4 = tegra_level(&g, 4);
    CHECK_EQ(l4.offset, 348160);
    CHECK_EQ(l4.w, 16);
    CHECK_EQ(l4.h, 16);
    // A 3D texture's block depth follows its depth.
    TexLayout h = layout(64, 64, 8, 1, 1, 0x25);
    CHECK_EQ(h.block_depth_log2, 3);
    // One texel: one GOB.
    TexLayout i = layout(1, 1, 1, 1, 1, 0x25);
    check_layout(&i, 512, "RGBA8 1x1");
    // Compressed: 64x64 ASTC 4x4 is 16 x 16 blocks of 16 bytes = 4 KB (4 GOBs wide, 2 GOBs tall).
    TexLayout j = layout(64, 64, 1, 1, 1, 0x79);
    check_layout(&j, 4096, "ASTC4x4 64x64");
}

static void roundtrip_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = tegra_level(t, level);
    uint64_t lin = (uint64_t)l.wb * t->fmt->bpb * l.hb * l.d;
    uint64_t swz = tegra_layer_size(t);
    if (lin > swz) {
        t_failures++;
        fprintf(stderr, "FAIL level %u: linear %llu > layer %llu\n", level, (unsigned long long)lin,
                (unsigned long long)swz);
        t_checks++;
        return;
    }
    uint8_t *a = malloc((size_t)(lin ? lin : 1)), *b = malloc((size_t)(lin ? lin : 1));
    uint8_t *s = malloc((size_t)(swz ? swz : 1)), *s2 = malloc((size_t)(swz ? swz : 1));
    for (uint64_t i = 0; i < lin; i++) a[i] = (uint8_t)rnd();
    memset(s, 0xCD, (size_t)swz);
    tegra_swizzle(a, s, l.wb, l.hb, l.d, t->fmt->bpb, l.block_height_log2, l.block_depth_log2);
    tegra_deswizzle(s, b, l.wb, l.hb, l.d, t->fmt->bpb, l.block_height_log2, l.block_depth_log2);
    CHECK_MEM(a, b, (size_t)lin);
    // The reverse direction too: a block-linear buffer deswizzled and swizzled back.
    memset(s2, 0xCD, (size_t)swz);
    tegra_swizzle(b, s2, l.wb, l.hb, l.d, t->fmt->bpb, l.block_height_log2, l.block_depth_log2);
    CHECK_MEM(s, s2, (size_t)swz);
    free(a), free(b), free(s), free(s2);
}

static void test_roundtrip(void) {
    static const struct {
        uint32_t w, h, d, layers, levels, fmt;
    } cases[] = {
        {1, 1, 1, 1, 1, 0x25},    {2, 3, 1, 1, 1, 0x25},       {15, 17, 1, 1, 1, 0x25},   {64, 64, 1, 1, 1, 0x25},
        {65, 63, 1, 1, 1, 0x25},  {1920, 1080, 1, 1, 1, 0x25}, {63, 65, 1, 1, 1, 0x19},   {65, 63, 1, 1, 1, 0x01},
        {33, 35, 1, 1, 1, 0x0A},  {64, 64, 1, 1, 5, 0x25},     {256, 256, 1, 1, 9, 0x25}, {64, 64, 8, 1, 1, 0x25},
        {64, 64, 32, 1, 1, 0x25}, {127, 129, 7, 1, 1, 0x25},   {64, 64, 1, 2, 1, 0x25},   {64, 64, 1, 6, 1, 0x25},
        {64, 64, 1, 1, 1, 0x79},  {100, 100, 1, 1, 1, 0x41},   {512, 512, 1, 1, 1, 0x33}, {512, 512, 1, 1, 1, 0x36},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        TexLayout t = layout(cases[i].w, cases[i].h, cases[i].d, cases[i].layers, cases[i].levels, cases[i].fmt);
        check_layout(&t, 0, "roundtrip case");
        for (uint32_t l = 0; l < t.levels; l++) roundtrip_level(&t, l);
    }
}

// The default block height must not exceed 16 GOBs, and must grow with the height.
static void test_block_height(void) {
    CHECK_EQ(tegra_default_block_height_log2(1), 0);
    CHECK_EQ(tegra_default_block_height_log2(8), 0);
    CHECK_EQ(tegra_default_block_height_log2(9), 1);
    CHECK_EQ(tegra_default_block_height_log2(16), 1);
    CHECK_EQ(tegra_default_block_height_log2(17), 2);
    CHECK_EQ(tegra_default_block_height_log2(33), 3);
    CHECK_EQ(tegra_default_block_height_log2(65), 4);
    CHECK_EQ(tegra_default_block_height_log2(1000), 4);  // capped at 16 GOBs
    for (uint32_t n = 1; n < 4096; n++) {
        uint32_t a = tegra_default_block_height_log2(n), b = tegra_default_block_height_log2(n + 1);
        CHECK(a <= 4);
        CHECK(b >= a);
    }
}

int main(void) {
    test_formats();
    test_known_sizes();
    test_roundtrip();
    test_block_height();
    TEST_END();
}
