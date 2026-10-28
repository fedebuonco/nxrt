// The yield back-off policy (runtime/yield_backoff.c): the timing rule and the progress check.
#include "harness.h"
#include "yield_backoff.h"

int main(void) {
    // Defaults (start 2 ms, cap 250 us): no sleep before the run, 250 us at 2 ms, capped afterwards.
    CHECK_EQ(yield_backoff_sleep_ns(0, 2000000, 250000), 0);
    CHECK_EQ(yield_backoff_sleep_ns(1999999, 2000000, 250000), 0);
    CHECK_EQ(yield_backoff_sleep_ns(2000000, 2000000, 250000), 250000);
    CHECK_EQ(yield_backoff_sleep_ns(8000000, 2000000, 250000), 250000);

    // An eighth of the run, before the cap (8 ms -> 1 ms).
    CHECK_EQ(yield_backoff_sleep_ns(8000000, 2000000, 10000000), 1000000);

    // The 10 us floor: 50 us of yields at a 10 us start is 6.25 us, raised to 10 us.
    CHECK_EQ(yield_backoff_sleep_ns(50000, 10000, 250000), 10000);

    // start 0 disables the policy (the kernel never passes 0; a guard for the parser).
    CHECK_EQ(yield_backoff_sleep_ns(1000000, 0, 250000), 0);

    // Progress: equal x19..x28 is a wait; any difference is work.
    uint64_t before[10] = {0}, after[10] = {0};
    CHECK(!yield_backoff_progress(before, after));
    after[0] = 1;
    CHECK(yield_backoff_progress(before, after));
    after[0] = 0;
    after[9] = 1;
    CHECK(yield_backoff_progress(before, after));
    TEST_END();
}
