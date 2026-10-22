#include "frame_rate.h"

#include "nxgame.h"
#include "settings.h"

int rt_frame_rate(void) { return NX_HOOK(max_frame_rate) >= 120 && g_settings.frame_rate == 120 ? 120 : 60; }
uint64_t rt_frame_period_ns(void) { return (1000000000ull + rt_frame_rate() / 2) / rt_frame_rate(); }
float rt_frame_scale(void) { return 60.0f / rt_frame_rate(); }
