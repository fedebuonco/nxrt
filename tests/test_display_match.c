#include <assert.h>
#include <stdio.h>

#include "display_match.h"

int main(void) {
    assert(display_hz_multiple(120, 60));
    assert(display_hz_multiple(59.94, 60));
    assert(display_hz_multiple(165, 55));
    assert(!display_hz_multiple(144, 60));
    assert(!display_hz_multiple(0, 60));
    assert(!display_hz_multiple(120, 0));

    DisplayModeInfo modes[] = {
        {.hz = 60, .width = 2560, .height = 1440, .pixel_width = 2560, .pixel_height = 1440, .usable = true},
        {.hz = 120, .width = 2560, .height = 1440, .pixel_width = 2560, .pixel_height = 1440, .usable = true},
        {.hz = 144, .width = 2560, .height = 1440, .pixel_width = 2560, .pixel_height = 1440, .usable = true},
        {.hz = 120, .width = 1920, .height = 1080, .pixel_width = 1920, .pixel_height = 1080, .usable = true},
        {.hz = 120, .width = 2560, .height = 1440, .pixel_width = 2560, .pixel_height = 1440, .usable = false},
    };
    DisplayModeInfo cur144 = modes[2];
    assert(display_pick_mode(modes, 5, &cur144, 60) == 1);  // 120, not 60 (highest multiple), not 144
    DisplayModeInfo cur120 = modes[1];
    assert(display_pick_mode(modes, 5, &cur120, 60) == 1);
    assert(display_pick_mode(modes, 5, &cur144, 50) == -1);  // no mode at a multiple of 50
    assert(display_pick_mode(modes, 5, &cur144, 48) == 2);  // 144 is a multiple of 48
    DisplayModeInfo none = {.width = 800, .height = 600, .pixel_width = 800, .pixel_height = 600, .usable = true};
    assert(display_pick_mode(modes, 5, &none, 60) == -1);
    assert(display_pick_mode(NULL, 0, &cur144, 60) == -1);

    puts("test_display_match: ok");
    return 0;
}
