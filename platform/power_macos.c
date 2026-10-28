// Process power counters (platform/platform.h): cumulative energy and wakeups from proc_pid_rusage. Kept apart from
// macos.c so a test can link it without the rest of the host layer.
#include "platform.h"

#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>

bool plat_power_sample(PlatPower *out) {
    if (!out) return false;
    out->energy_nj = 0;
    out->wakeups = 0;
    struct rusage_info_v6 v6;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&v6) == 0) {
        out->energy_nj = v6.ri_energy_nj;
        out->wakeups = v6.ri_pkg_idle_wkups + v6.ri_interrupt_wkups;
        return true;
    }
    // Older kernels: no energy field, but the wakeup counts exist in v4.
    struct rusage_info_v4 v4;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&v4) == 0) {
        out->wakeups = v4.ri_pkg_idle_wkups + v4.ri_interrupt_wkups;
        return true;
    }
    return false;
}
