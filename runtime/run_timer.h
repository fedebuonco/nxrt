// The run clock (runtime/run_timer.c) and the LiveSplit Server client it drives (runtime/livesplit.c). A port starts
// an attempt when gameplay begins from the title, splits at its milestones and ends it when the run is over; the
// clock is real time and never stops (loads, transitions, the map and menus all count).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nx_timer_start(void);                 // a new attempt, from 0:00
void nx_timer_split(const char *label);    // a split now (NULL/"" for a plain split)
void nx_timer_stop(void);                  // the attempt is over: its time stays
uint64_t nx_timer_ms(void);                // the attempt's time (0: never started)
bool nx_timer_running(void);
bool nx_timer_finished(void);
// (presenter, once a presented frame) the periodic LiveSplit sync, and the NXRT_TIMER_TEST self-test.
void nx_timer_frame(uint64_t frame);

// (runtime/livesplit.c) bring a freshly connected LiveSplit up to date from the timer's state
void run_timer_livesplit_sync(void (*send)(const char *line, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
