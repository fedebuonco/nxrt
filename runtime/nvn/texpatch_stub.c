// The generic (no game) texture-identification hooks. A game's tools/runtime can replace these with its own mapping.
#include <stddef.h>
#include <stdint.h>

#include "../texpatch.h"

int texpatch_marker(NvnTexture *t) { (void)t; return 0; }
uint64_t texpatch_generation(void) { return 0; }
void texpatch_apply(int id, int level, uint8_t *blocks, int w, int h, int stride) {
    (void)id, (void)level, (void)blocks, (void)w, (void)h, (void)stride;
}
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
int texpatch_bottle_kind(int id) { (void)id; return 0; }
int texpatch_heart_kind(int id) { (void)id; return 0; }
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind, (void)tint; }
NvnTexture *texpatch_hint_icon(void) { return NULL; }
