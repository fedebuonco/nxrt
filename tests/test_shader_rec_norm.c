// ShaderRec normalisation (runtime/nvn/shader_rec_norm.c).
#include "harness.h"
#include "shader_rec_norm.h"

int main(void) {
    ShaderRec r;
    memset(&r, 0, sizeof r);
    r.kind = 'G';
    r.ncolor = 3;
    r.color[0] = 0;          // no target 0: its mask/blend must go
    r.channel_mask = 0xF | (0xF << 8);
    r.blend_on = 1 | 2;      // bit 0 has no target; bit 1 blends
    memset(r.blend[0], 0x5a, 8);
    memset(r.blend[1], 0x33, 8);
    r.color[1] = 0x25;
    r.blend[1][1] = 1;
    r.color[2] = 0x25;       // target 2 but blending off: its factors go
    memset(r.blend[2], 0x44, 8);
    r.color[7] = 0x99;       // stale tail beyond ncolor

    shader_rec_normalize(&r);
    CHECK_EQ(r.color[0], 0);
    CHECK_EQ(r.channel_mask & 0xF, 0);
    CHECK(!(r.blend_on & 1));
    CHECK(r.blend[1][0] == 0x33 && r.blend[1][1] == 1);  // blending on: kept
    CHECK(r.blend[2][0] == 0);                            // blending off: gone
    CHECK_EQ(r.color[7], 0);                              // beyond ncolor: gone
    CHECK_EQ(r.color[1], 0x25);
    TEST_END();
}
