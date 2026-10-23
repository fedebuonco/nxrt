// nx-input: the player's keyboard and controllers (SDL3), through their bindings (input_bind.h), as a Switch pad.
// The host feeds it SDL events (input_handle_event) and reads the pad when its game polls (nxin_read and the steps
// after it, in this order: the runtime's nn::hid, runtime/hle/hid.c, is the example).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;
typedef struct SDL_Window SDL_Window;

// The player 1 pad as a Switch game reads it: HidNpadButton bits (NX_BTN_*; 16-23 the stick directions), sticks
// -32767..32767 with up positive.
typedef struct NxPad {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
} NxPad;
enum {
    NX_BTN_A = 0, NX_BTN_B, NX_BTN_X, NX_BTN_Y, NX_BTN_LSTICK, NX_BTN_RSTICK, NX_BTN_L, NX_BTN_R, NX_BTN_ZL, NX_BTN_ZR,
    NX_BTN_PLUS, NX_BTN_MINUS, NX_BTN_LEFT, NX_BTN_UP, NX_BTN_RIGHT, NX_BTN_DOWN
};

enum { PROMPTS_AUTO = 0, PROMPTS_NINTENDO = 1, PROMPTS_XBOX = 2, PROMPTS_PLAYSTATION = 3, PROMPTS_KEYBOARD = 4 };
typedef struct NxInputSettings {
    bool dpad_move;  // the d-pad (and arrow keys) move the player like the stick
    int prompts;     // PROMPTS_*: button glyphs the game shows (auto = follow the controller SDL reports)
} NxInputSettings;
extern NxInputSettings g_input_settings;
extern int g_input_prompt_styles;  // the styles the host's game draws: 4 (auto .. PlayStation), 5 with Keyboard

void nxin_init(void);  // registers its settings section (nx_settings.h): before settings_load

// ---- events (main thread)
void input_handle_event(const SDL_Event *e);
bool input_user_blocked(const SDL_Event *e);  // a scripted test run: the user's own input is left out
void input_reset(void);                       // forget held keys (a menu took focus)
int input_pad_family(void);                   // 0 Nintendo / other / none, 1 Xbox, 2 PlayStation
extern volatile int g_debug_frame_request;    // F9 was pressed (the host's renderer: dump the next frame)

// ---- the mouse: buttons are bindable keys (bindings.c); captured (relative mode) its motion is the host's
void nxin_mouse_capture(SDL_Window *window, bool want);  // main thread
bool nxin_mouse_captured(void);
void nxin_mouse_take(int32_t *dx, int32_t *dy);  // the motion since the last call, 1/256 pixel units
bool nxin_mouse_button(int button);              // SDL_BUTTON_LEFT ... SDL_BUTTON_X2 held
bool nxin_mouse_bound(int button);               // that button is bound to a Switch input (then it is that input)
void nxin_set_stick_deadzone(int radius);        // controller sticks inside this radius read 0 (0 = none)

// ---- once per presented frame: test scripts (NXRT_INPUT, NXRT_HOST_INPUT, NXRT_BIND_TEST); script: the game's
// own steering of the scripted pad (NULL: none)
void nxin_frame(uint64_t frame, void (*script)(uint64_t frame, NxPad *p));

// ---- rumble: HD Rumble approximated with a two-motor gamepad (the runtime's HID vibration sends these; a port
// drives the pad's light through input_led_set). Called once per presented frame by the presenter.
typedef struct { float low_amp, low_hz, high_amp, high_hz; } RumbleValue;
void input_rumble_send(uint32_t handle, const RumbleValue *value);
RumbleValue input_rumble_get(uint32_t handle);
void input_rumble_permit(bool permitted);
bool input_rumble_permitted(void);
void input_rumble_poll(void);
void input_led_set(uint8_t r, uint8_t g, uint8_t b);
void input_led_poll(void);
void input_led_stop(void);

// ---- the pad (any thread). captured: a menu has the input (only test scripts reach the game)
void nxin_read(NxPad *p, bool captured);                     // 1. held buttons and sticks
void nxin_dpad_walk(NxPad *p, bool captured, bool mirrored); // 2. the d-pad moves the left stick (setting)
void nxin_stick_buttons(NxPad *p);                           // 3. stick directions as buttons 16-23
uint64_t input_buttons_now(bool captured);                   // held buttons, whether or not the game polls

#ifdef __cplusplus
}
#endif
