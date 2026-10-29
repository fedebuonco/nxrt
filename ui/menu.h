// The in-game menu (nx-ui, Dear ImGui). Open with F1 / Esc, or Minus+Plus (Back+Start) on a controller. Its pages and
// overlays are registered through nx_ui.h; it draws through the host's backend (NxUiBackend).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Main (UI) thread: returns true if the event was consumed by the menu (the game must not see it).
bool menu_handle_event(const SDL_Event *e);
bool menu_is_open(void);
bool menu_input_captured(void);  // the menu or the name dialog has the input (the game gets none)
void menu_test_open(void);       // (tests) open it (NXRT_MENU_TAB: on that tab)
// Main thread: apply window requests (fullscreen) made from the menu.
void menu_apply_window_requests(void *sdl_window);

// Render thread: lay out and draw the menu, the name dialog and the overlays into the host's frame (the backend's
// new_frame / render get `frame`), after the game image. w, h: the frame's size in pixels. Nothing while all are off.
void menu_render(void *frame, double w, double h);
// true when menu_render would draw (the Vulkan backend drains the recorder before ImGui records)
bool menu_wants_render(void);

// The host's game is gone (an in-place reboot): menu and name dialog close.
void menu_reset(void);

// The name dialog (the game's software keyboard): open, wait (re-presenting frames) while the state is 1, close.
// state: 1 open, 2 confirmed, 3 cancelled. close: true = confirmed with text (in out).
void menu_keyboard_open(const char *initial, int max_len);
int menu_keyboard_state(void);
bool menu_keyboard_close(char *out, int cap);

// Render thread, inside a page: the Controls page's own rows, for rows others add there (nxui_add_rows) and pages
// that want to look the same
void menu_header(const char *text);
bool menu_toggle(const char *label, bool *v);
bool menu_choice(const char *label, int *value, const char *const *names, int n);
void menu_hint(const char *text);

#ifdef __cplusplus
}
void menu_tabs_draw(float scale, struct ImFont *font, struct ImFont *font_big, bool in_game, void (*prelaunch)(float));
void menu_tabs_focus(bool first_tab);
extern int g_tab_request_ext;
#endif
