#include "shader_rec_norm.h"

#include <string.h>

void shader_rec_normalize(ShaderRec *r) {
    if (!r) return;
    for (int i = 0; i < 8; i++) {
        bool has_color = (i < r->ncolor && r->color[i] != 0);
        if (!has_color) {
            r->color[i] = 0;
            r->cview[i] = 0;
            r->channel_mask &= ~(0xFu << (i * 4));
            r->blend_on &= (uint8_t)~(1u << i);
            memset(r->blend[i], 0, sizeof r->blend[i]);
            continue;
        }
        if (!(r->blend_on & (1u << i))) memset(r->blend[i], 0, sizeof r->blend[i]);  // blending off: factors unused
    }
    for (int i = r->ncolor; i < 8; i++) {  // (a record that left the tail of color[] stale)
        r->color[i] = 0;
        r->cview[i] = 0;
        r->channel_mask &= ~(0xFu << (i * 4));
        r->blend_on &= (uint8_t)~(1u << i);
        memset(r->blend[i], 0, sizeof r->blend[i]);
    }
}
