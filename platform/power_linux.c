// Linux process power counters: no portable energy/wakeup counter, so [power] is skipped (like a kernel without
// perf events). The Deck profile can fill this in from RAPL/amdgpu later.
#include "platform.h"

bool plat_power_sample(PlatPower *out) {
    (void)out;
    return false;
}
