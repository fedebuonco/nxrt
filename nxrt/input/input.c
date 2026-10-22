// nx-input: the player's controllers and keyboard (SDL3) through their bindings (bindings.c, defaults there: keyboard
// WASD left stick, arrows d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad
// 8/4/5/6 right stick; controller by position, Switch A = east), as a Switch pad's state (NxPad: nx_input.h), and the
// scripted input of test runs. Nothing here knows the guest: the host hands the pad to the game (runtime/hle/hid.c).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "input_bind.h"
#include "nx_app.h"
#include "nx_input.h"
#include "nx_settings.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

_Static_assert(NX_BTN_DOWN == BIND_DOWN && NX_BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;

// Host keys / buttons held by a test script (NXRT_HOST_INPUT, nxin_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in NXRT_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

// The mouse: its buttons are keys to the bindings (512 + SDL button: "Left Click" ...), its motion while captured
// (relative mode, nxin_mouse_capture) is the host's to use (nxin_mouse_take: e.g. a camera)
static _Atomic bool mouse_btn[6];
static _Atomic bool mouse_captured;
static bool mouse_skip_motion;
static _Atomic int32_t mouse_dx, mouse_dy;  // (1/256 pixel)
static int stick_deadzone;                  // (nxin_set_stick_deadzone)

static int held_key(int sc) {
    if (sc > 512 && sc <= 512 + 5) return atomic_load(&mouse_btn[sc - 512]);
    return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]);
}

static void mouse_add_motion(_Atomic int32_t *axis, float delta) {
    if (!isfinite(delta)) return;
    int32_t add = (int32_t)(fmaxf(-8192, fminf(8192, delta)) * 256), old = atomic_load(axis), next;
    do { next = old + add; next = next < -8388352 ? -8388352 : next > 8388352 ? 8388352 : next; }
    while (!atomic_compare_exchange_weak(axis, &old, next));
}

bool nxin_mouse_button(int button) { return button >= 1 && button <= 5 && atomic_load(&mouse_btn[button]); }
bool nxin_mouse_bound(int button) {
    for (int i = 0; i < BIND_KEY_COUNT; i++) if (g_bind_key[i] == 512 + button) return true;
    return false;
}
bool nxin_mouse_captured(void) { return atomic_load(&mouse_captured); }
void nxin_mouse_take(int32_t *dx, int32_t *dy) { *dx = atomic_exchange(&mouse_dx, 0), *dy = atomic_exchange(&mouse_dy, 0); }
void nxin_set_stick_deadzone(int radius) { stick_deadzone = radius; }

static void deadzone(int32_t *x, int32_t *y) {
    if (stick_deadzone && (int64_t)*x * *x + (int64_t)*y * *y <= (int64_t)stick_deadzone * stick_deadzone) *x = *y = 0;
}

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    int32_t lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = -(int32_t)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    int32_t rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), ry = -(int32_t)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    deadzone(&lx, &ly);  // (a resting stick's drift: the host's dead zone, nxin_set_stick_deadzone)
    deadzone(&rx, &ry);
    pad_lx = lx, pad_ly = ly, pad_rx = rx, pad_ry = ry;
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    for (int i = 0; i < 6; i++) atomic_store(&mouse_btn[i], false);
    mouse_dx = mouse_dy = 0;
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log

// Events a test pushes (NXRT_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// NXRT_NO_USER_INPUT=1, on by default whenever NXRT_INPUT / NXRT_DOORJUMP_TEST / NXRT_PLACE is set
// (or NXRT_HOST_INPUT / NXRT_BIND_TEST; NXRT_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("NXRT_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("NXRT_INPUT") || getenv("NXRT_DOORJUMP_TEST") || getenv("NXRT_PLACE") ||
                                 getenv("NXRT_HOST_INPUT") || getenv("NXRT_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (NXRT_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (the game restyles them): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// NXRT_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(void) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        default: break;
    }
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("NXRT_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// The mouse in relative mode (captured for the game: hidden, its motion as deltas) or not
void nxin_mouse_capture(SDL_Window *w, bool want) {
    if (want == atomic_load(&mouse_captured)) return;
    if (!SDL_SetWindowRelativeMouseMode(w, want)) {
        LOG("[input] relative mouse mode: %s\n", SDL_GetError());
        return;
    }
    mouse_captured = want;
    mouse_skip_motion = want;  // (the event that captured it may still carry an absolute-mode delta)
    input_reset();
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        recompute_keyboard();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_reset();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_captured) {
            if (mouse_skip_motion) { mouse_skip_motion = false; break; }
            mouse_add_motion(&mouse_dx, e->motion.xrel);
            mouse_add_motion(&mouse_dy, e->motion.yrel);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button >= 1 && e->button.button <= 5) {
            atomic_store(&mouse_btn[e->button.button], e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            recompute_keyboard();
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) { pad = SDL_OpenGamepad(e->gdevice.which); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) { SDL_CloseGamepad(pad); pad = NULL; note_family(); }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        recompute_pad();
        break;
    }
}

// Scripted input for testing: NXRT_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// NXRT_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in the settings file's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// NXRT_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("NXRT_HOST_INPUT"); bspec = getenv("NXRT_BIND_TEST"); host_test = hspec; }
    static char live_spec[16384];  // NXRT_INPUT_FILE: the same script in a file, read every frame (edit it live)
    const char *live = getenv("NXRT_INPUT_FILE");
    if (live) {
        FILE *f = fopen(live, "rb");
        size_t n = f ? fread(live_spec, 1, sizeof live_spec - 1, f) : 0;
        if (f) fclose(f);
        live_spec[n] = 0;
        hspec = live_spec, host_test = true;
    }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}


// ------------------------------------------------------------------ the pad, for the host
NxInputSettings g_input_settings;
int g_input_prompt_styles = 4;

void nxin_frame(uint64_t frame, void (*script)(uint64_t frame, NxPad *p)) {
    host_test_frame(frame);
    const char *spec = getenv("NXRT_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    if (script) {  // the game's own test scripts steering the pad
        NxPad p = {b, lx, ly, rx, ry};
        script(frame, &p);
        b = p.buttons, lx = p.lx, ly = p.ly, rx = p.rx, ry = p.ry;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(bool captured) { return (captured ? 0 : key_buttons | pad_buttons) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

void nxin_read(NxPad *p, bool captured) {
    p->buttons = captured ? script_buttons : key_buttons | pad_buttons | script_buttons;
    p->lx = clamp_axis((captured ? 0 : key_lx + pad_lx) + script_lx), p->ly = clamp_axis((captured ? 0 : key_ly + pad_ly) + script_ly);
    p->rx = clamp_axis((captured ? 0 : key_rx + pad_rx) + script_rx), p->ry = clamp_axis((captured ? 0 : key_ry + pad_ry) + script_ry);
}

// d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick never
// reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
void nxin_dpad_walk(NxPad *p, bool captured, bool mirrored) {
    if (captured || !g_input_settings.dpad_move || (int64_t)p->lx * p->lx + (int64_t)p->ly * p->ly >= 8000ll * 8000) return;
    uint64_t d = key_buttons | pad_buttons | script_buttons;
    int dx = (int)((d >> NX_BTN_RIGHT) & 1) - (int)((d >> NX_BTN_LEFT) & 1);
    if (mirrored) dx = -dx;
    int dy = (int)((d >> NX_BTN_UP) & 1) - (int)((d >> NX_BTN_DOWN) & 1);
    int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
    if (dx || dy) p->lx = dx * m, p->ly = dy * m;
}

// stick directions also appear as buttons (bits 16-23), as nn::hid reports them
void nxin_stick_buttons(NxPad *p) {
    if (p->lx < -16000) p->buttons |= 1ull << 16;
    if (p->ly > 16000) p->buttons |= 1ull << 17;
    if (p->lx > 16000) p->buttons |= 1ull << 18;
    if (p->ly < -16000) p->buttons |= 1ull << 19;
    if (p->rx < -16000) p->buttons |= 1ull << 20;
    if (p->ry > 16000) p->buttons |= 1ull << 21;
    if (p->rx > 16000) p->buttons |= 1ull << 22;
    if (p->ry < -16000) p->buttons |= 1ull << 23;
}

// ------------------------------------------------------------------ settings (nx_settings.h)
static bool input_key(const char *key, const char *val) {
    if (!strcmp(key, "dpad_move")) g_input_settings.dpad_move = atoi(val) != 0;
    else if (!strcmp(key, "prompts")) g_input_settings.prompts = !strcmp(val, "nintendo") ? PROMPTS_NINTENDO : !strcmp(val, "xbox") ?
                                                                 PROMPTS_XBOX : !strcmp(val, "playstation") ? PROMPTS_PLAYSTATION :
                                                                 !strcmp(val, "keyboard") ? PROMPTS_KEYBOARD : PROMPTS_AUTO;
    else return bind_settings_load(key, val);  // bind_pad_* / bind_key_*: bindings.c
    return true;
}
static void input_write(FILE *f) {
    fprintf(f, "dpad_move = %d\n", g_input_settings.dpad_move);
    static const char *const prompts[] = {"auto", "nintendo", "xbox", "playstation", "keyboard"};
    fprintf(f, "prompts = %s\n", g_input_settings.prompts >= 0 && g_input_settings.prompts < 5 ? prompts[g_input_settings.prompts] : "auto");
    bind_settings_save(f);
}

void nxin_init(void) {
    static const NxSettingsSection s = {"input", NULL, input_key, NULL, input_write};
    nx_settings_register(&s);
}
