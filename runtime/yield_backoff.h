// Yield back-off policy: turn a busy yield loop into a short sleep after a run of yields. See kernel.c (SleepThread(0)).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Consecutive yields for at least start_ns: sleep an eighth of the run, clamped to 10 us .. cap_ns. 0: yield normally.
uint64_t yield_backoff_sleep_ns(uint64_t run_ns, uint64_t start_ns, uint64_t cap_ns);

// True when the caller's callee-saved registers (x19..x28) changed since the previous yield: the loop is doing work.
bool yield_backoff_progress(const uint64_t prev[10], const uint64_t now[10]);
