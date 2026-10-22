// The frame rate the game runs at (Video > Frame rate): 60, or 120 for games that support it (nx_game hooks
// max_frame_rate). The display's vsync (hle/vi.c) and the presenter pace to it; a game's own timing patches read it.
#pragma once
#include <stdint.h>

int rt_frame_rate(void);           // 60 or 120
uint64_t rt_frame_period_ns(void);
float rt_frame_scale(void);        // 60 / the frame rate (a 60 Hz step's share of a frame)
