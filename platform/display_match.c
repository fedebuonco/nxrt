#include "display_match.h"

#include <math.h>

bool display_hz_multiple(double hz, int fps) {
    if (hz <= 0 || fps <= 0) return false;
    double k = round(hz / fps);
    return k >= 1 && fabs(hz / fps - k) < 0.01;  // (59.94 Hz counts as 60)
}

int display_pick_mode(const DisplayModeInfo *modes, int n, const DisplayModeInfo *cur, int fps) {
    if (!modes || !cur || n <= 0) return -1;
    int best = -1;
    double best_hz = 0;
    for (int i = 0; i < n; i++) {
        const DisplayModeInfo *m = &modes[i];
        if (!m->usable || m->width != cur->width || m->height != cur->height ||
            m->pixel_width != cur->pixel_width || m->pixel_height != cur->pixel_height)
            continue;
        if (!display_hz_multiple(m->hz, fps) || m->hz <= best_hz) continue;
        best = i;
        best_hz = m->hz;
    }
    return best;
}
