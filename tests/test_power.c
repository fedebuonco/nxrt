// Process power counters (platform/power_macos.c): a sample is monotonic; when the OS exposes energy it is too.
#include "harness.h"
#include "platform.h"

int main(void) {
    PlatPower a, b;
    if (!plat_power_sample(&a)) {
        printf("test_power: proc_pid_rusage unavailable; nothing to check\n");
        return 0;
    }
    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 2000000; i++) x += i;
    (void)x;
    CHECK(plat_power_sample(&b));
    CHECK(b.wakeups >= a.wakeups);
    CHECK(b.energy_nj >= a.energy_nj);
    TEST_END();
}
