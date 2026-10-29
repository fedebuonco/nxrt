// Texture-identification hooks (LAS's item/cursor/bottle/heart cosmetics). The generic runtime keeps the seam; a game
// supplies the mapping. The defaults identify nothing and change nothing.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct NvnTexture NvnTexture;
enum { TEXPATCH_RECOLOUR_BASE = 1000000 };

int texpatch_marker(NvnTexture *t);
uint64_t texpatch_generation(void);
void texpatch_apply(int id, int level, uint8_t *blocks, int w, int h, int stride);
bool texpatch_is_item_cursor(int id);
int texpatch_bottle_kind(int id);
int texpatch_heart_kind(int id);
void texpatch_heart_tint(int kind, float tint[8]);
NvnTexture *texpatch_hint_icon(void);
