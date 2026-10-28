// Host input handed to the UI/backend (metal.m), separate from the renderer API.
#pragma once
#include <stdbool.h>
#include <SDL3/SDL_events.h>

void input_handle_event(const SDL_Event *e);
void input_rumble_poll(void);
void input_rumble_stop(void);
void input_led_poll(void);  // controller light: UI thread, after input_rumble_poll
void input_led_stop(void);
bool input_user_blocked(const SDL_Event *e);
void input_cursor_event(const SDL_Event *e);  // hide the mouse cursor on pad / key play, show it on mouse use
// g_settings.bg_input -> SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS (before SDL_Init, and again when the setting
// changes: SDL reads the hint live)
void input_apply_background(void);
void input_load_user_mappings(void);  // after SDL_Init: the data root's gamecontrollerdb.txt, if there is one
