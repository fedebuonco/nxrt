// Hooks the host UI supplies for controller behaviour the runtime only observes. Linux/Deck defaults: no background
// filtering, no cursor auto-hide, no rumble stop (the SDL input layer owns those on a host that implements them).
#include "input_host.h"

void input_rumble_stop(void) {}
void input_cursor_event(const SDL_Event *e) { (void)e; }
void input_apply_background(void) {}
