// Choosing the display mode closest to the game's frame rate (display_macos.m; tests/test_display_match.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double hz;              // 0: the mode does not report a rate
    uint32_t width, height; // points
    uint32_t pixel_width, pixel_height;
    bool usable;            // CGDisplayModeIsUsableForDesktopGUI
} DisplayModeInfo;

bool display_hz_multiple(double hz, int fps);  // 59.94 counts as 60; 0/unknown is false
// The mode of the current mode's size with the highest rate that is a whole multiple of fps, or -1.
int display_pick_mode(const DisplayModeInfo *modes, int n, const DisplayModeInfo *cur, int fps);
