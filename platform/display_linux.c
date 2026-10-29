// Linux display refresh matching: the compositor owns the mode (Gamescope on Deck), so nxrt never changes it.
#include "display.h"

void plat_display_bind(void *window) { (void)window; }
void plat_display_tick(int fps, bool fullscreen) {
    (void)fps, (void)fullscreen;
}
void plat_display_restore(void) {}
