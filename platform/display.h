// The macOS display refresh match (C2): in fullscreen, a screen whose rate is not a multiple of the game's frame rate
// switches to a mode that is (144 -> 120Hz for 60fps), for this app only. A window keeps the screen's rate.
#pragma once
#include <stdbool.h>

void plat_display_bind(void *nswindow);  // NSWindow, main thread (after ui_window_init)
void plat_display_tick(int fps, bool fullscreen);  // cheap; applies on the main thread when the state changes
void plat_display_restore(void);
