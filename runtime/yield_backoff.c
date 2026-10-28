// The policy half of the yield back-off (kernel.c): pure functions, so tests/test_yield.c can prove the timing.
#include "yield_backoff.h"

#include <string.h>

#define YIELD_MIN_NS 10000ull  // (an eighth of a short run is below this: never sleep less than 10 us)

uint64_t yield_backoff_sleep_ns(uint64_t run_ns, uint64_t start_ns, uint64_t cap_ns) {
    if (!start_ns || run_ns < start_ns) return 0;
    uint64_t sleep_ns = run_ns / 8;
    if (sleep_ns < YIELD_MIN_NS) sleep_ns = YIELD_MIN_NS;
    if (cap_ns && sleep_ns > cap_ns) sleep_ns = cap_ns;
    return sleep_ns;
}

bool yield_backoff_progress(const uint64_t prev[10], const uint64_t now[10]) {
    return memcmp(prev, now, 10 * sizeof *prev) != 0;
}
