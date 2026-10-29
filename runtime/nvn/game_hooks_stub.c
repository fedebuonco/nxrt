// LAS's game-specific hooks: the generic runtime keeps the call sites (and the per-game ABI), while a game that needs
// them overrides these. They do nothing for a plain recompilation.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void widescreen_set_factor(float f) { (void)f; }
bool widescreen_allowed(void) { return false; }
void widescreen_frame_end(uint64_t frame) { (void)frame; }
void widescreen_present_crop(uint64_t frame, double shape, float *crop, double *vx, double *vy, double *vw, double *vh) {
    (void)frame, (void)shape, (void)crop, (void)vx, (void)vy, (void)vw, (void)vh;
}
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }
bool widescreen_ui_pane_partial(const float *u, uint64_t size) { (void)u, (void)size; return false; }
float widescreen_effect_stretch(uint64_t hash) { (void)hash; return 0; }
bool mirror_draw(bool composite) { (void)composite; return false; }
bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    (void)sw, (void)sh, (void)w, (void)h;
    return false;
}
void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    (void)composite, (void)sw, (void)sh, (void)w, (void)h;
}
bool pot_level_active(void) { return false; }
bool pot_glow_active(void) { return false; }
bool pot_tint(const float *m, float tint[8], uint64_t mesh) { (void)m, (void)tint, (void)mesh; return false; }
void pot_gold_tint(float tint[8]) { (void)tint; }
bool pot_instance_gold(const float *m) { (void)m; return false; }
bool pets_hide_item_cursor(void) { return false; }
void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) {
    (void)kind, (void)ui, (void)ui_size, (void)tint;
}
bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size) { (void)ui, (void)ui_size; return false; }
const char *game_level_name(void) { return ""; }
void save_editor_frame(uint64_t frame) { (void)frame; }
void warp_frame(uint64_t frame) { (void)frame; }
