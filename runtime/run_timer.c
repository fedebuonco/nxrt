// The run clock: real time from the start of an attempt to its end, split marks along the way, driven by the port.
// LiveSplit gets the same events through runtime/livesplit.c (off unless the setting is on).
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "livesplit.h"
#include "platform.h"
#include "rt.h"
#include "run_timer.h"
#include "settings.h"

static _Atomic int running, finished;
static _Atomic uint64_t started_ns, final_ms, last_sync_ns;

static void fmt_time(char *out, size_t n, uint64_t ms) {  // LiveSplit's h:mm:ss.fff
    snprintf(out, n, "%llu:%02llu:%02llu.%03llu", (unsigned long long)(ms / 3600000),
             (unsigned long long)((ms / 60000) % 60), (unsigned long long)((ms / 1000) % 60),
             (unsigned long long)(ms % 1000));
}

void nx_timer_start(void) {
    atomic_store(&started_ns, plat_now_ns());
    atomic_store(&final_ms, 0);
    atomic_store(&finished, 0);
    atomic_store(&last_sync_ns, 0);
    atomic_store(&running, 1);
    livesplit_poke();
    livesplit_cmd("initgametime");
    livesplit_cmd("starttimer");
    LOG("[timer] started\n");
}

void nx_timer_split(const char *label) {
    if (!atomic_load(&running)) return;
    uint64_t ms = nx_timer_ms();
    char t[32];
    fmt_time(t, sizeof t, ms);
    LOG("[timer] split%s%s at %s\n", label && *label ? " " : "", label && *label ? label : "", t);
    livesplit_cmd("split");
}

void nx_timer_stop(void) {
    if (!atomic_load(&running)) return;
    uint64_t ms = nx_timer_ms();
    atomic_store(&final_ms, ms);
    atomic_store(&running, 0);
    atomic_store(&finished, 1);
    char line[64], t[32];
    fmt_time(t, sizeof t, ms);
    snprintf(line, sizeof line, "setgametime %s", t);
    livesplit_cmd(line);
    livesplit_cmd("pausegametime");
    LOG("[timer] finished: %s\n", t);
}

uint64_t nx_timer_ms(void) {
    if (!atomic_load(&running)) return atomic_load(&final_ms);
    uint64_t s = atomic_load(&started_ns);
    return s ? (plat_now_ns() - s) / 1000000ull : 0;
}
bool nx_timer_running(void) { return atomic_load(&running) != 0; }
bool nx_timer_finished(void) { return atomic_load(&finished) != 0; }

void run_timer_livesplit_sync(void (*send)(const char *line, void *ctx), void *ctx) {
    if (!atomic_load(&running) && !atomic_load(&finished)) return;  // (no attempt yet)
    char t[32], line[64];
    send("initgametime", ctx);
    if (atomic_load(&running)) send("starttimer", ctx);
    fmt_time(t, sizeof t, nx_timer_ms());
    snprintf(line, sizeof line, "setgametime %s", t);
    send(line, ctx);
    if (!atomic_load(&running)) send("pausegametime", ctx);
}

void nx_timer_frame(uint64_t frame) {
    static int test = -1;
    if (test < 0) test = getenv("NXRT_TIMER_TEST") ? 1 : 0;
    if (test) {  // (a self-test run: start, three splits, stop, on presented frames)
        if (frame == 60) nx_timer_start();
        else if (frame == 120 || frame == 180 || frame == 240) nx_timer_split("test");
        else if (frame == 300) nx_timer_stop();
    }
    uint64_t now = plat_now_ns(), last = atomic_load(&last_sync_ns);
    if (atomic_load(&running) && (!last || now - last > 250000000ull)) {
        atomic_store(&last_sync_ns, now);
        char t[32], line[64];
        fmt_time(t, sizeof t, nx_timer_ms());
        snprintf(line, sizeof line, "setgametime %s", t);
        livesplit_cmd(line);
    }
}

RT_ON_RESET(run_timer_reset) {
    atomic_store(&running, 0);
    atomic_store(&finished, 0);
    atomic_store(&started_ns, 0);
    atomic_store(&final_ms, 0);
    atomic_store(&last_sync_ns, 0);
}
