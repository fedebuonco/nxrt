// Controller/keyboard rebinding (input/bindings.c): the default layout, name <-> slot tables, the capture that takes
// the next press, the swap on a conflict, and the settings-file lines. Needs SDL3 for scancode names (run.sh skips it
// when SDL3 isn't installed).
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "input_bind.h"

// bindings.c asks the input layer for the connected pad's family; there is no pad in this test.
int input_pad_family(void) {
    return 0;
}

static void test_defaults_and_names(void) {
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);
    CHECK(bind_is_default(BIND_DEV_PAD));
    CHECK(bind_is_default(BIND_DEV_KEY));
    for (int i = 0; i < BIND_PAD_COUNT; i++) CHECK(bind_is_default_slot(BIND_DEV_PAD, i));
    for (int i = 0; i < BIND_KEY_COUNT; i++) CHECK(bind_is_default_slot(BIND_DEV_KEY, i));
    // No duplicate bindings in the default layout.
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        for (int j = i + 1; j < BIND_PAD_COUNT; j++) CHECK(g_bind_pad[i] != g_bind_pad[j]);
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        for (int j = i + 1; j < BIND_KEY_COUNT; j++) CHECK(g_bind_key[i] != g_bind_key[j]);

    CHECK_EQ(bind_slot_from_name("a"), BIND_A);
    CHECK_EQ(bind_slot_from_name("zl"), BIND_ZL);
    CHECK_EQ(bind_slot_from_name("ls_up"), BIND_LS_UP);
    CHECK_EQ(bind_slot_from_name("rs_right"), BIND_RS_RIGHT);
    CHECK_EQ(bind_slot_from_name("bogus"), -1);
    CHECK_EQ(bind_pad_from_name("south"), SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK_EQ(bind_pad_from_name("ltrigger"), PADB_LTRIGGER);
    CHECK_EQ(bind_pad_from_name("rtrigger"), PADB_RTRIGGER);
    CHECK_EQ(bind_pad_from_name("bogus"), -1);
    CHECK_STR(bind_input_name(BIND_A), "A");
    CHECK_STR(bind_input_name(BIND_LS_UP), "Left stick Up");
    CHECK_STR(bind_input_name(-1), "?");
    CHECK_STR(bind_input_name(BIND_KEY_COUNT), "?");

    CHECK_STR(bind_pad_label(SDL_GAMEPAD_BUTTON_SOUTH, 0), "B");      // Nintendo layout by position
    CHECK_STR(bind_pad_label(SDL_GAMEPAD_BUTTON_SOUTH, 1), "A");      // Xbox
    CHECK_STR(bind_pad_label(SDL_GAMEPAD_BUTTON_SOUTH, 2), "Cross");  // PlayStation
    CHECK_STR(bind_pad_label(PADB_LTRIGGER, 0), "ZL");
    CHECK_STR(bind_pad_label(-1, 0), "(none)");
    CHECK_STR(bind_key_label(SDL_SCANCODE_A), SDL_GetScancodeName(SDL_SCANCODE_A));
    CHECK_STR(bind_key_label(512 + SDL_BUTTON_LEFT), "Left Click");
    CHECK_STR(bind_key_label(512 + SDL_BUTTON_MIDDLE), "Middle Click");
    CHECK_STR(bind_key_label(512 + SDL_BUTTON_RIGHT), "Right Click");
    CHECK_STR(bind_key_label(0), "(none)");
    CHECK_STR(bind_key_label(SDL_SCANCODE_COUNT), "(none)");
    CHECK(bind_prompt_host(BIND_A) >= 0);
    CHECK_EQ(bind_prompt_host(-1), -1);
}

static void test_settings(void) {
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);
    CHECK(bind_settings_load("bind_pad_a", "north"));
    CHECK_EQ(g_bind_pad[BIND_A], SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(!bind_is_default(BIND_DEV_PAD));
    CHECK(bind_settings_load("bind_pad_zl", "ltrigger"));
    CHECK_EQ(g_bind_pad[BIND_ZL], PADB_LTRIGGER);
    CHECK(bind_settings_load("bind_pad_b", "bogus"));  // (a bad value is ignored, the binding stays)
    CHECK_EQ(g_bind_pad[BIND_B], SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(bind_settings_load("bind_key_a", "Z"));
    CHECK_EQ(g_bind_key[BIND_A], SDL_SCANCODE_Z);
    CHECK(bind_settings_load("bind_key_b", "Left Click"));
    CHECK_EQ(g_bind_key[BIND_B], 512 + SDL_BUTTON_LEFT);
    CHECK(bind_settings_load("bind_key_x", "none"));
    CHECK_EQ(g_bind_key[BIND_X], 0);
    CHECK(bind_settings_load("bind_pad_bogus", "south"));  // (the section claims the prefix)
    CHECK(bind_settings_load("unrelated_key", "x") == false);

    // Save: only the lines that differ from the defaults, and they load back to the same state.
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (!f) return;
    bind_settings_save(f);
    rewind(f);
    char line[256];
    int nlines = 0;
    while (fgets(line, sizeof line, f)) nlines++;
    CHECK(nlines >= 4);
    rewind(f);
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);
    while (fgets(line, sizeof line, f)) {
        char key[128], val[128];
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%127s = %127[^\n]", key, val) == 2) CHECK(bind_settings_load(key, val));
    }
    fclose(f);
    CHECK_EQ(g_bind_pad[BIND_A], SDL_GAMEPAD_BUTTON_NORTH);
    CHECK_EQ(g_bind_pad[BIND_ZL], PADB_LTRIGGER);
    CHECK_EQ(g_bind_key[BIND_A], SDL_SCANCODE_Z);
    CHECK_EQ(g_bind_key[BIND_B], 512 + SDL_BUTTON_LEFT);
    CHECK_EQ(g_bind_key[BIND_X], 0);
}

static void test_capture(void) {
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);
    int dev = 99, slot = 99, ms = 99, other = 99, serial0 = 0, serial1 = 0;
    bind_last_result(NULL, NULL, NULL, &serial0);
    CHECK(!bind_capture_active(&dev, &slot, &ms));
    CHECK_EQ(dev, -1);

    // Capture A on the pad and press the button currently on B (south): the two swap.
    int old_a = g_bind_pad[BIND_A], old_b = g_bind_pad[BIND_B];
    bind_capture_begin(BIND_DEV_PAD, BIND_A);
    CHECK(bind_capture_active(&dev, &slot, &ms));
    CHECK_EQ(dev, BIND_DEV_PAD);
    CHECK_EQ(slot, BIND_A);
    SDL_Event e = {0};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = (Uint8)old_b;
    CHECK(bind_capture_event(&e));
    CHECK_EQ(bind_last_result(&dev, &slot, &other, &serial1), 2);
    CHECK_EQ(dev, BIND_DEV_PAD);
    CHECK_EQ(slot, BIND_A);
    CHECK_EQ(other, BIND_B);
    CHECK(serial1 > serial0);
    CHECK_EQ(g_bind_pad[BIND_A], old_b);
    CHECK_EQ(g_bind_pad[BIND_B], old_a);
    CHECK(!bind_capture_active(NULL, NULL, NULL));

    // Escape cancels a capture; a pad capture ignores keys other than Escape.
    bind_capture_begin(BIND_DEV_PAD, BIND_A);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_Z;
    CHECK(bind_capture_event(&e));  // consumed, not assigned
    CHECK(bind_capture_active(NULL, NULL, NULL));
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    CHECK(bind_capture_event(&e));
    CHECK_EQ(bind_last_result(&dev, &slot, &other, NULL), 3);
    CHECK(!bind_capture_active(NULL, NULL, NULL));

    // A key capture takes a key, and ignores a pad button.
    bind_capture_begin(BIND_DEV_KEY, BIND_A);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(bind_capture_event(&e));
    CHECK(bind_capture_active(NULL, NULL, NULL));
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_U;  // (not in the default layout: no swap)
    CHECK(bind_capture_event(&e));
    CHECK_EQ(bind_last_result(&dev, &slot, &other, NULL), 1);
    CHECK_EQ(g_bind_key[BIND_A], SDL_SCANCODE_U);

    // A mouse button is a key binding; a key capture already on F1 keeps it for the overlay.
    bind_capture_begin(BIND_DEV_KEY, BIND_B);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_RIGHT;
    CHECK(bind_capture_event(&e));
    CHECK_EQ(g_bind_key[BIND_B], 512 + SDL_BUTTON_RIGHT);
    bind_capture_begin(BIND_DEV_KEY, BIND_X);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_F1;
    CHECK(bind_capture_event(&e));
    CHECK(bind_capture_active(NULL, NULL, NULL));  // still capturing
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    CHECK(bind_capture_event(&e));
    CHECK(!bind_capture_active(NULL, NULL, NULL));

    // Invalid slots and a cancel with nothing capturing do nothing.
    int before = g_bind_pad[BIND_A];
    bind_capture_begin(BIND_DEV_PAD, -1);
    bind_capture_begin(BIND_DEV_PAD, BIND_PAD_COUNT);
    CHECK(!bind_capture_active(NULL, NULL, NULL));
    bind_capture_cancel();
    CHECK_EQ(g_bind_pad[BIND_A], before);
}

int main(void) {
    test_defaults_and_names();
    test_settings();
    test_capture();
    TEST_END();
}
